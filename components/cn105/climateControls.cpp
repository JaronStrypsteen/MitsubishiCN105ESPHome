#include "cn105.h"
#include "Globals.h"
#include "heatpump_profile_protocol.h"

#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <map>
#include <vector>
#include <utility>

using namespace esphome;


void CN105Climate::checkPendingWantedSettings() {
    // Already in-flight — don't log or re-send
    if (this->wantedSettings.hasBeenSent) {
        return;
    }

    long now = CUSTOM_MILLIS;
    if (!(this->wantedSettings.hasChanged) || (now - this->wantedSettings.lastChange < this->debounce_delay_)) {
        return;
    }

    // Don't log if sendWantedSettings() will defer due to write throttle (300ms)
    if (now - this->lastSend <= 300) {
        return;
    }

    ESP_LOGI(LOG_ACTION_EVT_TAG, "checkPendingWantedSettings - wanted settings have changed, sending them to the heatpump...");
    this->sendWantedSettings();
}

void CN105Climate::checkPendingWantedRunStates() {
    // Already in-flight — don't log or re-send
    if (this->wantedRunStates.hasBeenSent) {
        return;
    }

    long now = CUSTOM_MILLIS;
    if (!(this->wantedRunStates.hasChanged) || (now - this->wantedRunStates.lastChange < this->debounce_delay_)) {
        return;
    }

    // Don't log if sendWantedRunStates() will defer due to write throttle (300ms)
    if (now - this->lastSend <= 300) {
        return;
    }

    ESP_LOGI(LOG_ACTION_EVT_TAG, "checkPendingWantedRunStates - wanted run states have changed, sending them to the heatpump...");
    this->sendWantedRunStates();
}

void logCheckWantedSettingsMutex(wantedHeatpumpSettings& settings) {

    if (settings.hasBeenSent) {
        ESP_LOGE("control", "Mutex lock faillure: wantedSettings should be locked while sending.");
        ESP_LOGD("control", "-- This is an assertion test on wantedSettings.hasBeenSent");
        ESP_LOGD("control", "-- wantedSettings.hasBeenSent = true is unexpected");
        ESP_LOGD("control", "-- should be false because mutex should prevent running this while sending");
        ESP_LOGD("control", "-- and mutex should be released only when hasBeenSent is false");
    }

}
void CN105Climate::controlDelegate(const esphome::climate::ClimateCall& call) {
    ESP_LOGD("control", "espHome control() interface method called...");
    bool updated = false;

    logCheckWantedSettingsMutex(this->wantedSettings);

    updated = this->processModeChange(call) || updated;
    updated = this->processTemperatureChange(call) || updated;
    updated = this->processPresetChange(call) || updated;
    updated = this->processFanChange(call) || updated;
    updated = this->processSwingChange(call) || updated;

    this->finalizeControlIfUpdated(updated);

    // Persist the (possibly new) mode + band so it can be restored after a reboot.
    if (updated) {
        this->save_setpoint_state_();
    }
}

bool CN105Climate::processModeChange(const esphome::climate::ClimateCall& call) {
    if (!call.get_mode().has_value()) {
        return false;
    }

    climate::ClimateMode target_mode = *call.get_mode();
    ESP_LOGD("control", "Mode change asked: %d (current mode: %d)", static_cast<int>(target_mode), static_cast<int>(this->mode));

    if (target_mode == climate::CLIMATE_MODE_OFF) {
        if (this->boost_active_) {
            ESP_LOGI("preset", "Turning OFF: cancelling active BOOST countdown");
            this->clearActiveBoost_();
        }
        if (this->away_active_) {
            this->away_active_ = false;
            this->preset = climate::CLIMATE_PRESET_NONE;
        }
    }

    // Handle coil drying / anti-mold post-cooling interception
    if (this->is_drying_cycle_) {
        if (target_mode == climate::CLIMATE_MODE_OFF) {
            ESP_LOGI("CoilDrying", "User forced OFF during drying cycle. Aborting anti-mold cycle.");
            this->cancel_coil_drying_cycle(false);
            this->mode = climate::CLIMATE_MODE_OFF;
            this->controlMode();
            return true;
        } else {
            ESP_LOGI("CoilDrying", "User resumed operation. Cancelling anti-mold drying cycle.");
            this->cancel_coil_drying_cycle(false);
        }
    } else if (target_mode == climate::CLIMATE_MODE_OFF && this->coil_drying_enabled_ &&
               (this->mode == climate::CLIMATE_MODE_COOL || this->mode == climate::CLIMATE_MODE_DRY)) {
        ESP_LOGI("CoilDrying", "Post-cooling anti-mold cycle starting: running fan for %" PRIu32 " seconds before power off", this->coil_drying_duration_s_);
        this->start_coil_drying_cycle();
        return true;
    }

    this->mode = target_mode;
    this->controlMode();
    // Do NOT call controlTemperature() here unconditionally.
    // A mode-only change (e.g. switching to DRY) should not emit a SET temperature
    // packet. If the call also includes a temperature change, processTemperatureChange()
    // will handle it. This prevents overwriting the user's last setpoint on mode changes.
    return true;
}

