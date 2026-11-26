#include "cn105.h"

#include <cmath>
#include <map>

using namespace esphome;

/**
 * processInput: reads available bytes from UART and feeds them to the FrameParser.
 * When a complete frame is detected, delegates to processDataPacket().
 */


bool CN105Climate::processInput(void) {
    // Return true only when a complete frame was processed. Merely consuming
    // bytes from a partial/noisy stream must not starve pending writes/cycles.
    bool processed_frame = false;
    while (this->get_hw_serial_()->available()) {
        uint8_t inputData;
        if (this->get_hw_serial_()->read_byte(&inputData)) {
            ESP_LOGV("Decoder", "--> %02X", inputData);
            this->parser_.feed(inputData);
            if (this->parser_.frame_complete()) {
                this->processDataPacket();
                this->parser_.reset();
                processed_frame = true;
            }
        }
    }
    return processed_frame;
}

/**
 * processDataPacket: called when the FrameParser has assembled a complete frame.
 * Validates checksum, sets the data pointer, and dispatches to processCommand().
 */
void CN105Climate::processDataPacket() {

    ESP_LOGV(TAG, "processing data packet...");

    // Point data at the payload section of the parser buffer
    // Note: cast away const because downstream code uses non-const data pointer
    this->data = const_cast<uint8_t*>(this->parser_.data());

    this->hpPacketDebug(this->parser_.raw(), this->parser_.frame_size(), "READ");

    // During handshake, log every received frame for diagnostics
    if (!this->isHeatpumpConnected()) {
        ESP_LOGD(LOG_CONN_TAG, "RX during handshake (cmd=0x%02X len=%d)",
            this->parser_.command(), this->parser_.data_length());
        this->hpPacketDebug(this->parser_.raw(), this->parser_.frame_size(), LOG_CONN_TAG);
    }

    if (this->parser_.checksum_valid()) {
        ESP_LOGD("chkSum", "OK");
        // `lastResponseMs` is a connection heartbeat, not an "any byte arrived"
        // timestamp. Only awaited responses, ACKs and handshakes refresh it.
        processCommand();
    } else {
        this->increment_checksum_errors();
        ESP_LOGW("chkSum", "KO -> checksum mismatch (cmd=0x%02X len=%d received=0x%02X expected=0x%02X)",
            this->parser_.command(), this->parser_.data_length(),
            this->parser_.received_checksum(), this->parser_.expected_checksum());
        if (!this->isHeatpumpConnected()) {
            ESP_LOGD(LOG_CONN_TAG, "Checksum KO during handshake");
            this->hpPacketDebug(this->parser_.raw(), this->parser_.frame_size(), LOG_CONN_TAG);
        }
    }
}

void CN105Climate::getPowerFromResponsePacket() {
    if (this->profile_->decode_submode()) this->publish_state();
}

void CN105Climate::getSettingsFromResponsePacket() {
    this->profile_->decode_settings();
}

void CN105Climate::getRoomTemperatureFromResponsePacket() {
    auto receivedStatus = this->profile_->decode_room_status(this->data, this->currentStatus);

    // Update the remote temperature control sensor (Issue 290)
    if (this->remote_temp_sensor_ != nullptr) {
        bool is_remote = false;
        if (this->remote_temp_keepalive_active_ && this->remoteTemperature_ > 0 &&
            std::isfinite(receivedStatus.roomTemperature) && std::isfinite(this->remoteTemperature_)) {
            float diff = std::fabs(receivedStatus.roomTemperature - this->remoteTemperature_);
            if (diff <= this->remote_temp_margin_) {
                is_remote = true;
            }
        }
        this->remote_temp_sensor_->publish_state(is_remote);
    }

    ESP_LOGD("Decoder", "[Room °C: %f]", receivedStatus.roomTemperature);
    ESP_LOGD("Decoder", "[OAT  °C: %f]", receivedStatus.outsideAirTemperature);
    this->statusChanged(receivedStatus);
}

void CN105Climate::getOperatingAndCompressorFreqFromResponsePacket() {
    this->profile_->decode_status();
}

