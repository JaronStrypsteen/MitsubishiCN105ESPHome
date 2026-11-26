#include "cn105.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

using namespace esphome;

/**
 * This method is call by the esphome framework to initialize the component
 * We don't try to connect to the heater here because errors could not be logged fine because the
 * UART is used for communication with the heatpump
 * setupUART will handle the
*/
void CN105Climate::setup() {

    ESP_LOGD(TAG, "Component initialization: setup call");
    ESP_LOGI(TAG, "CN105 protocol profile: 0x%02X (%s)", this->profile_->id(), this->profile_->name());
    if (this->installer_mode_) this->profile_->allow_operation(cn105::ProfileFeature::INSTALLER_MODE);
    this->boot_ms_ = CUSTOM_MILLIS;
    this->current_temperature = NAN;
    this->target_temperature = NAN;
    this->target_temperature_low = NAN;
    this->target_temperature_high = NAN;
    this->fan_mode = climate::CLIMATE_FAN_OFF;
    this->clear_custom_fan_mode();
    this->swing_mode = climate::CLIMATE_SWING_OFF;
    if (this->coil_drying_switch_ != nullptr) {
        this->coil_drying_switch_->publish_state(this->coil_drying_enabled_);
    }
    if (this->coil_drying_sensor_ != nullptr) {
        this->coil_drying_sensor_->publish_state(false);
    }
    this->parser_.reset();
    this->lastResponseMs = CUSTOM_MILLIS;

    // initialize diagnostic stats
    this->nbCompleteCycles_ = 0;
    this->nbCycles_ = 0;
    this->nbHeatpumpConnections_ = 0;

    // Register info requests here to ensure all dependencies (like hardware_settings) are ready
    this->registerInfoRequests();

    ESP_LOGI(TAG, "tx_pin: %d rx_pin: %d", this->tx_pin_, this->rx_pin_);
    //ESP_LOGI(TAG, "remote_temp_timeout is set to %lu", this->remote_temp_timeout_);
    log_info_uint32(TAG, "remote_temp_timeout is set to ", this->remote_temp_timeout_);
    //ESP_LOGI(TAG, "debounce_delay is set to %lu", this->debounce_delay_);
    log_info_uint32(TAG, "debounce_delay is set to ", this->debounce_delay_);

    // IMPORTANT: do not initiate the UART/CN105 connection in setup().
    // We start the sequence in loop() to avoid missing the first OTA logs.

    // Initialize the internal flag based on the static configuration provided by YAML/Python
    this->supports_dual_setpoint_ = cn105_traits_requires_two_point(this->traits_);
    ESP_LOGI(TAG, "Dual setpoint support configured: %s", this->supports_dual_setpoint_ ? "YES" : "NO");
    ESP_LOGI(TAG, "Horizontal vanes configured: %d", this->horizontal_vanes_);
    this->log_capability_manifest_();

    if (this->night_mode_switch_ != nullptr) {
        this->warn_if_capability_denied_(ModelCapability::NIGHT_MODE, "night_mode_switch");
    }
    if (this->air_purifier_switch_ != nullptr) {
        this->warn_if_capability_denied_(ModelCapability::AIR_PURIFIER, "air_purifier_switch");
    }
    if (this->iSee_sensor_ != nullptr) {
        this->warn_if_capability_denied_(ModelCapability::ISEE, "isee_sensor");
    }
    if (this->airflow_control_select_ != nullptr) {
        this->warn_if_capability_denied_(ModelCapability::AIRFLOW_CONTROL, "airflow_control_select");
    }
    if (this->target_humidity_sensor_ != nullptr) {
        this->warn_if_capability_denied_(ModelCapability::TARGET_HUMIDITY, "target_humidity_sensor");
    }
    if (this->supports_dual_setpoint_) {
        this->warn_if_capability_denied_(ModelCapability::DUAL_SETPOINT, "supports.dual_setpoint");
    }
    if (!this->hardware_settings_.empty()) {
        this->warn_if_capability_denied_(ModelCapability::HARDWARE_SETTINGS_WRITES, "hardware_settings");
    }

    // Restore a previously-saved HEAT_COOL band (opt-in) before the first settings read,
    // so the profile reconciliation keeps HEAT_COOL instead of falling back to AUTO.
    this->restore_setpoint_state_();
}


/**
 * @brief Executes the main loop for the CN105Climate component.
 * This function is called repeatedly in the main program loop.
 */