bool CN105Climate::processTemperatureChange(const esphome::climate::ClimateCall& call) {
    const bool updated = this->profile_->process_temperature_change(call);
    if (updated && !call.get_preset().has_value() && this->boost_active_) {
        ESP_LOGI("preset", "Manual temperature change during BOOST: cancelling BOOST timer");
        this->clearActiveBoost_();
    }
    return updated;
}

bool CN105Climate::processFanChange(const esphome::climate::ClimateCall& call) {
    if (cn105_custom_fan_mode_equals(call, "FULL")) {
        ESP_LOGD("control", "Custom fan mode FULL asked");
        this->set_custom_fan_mode("FULL");
        this->fan_mode.reset();
        this->setFanSpeed("4");
        if (!call.get_preset().has_value() && this->boost_active_) {
            this->clearActiveBoost_();
        }
        return true;
    }
    if (!call.get_fan_mode().has_value()) {
        return false;
    }
    ESP_LOGD("control", "Fan change asked");
    this->clear_custom_fan_mode();
    this->fan_mode = *call.get_fan_mode();
    this->controlFan();
    if (!call.get_preset().has_value() && this->boost_active_) {
        ESP_LOGI("preset", "Manual fan change during BOOST: cancelling BOOST timer");
        this->clearActiveBoost_();
    }
    return true;
}

bool CN105Climate::processSwingChange(const esphome::climate::ClimateCall& call) {
    if (!call.get_swing_mode().has_value()) {
        return false;
    }
    if (!this->profile_->allow_operation(cn105::ProfileFeature::SWING)) {
        return false;
    }
    ESP_LOGD("control", "Swing change asked");
    this->swing_mode = *call.get_swing_mode();
    this->controlSwing();
    return true;
}

void CN105Climate::finalizeControlIfUpdated(bool updated) {
    if (!updated) {
        return;
    }
    ESP_LOGD(LOG_ACTION_EVT_TAG, "clim.control() -> User changed something...");
    logCheckWantedSettingsMutex(this->wantedSettings);
    this->markWantedSettingsPending();
    this->debugSettings("control (wantedSettings)", this->wantedSettings);
    this->publish_state();
}

void CN105Climate::markWantedSettingsPending() {
    mark_wanted_settings_pending(this->wantedSettings, CUSTOM_MILLIS);
}

void CN105Climate::markWantedRunStatesPending() {
    mark_wanted_run_states_pending(this->wantedRunStates, CUSTOM_MILLIS);
}

void CN105Climate::control(const esphome::climate::ClimateCall& call) {

#ifdef USE_ESP32
    std::lock_guard<std::mutex> guard(wantedSettingsMutex);
    this->controlDelegate(call);
#else    
    this->emulateMutex("CONTROL_WANTED_SETTINGS", std::bind(&CN105Climate::controlDelegate, this, call));
#endif    

}