void CN105Climate::getHVACOptionsFromResponsePacket() {
    this->profile_->decode_hvac_options();
}

void CN105Climate::terminateCycle() {
    if (this->shouldSendExternalTemperature_) {
        // We will receive ACK packet for this.
        // Sending WantedSettings must be delayed in this case (lastSend timestamp updated).
        ESP_LOGD(LOG_REMOTE_TEMP, "Sending remote temperature...");
        this->sendRemoteTemperature();
    }

    this->loopCycle.cycleEnded();

    this->nbCompleteCycles_++;
}
void CN105Climate::getErrorInfoFromResponsePacket() {
    if (this->error_code_sensor_ == nullptr) {
        return;
    }
    std::string error = this->profile_->decode_error(this->data);
    if (!this->error_code_sensor_->has_state() || this->error_code_sensor_->state != error) {
        this->error_code_sensor_->publish_state(error);
    }
}

bool CN105Climate::getDataFromResponsePacket() {

    // Known late/unsolicited frames are decoded but do not advance the cycle
    // or count as a fresh connection heartbeat.
    const uint8_t code = this->data[0];
    const auto result = this->scheduler_.process_response(code);
    if (result == RequestScheduler::ResponseResult::AWAITED) {
        return true;
    }
    if (result == RequestScheduler::ResponseResult::LATE) {
        return false;
    }

    // Sinon, switch pour les cas non gérés par l'orchestrateur
    switch (code) {

    case 0x04:
        // Handled by orchestrator (r_error_info onResponse → getErrorInfoFromResponsePacket)
        // Reaching here means the scheduler did not intercept this response — unexpected
        ESP_LOGW("Decoder", "[0x04] reached switch fallback — should have been handled by orchestrator");
        break; // orchestrator

    case 0x05:
        /* timer packet */
        if (this->parser_.data_length() >= 4) {
            uint8_t mode_byte = this->data[1];
            const char* mode_str = (mode_byte < 4) ? TIMER_MODE_MAP[mode_byte] : "UNKNOWN";
            if (this->timer_mode_sensor_ != nullptr) {
                this->timer_mode_sensor_->publish_state(mode_str);
            }
            float on_minutes = static_cast<float>(this->data[2] * TIMER_INCREMENT_MINUTES);
            if (this->on_timer_sensor_ != nullptr) {
                this->on_timer_sensor_->publish_state(on_minutes);
            }
            float off_minutes = static_cast<float>(this->data[3] * TIMER_INCREMENT_MINUTES);
            if (this->off_timer_sensor_ != nullptr) {
                this->off_timer_sensor_->publish_state(off_minutes);
            }
            ESP_LOGD("Decoder", "[0x05 Timer: mode=%s, on=%.0fm, off=%.0fm]", mode_str, on_minutes, off_minutes);
        }
        break;

    case 0x06:
        break; // orchestrator
    case 0x09:
        break; // orchestrator

    case 0x10:
        if (this->parser_.data_length() >= 2) {
            bool filter_dirty = (this->data[1] & 0x01) != 0;
            if (this->filter_dirty_sensor_ != nullptr) {
                this->filter_dirty_sensor_->publish_state(filter_dirty);
            }
            ESP_LOGD("Decoder", "[0x10 Maintenance: filter_dirty=%s]", filter_dirty ? "YES" : "NO");
        }
        break;

    case 0x20: // fallthrough
    case 0x22:
        break; // orchestrator

    case 0x42:
        break; // orchestrator

    default:
        ESP_LOGW("Decoder", "packet type [%02X] <-- unknown and unexpected", data[0]);
        //this->last_received_packet_sensor->publish_state("0x62-> ?? : Data -> Unknown");
        break;
    }

    return false;
}

void CN105Climate::updateSuccess() {
    ESP_LOGD(LOG_ACK, "Last heatpump data update successful!");
    // nothing can be done here because we have no mean to know wether it is an external temp ack
    // or a wantedSettings update ack
}