void CN105Climate::loop() {
    // Bootstrap connection CN105 (UART + CONNECT) from loop()
    this->maybe_start_connection_();

    // Inform user about report_when_idle property if not configured, but only if compressor frequency sensor is configured.
    // Wait for UART to be ready so log messages appear via OTA update.
    if (!this->compressor_frequency_report_when_idle_warning_shown_ &&
        (this->compressor_frequency_sensor_ != nullptr) &&
        (this->isUARTReady_())) {
        ESP_LOGW("Decoder",
            "Compressor frequency sensor configured in YAML without report_when_idle property. "
            "It is recommended to set true for multi-head systems sharing an outdoor unit, "
            "false for models with a noisy readings such as PAA/PUZ.");
        this->compressor_frequency_report_when_idle_warning_shown_ = true;
    }

    // Do not let a busy RX stream starve pending writes/cycles. A frame may be
    // decoded here, but the write/cycle logic below still runs this iteration.
    this->processInput();

    // As long as the connection is not successful, we do not launch ANY cycle/write (otherwise it short-circuits the delay).
    if (!this->isHeatpumpConnected()) {
        return;
    }

    if ((this->wantedSettings.hasChanged) && (!this->loopCycle.isCycleRunning())) {
        this->checkPendingWantedSettings();
    } else if ((this->wantedRunStates.hasChanged) && (!this->loopCycle.isCycleRunning())) {
        this->checkPendingWantedRunStates();
    } else if ((this->isSetFunctions_) && (!this->loopCycle.isCycleRunning())) {
        this->isSetFunctions_ = false;
        if (!this->profile_->allow_operation(cn105::ProfileFeature::AUXILIARY_CONTROLS)) {
            this->isGetFunctions_ = false;
        } else {
            this->setFunctions(this->functions);
            // Also request to get function settings from heat pump to update UI with latest values.
            this->isGetFunctions_ = true;
        }
    } else {
        if (this->loopCycle.isCycleRunning()) {                         // if we are running an update cycle
            this->loopCycle.checkTimeout(this->update_interval_);
        } else { // we are not running a cycle
            if (this->loopCycle.hasUpdateIntervalPassed(this->get_update_interval())) {
                if (this->isGetFunctions_) {
                    if (this->profile_->allow_operation(cn105::ProfileFeature::AUXILIARY_CONTROLS)) {
                        // Reactivate requests 0x20/0x22 and bypass interval timers.
                        // This must be done before starting a new cycle to prevent a race hazard of
                        // request 0x22 occurring before request 0x20.
                        this->scheduler_.enable_request(0x20);
                        this->scheduler_.timer_bypass(0x20);
                        this->scheduler_.enable_request(0x22);
                        this->scheduler_.timer_bypass(0x22);
                    }
                    this->isGetFunctions_ = false;
                }
                this->buildAndSendRequestsInfoPackets();            // initiate an update cycle with this->cycleStarted();
            }
        }
    }
}

void CN105Climate::maybe_start_connection_() {
    switch (state_) {
        case DriverState::BOOT: {
            // Arm a 120s global timeout (fires once, forces connection even without WiFi)
            this->set_timeout("cn105_bootstrap_timeout", 120000, [this]() {
                if (state_ >= DriverState::CONNECTING) return;
                ESP_LOGW(LOG_CONN_TAG, "Bootstrap connexion: timeout 120s, démarrage CN105 malgré tout");
                this->setupUART();
                this->sendFirstConnectionPacket();
            });

#ifdef USE_WIFI
            if (wifi::global_wifi_component != nullptr && !wifi::global_wifi_component->is_connected()) {
                this->transition_to_(DriverState::WAIT_WIFI);
                ESP_LOGI(LOG_CONN_TAG, "Bootstrap connexion: attente WiFi avant init UART/CONNECT");
                return;
            }
#endif
            // WiFi ready (or no WiFi) — check grace delay
            this->transition_to_(DriverState::WAIT_GRACE);
            ESP_LOGI(LOG_CONN_TAG, "Bootstrap connexion: délai de grâce %" PRIu32 "ms pour logs OTA", this->conn_bootstrap_delay_ms_);
            return;
        }

        case DriverState::WAIT_WIFI: {
#ifdef USE_WIFI
            if (wifi::global_wifi_component != nullptr && !wifi::global_wifi_component->is_connected()) {
                return;  // still waiting
            }
#endif
            this->transition_to_(DriverState::WAIT_GRACE);
            ESP_LOGI(LOG_CONN_TAG, "Bootstrap connexion: WiFi connecté, délai de grâce %" PRIu32 "ms", this->conn_bootstrap_delay_ms_);
            return;
        }

        case DriverState::WAIT_GRACE: {
            const uint32_t elapsed = CUSTOM_MILLIS - this->boot_ms_;
            if (elapsed < this->conn_bootstrap_delay_ms_) {
                return;  // grace delay not elapsed yet
            }
            // If UART configuration is invalid, setupUART() leaves us here.
            // Retry on a slow cadence instead of re-entering setupUART() and
            // its blocking delay on every loop iteration.
            if (!this->isUARTReady_() && (CUSTOM_MILLIS - this->last_bootstrap_attempt_ms_) < 2000) {
                return;
            }
            this->last_bootstrap_attempt_ms_ = CUSTOM_MILLIS;
            ESP_LOGI(LOG_CONN_TAG, "Bootstrap connexion: init UART + envoi CONNECT (loop)");
            this->setupUART();
            if (this->isUARTReady_()) {
                this->sendFirstConnectionPacket();
            } else {
                ESP_LOGW(LOG_CONN_TAG, "UART not ready after setup; retrying bootstrap in 2s");
            }
            return;
        }

        case DriverState::CONNECTING:
        case DriverState::CONNECTED:
            // Nothing to do — connection already started or managed elsewhere
            return;

        case DriverState::DISCONNECTED: {
            // A failed reconnect must not spin. Retry UART setup on a slow cadence.
            if ((CUSTOM_MILLIS - this->last_bootstrap_attempt_ms_) < 2000) return;
            this->last_bootstrap_attempt_ms_ = CUSTOM_MILLIS;
            this->setupUART();
            if (this->isUARTReady_()) this->sendFirstConnectionPacket();
            return;
        }
    }
}

uint32_t CN105Climate::get_update_interval() const { return this->update_interval_; }
void CN105Climate::set_update_interval(uint32_t update_interval) {
    //ESP_LOGD(TAG, "Setting update interval to %lu", update_interval);
    log_debug_uint32(TAG, "Setting update interval to ", update_interval);

    this->update_interval_ = update_interval;
    this->autoUpdate = (update_interval != 0);
}