/**
 * @brief Controls the swing modes based on user selection.
 *
 * This function handles the logic for CLIMATE_SWING_OFF, VERTICAL, HORIZONTAL, and BOTH.
 * It is designed to be safe for units that do not support horizontal swing (wideVane)
 * and provides an intuitive user experience by preserving static vane settings when possible.
 */
void CN105Climate::controlSwing() {
    this->profile_->control_swing();
}
void CN105Climate::controlFan() {
    this->profile_->control_fan();
}


void CN105Climate::controlTemperature() {
    this->profile_->control_temperature();
}

void CN105Climate::controlMode() {
    this->profile_->control_mode();
}

/**
 * Thanks to Bascht74 on issu #9 we know that the compressor frequency is not a good indicator of the heatpump being in operation
 * Because one can have multiple inside module for a single compressor.
 * Also, some heatpump does not support compressor frequency.
 * SO usage is deprecated.
*/


//inside the below we could implement an internal only HEAT_COOL doing the math with an offset or something
void CN105Climate::updateAction() {
    this->profile_->update_action();
}

climate::ClimateTraits CN105Climate::traits() {
    //ESP_LOGD(LOG_SETTINGS_TAG, "traits() called (dual: %d)", cn105_traits_requires_two_point(traits_));
    return traits_;
}


/**
 * Modify our supported traits.
 *
 * Returns:
 *   A reference to this class' supported climate::ClimateTraits.
 */
climate::ClimateTraits& CN105Climate::config_traits() {
    return traits_;
}


void CN105Climate::setModeSetting(const char* setting) {
    const char* value = this->profile_->mode_setting(setting);
    if (value != nullptr) this->wantedSettings.mode = value;
}

void CN105Climate::setPowerSetting(const char* setting) {
    int index = lookupByteMapIndex(POWER_MAP, 2, setting);
    if (index > -1) {
        wantedSettings.power = POWER_MAP[index];
    } else {
        wantedSettings.power = POWER_MAP[0];
    }
}

void CN105Climate::setFanSpeed(const char* setting) {
    const char* value = this->profile_->fan_setting(setting);
    if (value != nullptr) this->wantedSettings.fan = value;
}

void CN105Climate::setVaneSetting(const char* setting) {
    if (this->profile_->allow_operation(cn105::ProfileFeature::SWING)) {
        this->profile_->set_vane_setting(setting);
    }
}

void CN105Climate::setWideVaneSetting(const char* setting) {
    if (this->profile_->allow_operation(cn105::ProfileFeature::SWING)) {
        this->profile_->set_wide_vane_setting(setting);
    }
}

void CN105Climate::setAirflowControlSetting(const char* setting) {
    if (this->profile_->allow_operation(cn105::ProfileFeature::AUXILIARY_CONTROLS)) {
        this->profile_->set_airflow_setting(setting);
    }
}

void CN105Climate::set_remote_temperature(float setting) {
    if (!this->profile_->accepts_remote_temperature(setting)) return;
    // Each accepted sample refreshes the watchdog, even if its value is unchanged (#474).
    setting = cn105::HeatPumpProtocol::normalize_remote_temperature(setting, this->fahrenheitSupport_);
    this->remoteTemperature_ = setting;
    this->shouldSendExternalTemperature_ = true;
    ESP_LOGD(LOG_REMOTE_TEMP, "setting remote temperature to %f", this->remoteTemperature_);
    this->pingExternalTemperature();
    if (setting > 0) this->startRemoteTempKeepAlive();
    else this->stopRemoteTempKeepAlive();
}

// --- HEAT_COOL band persistence (opt-in: supports.restore_setpoints) ----------------
// The dual-setpoint band is synthetic (a Mitsubishi unit stores only a single setpoint),
// so it is lost on reboot and the entity returns as hardware AUTO. When enabled, we keep
// {mode, low, high} in flash and re-seed it in setup() before the first settings read.

void CN105Climate::restore_setpoint_state_() {
    this->profile_->restore_setpoints();
}

void CN105Climate::save_setpoint_state_() {
    this->profile_->save_setpoints();
}