void CN105Climate::processCommand() {
    switch (this->parser_.command()) {
    case 0x61:  /* last update was successful */
        this->hpPacketDebug(this->parser_.raw(), this->parser_.frame_size(), LOG_ACK);
        this->lastResponseMs = CUSTOM_MILLIS;
        this->updateSuccess();
        break;

    case 0x62: {  /* packet contains data (room °C, settings, timer, status, or functions...)*/
        const int payload_len = this->parser_.data_length();
        if (payload_len < 1) {
            ESP_LOGW("Decoder", "Ignoring empty 0x62 packet");
            break;
        }

        // Decoders read fixed offsets. Reject checksum-valid but truncated frames
        // before they can consume stale bytes left in the parser buffer.
        const uint8_t code = this->data[0];
        int min_len = 0;
        switch (code) {
        case 0x02: min_len = 15; break;  // settings, up to data[14]
        case 0x03: min_len = 14; break;  // room/OAT/runtime, up to data[13]
        case 0x04: min_len = 6;  break;  // error code, data[4] and data[5]
        case 0x05: min_len = 4;  break;  // timers, mode and on/off intervals
        case 0x06: min_len = 9;  break;  // status, up to data[8]
        case 0x09: min_len = 6;  break;  // sub-mode, up to data[5]
        case 0x10: min_len = 2;  break;  // maintenance / filter status
        case 0x20:                   // fallthrough
        case 0x22: min_len = 16; break;  // function settings
        case 0x42: min_len = 4;  break;  // HVAC options, up to data[3]
        default: break;                   // unknown codes are still logged downstream
        }
        if (payload_len < min_len) {
            ESP_LOGW("Decoder", "Ignoring short 0x62 payload 0x%02X: got %d bytes, need >=%d",
                     code, payload_len, min_len);
            break;
        }

        if (this->getDataFromResponsePacket()) {
            this->lastResponseMs = CUSTOM_MILLIS;
        }
        break;
    }
    case 0x7a:  // Standard handshake
    case 0x7b:  // Installer handshake
        if (this->profile_->accepts_handshake(this->parser_.command(), this->parser_.raw()[3])) {
            this->handleConnectionSuccess();
        } else {
            ESP_LOGW(LOG_CONN_TAG, "Ignoring unexpected handshake response for %s", this->profile_->name());
        }
        break;
    default:
        break;
    }
}

void CN105Climate::handleConnectionSuccess() {
    const bool installer = this->parser_.command() == 0x7b;
    ESP_LOGI(
        LOG_CONN_TAG,
        "--> %s did reply: connection success (%s, 0x%02X)! <--",
        this->profile_->name(),
        installer ? "Installer" : "User",
        this->parser_.command()
    );
    this->hpPacketDebug(this->parser_.raw(), this->parser_.frame_size(), LOG_CONN_TAG);
    this->lastResponseMs = CUSTOM_MILLIS;
    this->setHeatpumpConnected(true);
    this->loopCycle.lastCompleteCycleMs = CUSTOM_MILLIS;
    // Reset cached settings so the first read after reconnect performs a full sync.
    this->currentSettings.resetSettings();
    this->currentRunStates.resetSettings();
    this->profile_->reset();
}


void CN105Climate::statusChanged(heatpumpStatus status) {
    if (status != currentStatus) {
        this->debugStatus("received", status);
        this->debugStatus("current", currentStatus);
        cn105::CommonProtocol::apply_status(*this, status);
    } // else no change
}


void CN105Climate::publishStateToHA(heatpumpSettings& settings) {
    this->profile_->apply_received_settings(settings);
    this->publish_state();
}

void CN105Climate::heatpumpUpdate(heatpumpSettings& settings) {
    // settings correponds to current settings
    ESP_LOGV(LOG_SETTINGS_TAG, "Settings received");
    // if received settings are different from current settings
    if (settings != this->currentSettings) {
        ESP_LOGI(LOG_SETTINGS_TAG, "Settings changed, updating HA states");
        this->debugSettings("current", this->currentSettings);
        this->debugSettings("received", settings);
        this->debugSettings("wanted", this->wantedSettings);
        this->debugClimate("climate");
        this->publishStateToHA(settings);
    }

}