void CN105Climate::set_custom_fan_mode(const char* mode) {
    if (mode == nullptr) {
        this->clear_custom_fan_mode_();
    } else {
        this->set_custom_fan_mode_(mode);
    }
}

void CN105Climate::clear_custom_fan_mode() {
    this->clear_custom_fan_mode_();
}

bool CN105Climate::has_custom_fan_mode() const {
    return Climate::has_custom_fan_mode();
}

std::string CN105Climate::get_custom_fan_mode_str() const {
    return cn105_get_custom_fan_mode(*this);
}

void CN105Climate::set_supported_custom_fan_modes_config(const std::vector<std::string>& modes) {
    this->custom_fan_modes_storage_ = modes;
    std::vector<const char*> c_modes;
    c_modes.reserve(this->custom_fan_modes_storage_.size());
    for (const auto& m : this->custom_fan_modes_storage_) {
        c_modes.push_back(m.c_str());
    }
    cn105_set_supported_custom_fan_modes(*this, this->config_traits(), c_modes);
}

bool CN105Climate::processPresetChange(const esphome::climate::ClimateCall& call) {
    if (!call.get_preset().has_value()) {
        return false;
    }
    const climate::ClimatePreset new_preset = *call.get_preset();
    ESP_LOGD("control", "Preset change asked: %i", static_cast<int>(new_preset));
    this->change_preset_(new_preset);
    return true;
}

void CN105Climate::restorePreBoostState_() {
    if (!this->boost_active_) return;

    this->boost_active_ = false;
    this->cancel_timeout(SCHEDULER_BOOST_TIMEOUT);
    this->cancel_interval("boost_countdown");
    this->update_boost_remaining_(0);
    this->preset = climate::CLIMATE_PRESET_NONE;

    if (this->pre_boost_had_custom_fan_ && !this->pre_boost_fan_mode_.empty()) {
        this->set_custom_fan_mode(this->pre_boost_fan_mode_.c_str());
        this->setFanSpeed("4");
    } else {
        this->clear_custom_fan_mode();
        this->fan_mode = this->pre_boost_std_fan_mode_;
        this->controlFan();
    }

    if (cn105_traits_requires_two_point(this->traits_)) {
        if (!std::isnan(this->pre_boost_target_temperature_low_)) {
            this->setTargetTemperatureLow(this->pre_boost_target_temperature_low_);
        }
        if (!std::isnan(this->pre_boost_target_temperature_high_)) {
            this->setTargetTemperatureHigh(this->pre_boost_target_temperature_high_);
        }
    }
    if (!std::isnan(this->pre_boost_target_temperature_)) {
        this->setTargetTemperature(this->pre_boost_target_temperature_);
    }
    this->controlTemperature();

    // The timeout path does not go through control(), so mark the restore dirty
    // or checkPendingWantedSettings() will never send it to the indoor unit.
    this->markWantedSettingsPending();
    this->publish_state();
}

void CN105Climate::update_boost_remaining_(uint32_t remaining_s) {
    if (this->boost_remaining_sensor_ != nullptr) {
        this->boost_remaining_sensor_->publish_state(static_cast<float>(remaining_s));
    }
    if (this->boost_remaining_time_sensor_ != nullptr) {
        if (remaining_s == 0) {
            this->boost_remaining_time_sensor_->publish_state("Inactive");
        } else {
            uint32_t mins = remaining_s / 60;
            uint32_t secs = remaining_s % 60;
            char buf[32];
            snprintf(buf, sizeof(buf), "%" PRIu32 "m %02" PRIu32 "s", mins, secs);
            this->boost_remaining_time_sensor_->publish_state(buf);
        }
    }
}

void CN105Climate::restart_boost_countdown_() {
    // Named char* timers use ESPHome's skip-cancel path. Cancel explicitly so
    // reactivating BOOST or changing its duration cannot leave an old timeout
    // or countdown interval running.
    this->cancel_timeout(SCHEDULER_BOOST_TIMEOUT);
    this->cancel_interval("boost_countdown");

    this->boost_start_time_ms_ = CUSTOM_MILLIS;
    this->update_boost_remaining_(this->boost_duration_ms_ / 1000);

    this->set_interval("boost_countdown", 1000, [this]() {
        if (!this->boost_active_) {
            this->cancel_interval("boost_countdown");
            this->update_boost_remaining_(0);
            return;
        }
        uint32_t elapsed_ms = CUSTOM_MILLIS - this->boost_start_time_ms_;
        uint32_t rem_s = (elapsed_ms < this->boost_duration_ms_) ?
            ((this->boost_duration_ms_ - elapsed_ms) / 1000) : 0;
        this->update_boost_remaining_(rem_s);
    });

    this->set_timeout(SCHEDULER_BOOST_TIMEOUT, this->boost_duration_ms_, [this]() {
        ESP_LOGI("preset", "BOOST mode expired, restoring previous settings");
        this->restorePreBoostState_();
    });
}

void CN105Climate::clearActiveBoost_() {
    if (!this->boost_active_) return;
    this->boost_active_ = false;
    this->cancel_timeout(SCHEDULER_BOOST_TIMEOUT);
    this->cancel_interval("boost_countdown");
    this->update_boost_remaining_(0);
    this->preset = climate::CLIMATE_PRESET_NONE;
}

void CN105Climate::restorePreAwayState_() {
    if (!this->away_active_) return;
    this->away_active_ = false;
    ESP_LOGI("preset", "Restoring pre-AWAY mode and setpoint");

    this->mode = this->pre_away_mode_;
    this->controlMode();

    if (this->pre_away_mode_ != climate::CLIMATE_MODE_OFF) {
        if (cn105_traits_requires_two_point(this->traits_)) {
            if (!std::isnan(this->pre_away_target_temperature_low_)) {
                this->setTargetTemperatureLow(this->pre_away_target_temperature_low_);
            }
            if (!std::isnan(this->pre_away_target_temperature_high_)) {
                this->setTargetTemperatureHigh(this->pre_away_target_temperature_high_);
            }
        }
        if (!std::isnan(this->pre_away_target_temperature_)) {
            this->setTargetTemperature(this->pre_away_target_temperature_);
        }
        this->controlTemperature();
    }
    this->save_setpoint_state_();
    this->markWantedSettingsPending();
    this->publish_state();
}

void CN105Climate::change_preset_(climate::ClimatePreset preset) {
    // Leaving BOOST must restore the saved fan/setpoint, not merely stop the timer.
    if (preset != climate::CLIMATE_PRESET_BOOST && this->boost_active_) {
        this->restorePreBoostState_();
    }
    if (preset != climate::CLIMATE_PRESET_AWAY && this->away_active_) {
        this->restorePreAwayState_();
    }

    this->preset = preset;

    if (preset == climate::CLIMATE_PRESET_BOOST) {
        if (!this->boost_active_) {
            this->boost_active_ = true;
            this->pre_boost_target_temperature_ = this->target_temperature;
            this->pre_boost_target_temperature_low_ = this->target_temperature_low;
            this->pre_boost_target_temperature_high_ = this->target_temperature_high;
            this->pre_boost_had_custom_fan_ = this->has_custom_fan_mode();
            if (this->pre_boost_had_custom_fan_) {
                this->pre_boost_fan_mode_ = this->get_custom_fan_mode_str();
            } else if (this->fan_mode.has_value()) {
                this->pre_boost_std_fan_mode_ = this->fan_mode.value();
            }
        }

        if (this->mode == climate::CLIMATE_MODE_OFF) {
            if (this->currentSettings.mode != nullptr) {
                if (std::strcmp(this->currentSettings.mode, "HEAT") == 0) {
                    this->mode = climate::CLIMATE_MODE_HEAT;
                } else if (std::strcmp(this->currentSettings.mode, "COOL") == 0) {
                    this->mode = climate::CLIMATE_MODE_COOL;
                } else if (std::strcmp(this->currentSettings.mode, "DRY") == 0) {
                    this->mode = climate::CLIMATE_MODE_DRY;
                } else if (std::strcmp(this->currentSettings.mode, "FAN") == 0) {
                    this->mode = climate::CLIMATE_MODE_FAN_ONLY;
                } else {
                    this->mode = climate::CLIMATE_MODE_AUTO;
                }
            } else {
                this->mode = climate::CLIMATE_MODE_AUTO;
            }
            this->controlMode();
        }

        // Force maximum heating in HEAT mode (or maximum cooling in COOL mode)
        // so the compressor ramps to full capacity rather than blowing lukewarm/cold air.
        if (this->mode == climate::CLIMATE_MODE_HEAT) {
            float max_temp = this->traits_.get_visual_max_temperature();
            this->setTargetTemperature(max_temp);
            if (cn105_traits_requires_two_point(this->traits_)) {
                this->setTargetTemperatureLow(max_temp);
            }
            this->controlTemperature();
        } else if (this->mode == climate::CLIMATE_MODE_COOL) {
            float min_temp = this->traits_.get_visual_min_temperature();
            this->setTargetTemperature(min_temp);
            if (cn105_traits_requires_two_point(this->traits_)) {
                this->setTargetTemperatureHigh(min_temp);
            }
            this->controlTemperature();
        }

        if (this->boost_duration_ms_ >= 60000) {
            ESP_LOGI("preset", "Activating BOOST mode (%u min full capacity + Super High fan)...",
                     static_cast<unsigned>(this->boost_duration_ms_ / 60000));
        } else {
            ESP_LOGI("preset", "Activating BOOST mode (%u s full capacity + Super High fan)...",
                     static_cast<unsigned>(this->boost_duration_ms_ / 1000));
        }
        this->set_custom_fan_mode("FULL");
        this->fan_mode.reset();
        this->setFanSpeed("4");

        this->restart_boost_countdown_();
    } else {
        if (preset == climate::CLIMATE_PRESET_SLEEP) {
            ESP_LOGI("preset", "Activating SLEEP mode (Quiet fan + Night Mode)");
            this->clear_custom_fan_mode();
            this->fan_mode = climate::CLIMATE_FAN_QUIET;
            this->controlFan();
            if (this->night_mode_switch_ != nullptr) {
                this->wantedRunStates.night_mode = 1;
                this->markWantedRunStatesPending();
                this->night_mode_switch_->publish_state(true);
            }
        } else if (preset == climate::CLIMATE_PRESET_ECO) {
            ESP_LOGI("preset", "Activating ECO mode");
            this->clear_custom_fan_mode();
            this->fan_mode = climate::CLIMATE_FAN_LOW;
            this->controlFan();
            if (this->night_mode_switch_ != nullptr &&
                (this->currentRunStates.night_mode == 1 || this->wantedRunStates.night_mode == 1)) {
                this->wantedRunStates.night_mode = 0;
                this->markWantedRunStatesPending();
                this->night_mode_switch_->publish_state(false);
            }
        } else if (preset == climate::CLIMATE_PRESET_AWAY) {
            if (!this->away_active_) {
                this->away_active_ = true;
                this->pre_away_mode_ = this->mode;
                this->pre_away_target_temperature_ = this->target_temperature;
                this->pre_away_target_temperature_low_ = this->target_temperature_low;
                this->pre_away_target_temperature_high_ = this->target_temperature_high;
            }
            ESP_LOGI("preset", "Activating AWAY mode (%.1f°C Freeze Protection)", this->away_temperature_);
            this->mode = climate::CLIMATE_MODE_HEAT;
            this->controlMode();
            this->setTargetTemperature(this->away_temperature_);
            if (cn105_traits_requires_two_point(this->traits_)) {
                this->setTargetTemperatureLow(this->away_temperature_);
            }
            this->controlTemperature();
        } else if (preset == climate::CLIMATE_PRESET_NONE) {
            ESP_LOGI("preset", "Clearing preset");
            if (this->night_mode_switch_ != nullptr &&
                (this->currentRunStates.night_mode == 1 || this->wantedRunStates.night_mode == 1)) {
                this->wantedRunStates.night_mode = 0;
                this->markWantedRunStatesPending();
                this->night_mode_switch_->publish_state(false);
            }
        }
    }
}
