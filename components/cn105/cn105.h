#pragma once
#include "Globals.h"
#include "esphome_compat.h"
#include "cn105_protocol.h"
#include "protocol_profile.h"
#include "frame_parser.h"
#include "esphome/components/uart/uart.h"
#include "heatpumpFunctions.h"
#include "van_orientation_select.h"
#include "boost_duration_select.h"
#include "uptime_connection_sensor.h"
#include "compressor_frequency_sensor.h"
#include "target_humidity_sensor.h"
#include "input_power_sensor.h"
#include "kwh_sensor.h"
#include "runtime_hours_sensor.h"
#include "outside_air_temperature_sensor.h"
#include "auto_sub_mode_sensor.h"
#include "isee_sensor.h"
#include "stage_sensor.h"
#include "functions_sensor.h"
#include "functions_number.h"
#include "functions_button.h"
#include "sub_mode_sensor.h"
#include "error_code_sensor.h"
#include "remote_temp_source_info.h"
#include "hvac_option_switch.h"
#include "hardware_setting_select.h"
#include "localization.h"
#include "info_request.h"
#include "request_scheduler.h"
#include <esphome/components/sensor/sensor.h>
#include <esphome/components/button/button.h>
#include <esphome/components/binary_sensor/binary_sensor.h>
#include "esphome/core/preferences.h"
#include "cycle_management.h"
#include <vector>
#include <map>

#ifdef USE_ESP32
#include <mutex>
#endif

namespace esphome {
    namespace cn105 { class HeatPumpProfile; class LossnayProfile; }

    // Connection lifecycle FSM — replaces 6 scattered booleans
    enum class DriverState : uint8_t {
        BOOT,           // setup() done, loop() not yet called
        WAIT_WIFI,      // WiFi required but not yet connected
        WAIT_GRACE,     // WiFi OK, OTA grace delay in progress
        CONNECTING,     // UART configured, CONNECT sent, awaiting 0x7A/0x7B
        CONNECTED,      // Handshake succeeded, ready to poll
        DISCONNECTED,   // Response timeout, reconnection needed
    };

    const char* driver_state_to_str(DriverState s);

    // Deferred writes keep their completion action, so a queued SET can still
    // publish optimistic state and clear its pending flags after it succeeds.
    enum class PendingWriteKind : uint8_t {
        GENERAL,
        SETTINGS,
        RUN_STATES,
        REMOTE_TEMP,
    };

    struct PendingWrite {
        uint8_t bytes[PACKET_LEN]{};
        uint8_t length = 0;
        bool check_is_active = true;
        PendingWriteKind kind = PendingWriteKind::GENERAL;
        float value = 0.0f;  // remote temperature payload, when applicable
        uint32_t generation = 0;  // wantedSettings/wantedRunStates change token
    };

    // Exact-model capability manifest. Bits are mirrored by climate.py; keep the
    // values stable because generated firmware stores them as a uint16_t.
    enum class ModelCapability : uint16_t {
        NIGHT_MODE = 1u << 0,
        BOOST = 1u << 1,
        SLEEP = 1u << 2,
        ECO = 1u << 3,
        AIR_PURIFIER = 1u << 4,
        ISEE = 1u << 5,
        AIRFLOW_CONTROL = 1u << 6,
        TARGET_HUMIDITY = 1u << 7,
        DUAL_SETPOINT = 1u << 8,
        HARDWARE_SETTINGS_WRITES = 1u << 9,
    };

    static constexpr uint8_t MAX_PENDING_WRITES = 4;

    void log_info_uint32(const char* tag, const char* msg, uint32_t value, const char* suffix = "");
    void log_debug_uint32(const char* tag, const char* msg, uint32_t value, const char* suffix = "");

    inline void publish_binary_if_changed(binary_sensor::BinarySensor* sensor, bool value) {
        if (sensor != nullptr && (!sensor->has_state() || sensor->state != value)) {
            sensor->publish_state(value);
        }
    }

    inline void publish_sensor_if_changed(sensor::Sensor* sensor, float value) {
        if (sensor != nullptr && (!sensor->has_state() || !cn105_float_equal(sensor->get_raw_state(), value))) {
            sensor->publish_state(value);
        }
    }

    inline void publish_text_if_changed(text_sensor::TextSensor* sensor, const std::string& value) {
        if (sensor != nullptr && (!sensor->has_state() || sensor->state != value)) {
            sensor->publish_state(value);
        }
    }

    class CN105Climate : public climate::Climate, public Component, public esphome::uart::UARTDevice {

        //friend class VaneOrientationSelect;

    public:

        CN105Climate(uart::UARTComponent* hw_serial, bool lossnay = false);

        // Expose the existing polling-cycle state to ESPHome lambda conditions.
        bool isCycleRunning() { return this->loopCycle.isCycleRunning(); }

        enum class VaneType {
            STANDARD = 0,
            SPLIT_HORIZONTAL = 1,
            SPLIT_VERTICAL = 2
        };

        void set_vertical_vane_select(VaneOrientationSelect* vertical_vane_select);
        void set_horizontal_vane_select(VaneOrientationSelect* horizontal_vane_select, const std::vector<std::string>& options = {});
        void set_airflow_control_select(VaneOrientationSelect* airflow_control_select);
        void set_boost_duration_select(BoostDurationSelect* boost_duration_select,
                                       const std::vector<std::string>& options,
                                       const std::string& initial_option);
        void set_compressor_frequency_sensor(esphome::sensor::Sensor* compressor_frequency_sensor);
        void set_compressor_frequency_report_when_idle(bool value);
        void set_target_humidity_sensor(esphome::sensor::Sensor* target_humidity_sensor);
        void set_input_power_sensor(esphome::sensor::Sensor* input_power_sensor);
        void set_kwh_sensor(esphome::sensor::Sensor* kwh_sensor);
        void set_runtime_hours_sensor(esphome::sensor::Sensor* runtime_hours_sensor);
        void set_outside_air_temperature_sensor(esphome::sensor::Sensor* outside_air_temperature_sensor);
        void set_isee_sensor(esphome::binary_sensor::BinarySensor* iSee_sensor);
        void set_stage_sensor(esphome::text_sensor::TextSensor* Stage_sensor);
        void set_use_stage_for_operating_status(bool value);
        void set_use_fahrenheit_support_mode(FahrenheitMode mode);
        void set_msz_a24na_setpoint_table(bool value);
        void set_air_purifier_switch(HVACOptionSwitch* air_purifier_switch);
        void set_night_mode_switch(HVACOptionSwitch* night_mode_switch);
        void set_circulator_switch(HVACOptionSwitch* circulator_switch);
        void set_power_saving_switch(HVACOptionSwitch* power_saving_switch);
        void set_coil_drying_switch(HVACOptionSwitch* coil_drying_switch);
        void set_coil_drying_sensor(binary_sensor::BinarySensor* sensor) { this->coil_drying_sensor_ = sensor; }
        void set_coil_drying_duration(uint32_t seconds) { this->coil_drying_duration_s_ = seconds; }
        void set_coil_drying_enabled(bool enabled) { this->coil_drying_enabled_ = enabled; }
        void set_coil_drying_fan_speed(const std::string& speed) { this->coil_drying_fan_speed_ = speed; }
        void set_coil_drying_vane(const std::string& vane) { this->coil_drying_vane_ = vane; }

        void set_timer_mode_sensor(text_sensor::TextSensor* s) { this->timer_mode_sensor_ = s; }
        void set_on_timer_sensor(sensor::Sensor* s) { this->on_timer_sensor_ = s; }
        void set_off_timer_sensor(sensor::Sensor* s) { this->off_timer_sensor_ = s; }
        void set_filter_dirty_sensor(binary_sensor::BinarySensor* s) { this->filter_dirty_sensor_ = s; }

        void set_checksum_error_sensor(sensor::Sensor* s) { this->checksum_error_sensor_ = s; }
        void set_packet_drop_sensor(sensor::Sensor* s) { this->packet_drop_sensor_ = s; }
        void set_retransmission_sensor(sensor::Sensor* s) { this->retransmission_sensor_ = s; }
        void set_connection_sensor(binary_sensor::BinarySensor* s) { this->connection_sensor_ = s; }
        void increment_checksum_errors();
        void increment_packet_drops();
        void increment_retransmissions();

        unsigned long get_complete_cycles() const { return this->nbCompleteCycles_; }
        unsigned long get_total_cycles() const { return this->nbCycles_; }
        unsigned int get_heatpump_connections() const { return this->nbHeatpumpConnections_; }
        uint32_t get_checksum_errors() const { return this->checksum_error_count_; }
        uint32_t get_packet_drops() const { return this->packet_drop_count_; }
        uint32_t get_retransmissions() const { return this->retransmission_count_; }

        void set_level_shifter_enable_pin(GPIOPin* pin) { this->level_shifter_enable_pin_ = pin; }
        void set_native_five_speed_fan(bool enabled) { this->native_five_speed_fan_ = enabled; }
        bool is_native_five_speed_fan() const { return this->native_five_speed_fan_; }
        void set_boost_remaining_sensor(sensor::Sensor* s) { this->boost_remaining_sensor_ = s; }
        void set_boost_remaining_time_sensor(text_sensor::TextSensor* s) { this->boost_remaining_time_sensor_ = s; }
        void update_boost_remaining_(uint32_t remaining_s);
        void set_boost_duration_runtime(uint32_t duration_ms);
        void restart_boost_countdown_();
        void restorePreAwayState_();

        void add_hardware_setting(HardwareSettingSelect* setting);
        void set_hardware_settings_interval(uint32_t interval_ms) { this->hardware_settings_interval_ms_ = interval_ms; }

        // Deprecated: kept for backward compatibility, will map to VaneType::SPLIT_HORIZONTAL
        void set_horizontal_vanes(int horizontal_vanes) {
            if (horizontal_vanes > 1) {
                this->vane_type_ = VaneType::SPLIT_HORIZONTAL;
            }
        }
        void set_vane_type(VaneType type) { this->vane_type_ = type; }

        void set_functions_sensor(esphome::text_sensor::TextSensor* Functions_sensor);
        void set_functions_get_button(FunctionsButton* Button);
        void set_functions_set_button(FunctionsButton* Button);
        void set_functions_set_code(FunctionsNumber* Number);
        void set_functions_set_value(FunctionsNumber* Number);

        void set_sub_mode_sensor(esphome::text_sensor::TextSensor* Sub_mode_sensor);
        void set_auto_sub_mode_sensor(esphome::text_sensor::TextSensor* Auto_sub_mode_sensor);
        void set_error_code_sensor(esphome::text_sensor::TextSensor* error_code_sensor);
        void set_remote_temp_source(esphome::sensor::Sensor* source);
        void set_remote_temp_source_info_sensor(esphome::text_sensor::TextSensor* info_sensor);
        void set_hp_uptime_connection_sensor(cn105::HpUpTimeConnectionSensor* hp_up_connection_sensor);

        void set_remote_temperature_control_sensor(esphome::binary_sensor::BinarySensor* sensor);
        void set_remote_temperature_margin(float margin);

        //sensor::Sensor* compressor_frequency_sensor;
        binary_sensor::BinarySensor* iSee_sensor_ = nullptr;
        binary_sensor::BinarySensor* remote_temp_sensor_ = nullptr;
        float remote_temp_margin_ = 0.4f;
        text_sensor::TextSensor* stage_sensor_{ nullptr }; // to save ref if needed
        bool use_stage_for_operating_status_{ false };
        FahrenheitSupport fahrenheitSupport_;
        text_sensor::TextSensor* Functions_sensor_ = nullptr;
        FunctionsButton* Functions_get_button_ = nullptr;
        FunctionsButton* Functions_set_button_ = nullptr;
        FunctionsNumber* Functions_set_code_ = nullptr;
        FunctionsNumber* Functions_set_value_ = nullptr;
        text_sensor::TextSensor* Sub_mode_sensor_ = nullptr;
        text_sensor::TextSensor* Auto_sub_mode_sensor_ = nullptr;
        text_sensor::TextSensor* error_code_sensor_{ nullptr };
        sensor::Sensor* remote_temp_source_{ nullptr };
        text_sensor::TextSensor* remote_temp_source_info_sensor_{ nullptr };
        HVACOptionSwitch* air_purifier_switch_ = nullptr;
        HVACOptionSwitch* night_mode_switch_ = nullptr;
        HVACOptionSwitch* circulator_switch_ = nullptr;
        HVACOptionSwitch* power_saving_switch_ = nullptr;
        HVACOptionSwitch* coil_drying_switch_ = nullptr;
        binary_sensor::BinarySensor* coil_drying_sensor_{ nullptr };
        bool coil_drying_enabled_{ false };
        bool is_drying_cycle_{ false };
        uint32_t coil_drying_duration_s_{ 900 };
        std::string coil_drying_fan_speed_{ "QUIET" };
        std::string coil_drying_vane_{ "—" };
        ESPPreferenceObject coil_drying_pref_;
        ESPPreferenceObject coil_drying_enabled_pref_;
        binary_sensor::BinarySensor* defrost_sensor_{ nullptr };
        text_sensor::TextSensor* timer_mode_sensor_{ nullptr };
        sensor::Sensor* on_timer_sensor_{ nullptr };
        sensor::Sensor* off_timer_sensor_{ nullptr };
        binary_sensor::BinarySensor* filter_dirty_sensor_{ nullptr };
        binary_sensor::BinarySensor* connection_sensor_{ nullptr };
        sensor::Sensor* checksum_error_sensor_{ nullptr };
        sensor::Sensor* packet_drop_sensor_{ nullptr };
        sensor::Sensor* retransmission_sensor_{ nullptr };
        uint32_t checksum_error_count_{ 0 };
        uint32_t packet_drop_count_{ 0 };
        uint32_t retransmission_count_{ 0 };
        GPIOPin* level_shifter_enable_pin_{ nullptr };
        bool native_five_speed_fan_{ false };
        bool away_active_{ false };
        float away_temperature_{ 10.0f };
        float pre_away_target_temperature_{ NAN };
        float pre_away_target_temperature_low_{ NAN };
        float pre_away_target_temperature_high_{ NAN };
        climate::ClimateMode pre_away_mode_{ climate::CLIMATE_MODE_OFF };
        bool boost_active_{ false };
        sensor::Sensor* boost_remaining_sensor_{ nullptr };
        text_sensor::TextSensor* boost_remaining_time_sensor_{ nullptr };
        uint32_t boost_start_time_ms_{ 0 };
        float pre_boost_target_temperature_{ NAN };
        float pre_boost_target_temperature_low_{ NAN };
        float pre_boost_target_temperature_high_{ NAN };
        std::string pre_boost_fan_mode_{ "" };
        climate::ClimateFanMode pre_boost_std_fan_mode_{ climate::CLIMATE_FAN_AUTO };
        bool pre_boost_had_custom_fan_{ false };
        std::vector<std::string> custom_fan_modes_storage_;
        std::vector<HardwareSettingSelect*> hardware_settings_;
        uint32_t hardware_settings_interval_ms_{ 86400000 };  // Default 24h
        bool compressor_frequency_report_when_idle_{ false };
        bool compressor_frequency_report_when_idle_warning_shown_{ false };

        // The value of the code and value for the functions set.
        int functions_code_ = 0;
        int functions_value_ = 0;

        //select::Select* van_orientation;


        VaneOrientationSelect* vertical_vane_select_ =
            nullptr;  // Select to store manual position of vertical swing
        VaneOrientationSelect* horizontal_vane_select_ =
            nullptr;  // Select to store manual position of horizontal swing
        std::vector<std::string> horizontal_vane_options_strings_;  // Store strings for horizontal vane options
        VaneOrientationSelect* airflow_control_select_ =
            nullptr;
        BoostDurationSelect* boost_duration_select_ =
            nullptr;
        std::vector<std::string> boost_duration_options_;
        sensor::Sensor* compressor_frequency_sensor_ =
            nullptr;  // Sensor to store compressor frequency
        sensor::Sensor* target_humidity_sensor_ =
            nullptr;  // Sensor to expose target humidity from 0x02 settings packet (byte 12)
        sensor::Sensor* input_power_sensor_ =
            nullptr;  // Sensor to store compressor frequency
        sensor::Sensor* kwh_sensor_ =
            nullptr;  // Sensor to store compressor frequency
        sensor::Sensor* runtime_hours_sensor_ =
            nullptr;  // Sensor to store compressor frequency
        sensor::Sensor* outside_air_temperature_sensor_ =
            nullptr;  // Outside air temperature

        // sensor to monitor heatpump connection time
        cn105::HpUpTimeConnectionSensor* hp_uptime_connection_sensor_ = nullptr;

        float convert_input_power_to_W(float raw_input_power);
        float convert_energy_usage_to_kWh(float raw_energy_usage);
        float get_compressor_frequency();
        float get_input_power();
        float get_kwh();
        float get_runtime_hours();
        bool is_operating();
        bool is_air_purifier();
        bool is_night_mode();
        bool is_circulator();
        bool is_power_saving();
        bool is_coil_drying_active() const { return this->is_drying_cycle_; }
        void start_coil_drying_cycle();
        void cancel_coil_drying_cycle(bool update_state = true);

        // checks if the field has changed

        bool hasChanged(const char* before, const char* now, const char* field, bool checkNotNull = false);

        inline bool hasChanged(esphome::StringRef before, const char* now, const char* field, bool checkNotNull = false) {
            return hasChanged(std::string(before).c_str(), now, field, checkNotNull);
        }


        float get_setup_priority() const override {
            return setup_priority::AFTER_WIFI;  // Configurez ce composant après le WiFi
        }

        void generateExtraComponents();

        void setup() override;
        void loop() override;
        void dump_config() override;

        void set_baud_rate(int baud_rate);
        void set_tx_rx_pins(int tx_pin, int rx_pin);
        void set_uart_port(int uart_port) { this->uart_port_ = uart_port; }
        void set_boost_duration(uint32_t duration_ms) { this->boost_duration_ms_ = duration_ms; }
        void set_away_temperature(float temp) { this->away_temperature_ = temp; }
        void set_error_code_translations(const std::map<uint8_t, std::string>& translations) {
            this->error_code_translations_ = translations;
        }
        void set_model_capability_manifest(const char* model, const char* source,
                                           uint16_t supported_mask, uint16_t denied_mask,
                                           bool strict) {
            this->model_name_ = model != nullptr ? model : "";
            this->capability_source_ = source != nullptr ? source : "unconfigured";
            this->capability_supported_mask_ = supported_mask;
            this->capability_denied_mask_ = denied_mask;
            this->strict_capabilities_ = strict;
        }
        //void set_wifi_connected_state(bool state);
        void setupUART();
        void disconnectUART();
        void reconnectUART();
        void buildAndSendRequestsInfoPackets();
        void buildAndSendRequestPacket(int packetType);
        void buildAndSendInfoPacket(uint8_t code);
        bool isHeatpumpConnectionActive();
        void reconnectIfConnectionLost();

        // FSM
        DriverState driver_state() const { return state_; }
        void transition_to_(DriverState next);
        // Compatibility accessors (replace former booleans)
        bool isUARTReady_() const { return state_ >= DriverState::CONNECTING; }
        bool isHeatpumpConnected() const { return state_ == DriverState::CONNECTED; }

        void sendWantedSettings();
        void sendWantedSettingsDelegate();
        // Use the temperature from an external sensor. Use
        // set_remote_temp(0) to switch back to the internal sensor.
        void set_remote_temperature(float);
        void sendRemoteTemperature();
        void sendRemoteTemperaturePacket();  // Send packet only, without resetting watchdog
        void sendWantedRunStates();
        float getDeadbandAdjustedTemperature(float remoteTemperature);

        void set_remote_temp_timeout(uint32_t timeout);

        // Configure the interval for remote temperature keep-alive (in milliseconds)
        // Set to 0 to disable keep-alive
        void set_remote_temp_keepalive_interval(uint32_t interval_ms);

        void set_debounce_delay(uint32_t delay);

        // this is the ping or heartbeat of the setRemotetemperature for timeout management
        void pingExternalTemperature();

        // Start/stop the remote temperature keep-alive timer
        // Keep-alive periodically re-sends the remote temperature to prevent PAC fallback to internal sensor
        void startRemoteTempKeepAlive();
        void stopRemoteTempKeepAlive();

        uint32_t get_update_interval() const;
        void set_update_interval(uint32_t update_interval);

        climate::ClimateTraits traits() override;

        // Get a mutable reference to the traits that we support.
        climate::ClimateTraits& config_traits();

        void control(const esphome::climate::ClimateCall& call) override;
        void controlMode();
        void controlTemperature();
        float calculateTemperatureSetting(float setting);
        float getTargetTemperatureInCurrentMode();
        float getTargetTemperature();
        float getTargetTemperatureLow();
        float getTargetTemperatureHigh();
        float getCurrentTemperature();
        void setTargetTemperature(float temperature);
        void setTargetTemperatureLow(float temperature);
        void setTargetTemperatureHigh(float temperature);
        void setCurrentTemperature(float temperature);

        void set_defrost_sensor(binary_sensor::BinarySensor* sensor);
        void set_supported_custom_fan_modes_config(const std::vector<std::string>& modes);
        bool processPresetChange(const esphome::climate::ClimateCall& call);
        void change_preset_(climate::ClimatePreset preset);
        void controlFan();
        void controlSwing();
        void set_custom_fan_mode(const char* mode);
        void clear_custom_fan_mode();
        bool has_custom_fan_mode() const;
        std::string get_custom_fan_mode_str() const;
        // Bootstrap connexion CN105 en loop() (évite de perdre les tout premiers logs OTA)
        void maybe_start_connection_();

        // Délai de grâce configurable avant d'envoyer CONNECT (pour laisser le flux OTA s'attacher)
        void set_connection_bootstrap_delay(uint32_t delay_ms) { this->conn_bootstrap_delay_ms_ = delay_ms; }


        // Mode installateur: utilise un handshake CONNECT étendu (0x5B) au lieu du standard (0x5A)
        void set_installer_mode(bool mode) {
            // Mode demandé via YAML
            this->installer_mode_ = mode;
            // Mode effectivement utilisé: peut tomber en fallback vers standard si la PAC ignore 0x5B
            this->installer_mode_effective_ = mode && this->profile_->supports(cn105::ProfileFeature::INSTALLER_MODE);
            this->installer_mode_fallback_done_ = false;
        }

        // Unité de puissance brute envoyée par la PAC: false = Watts (défaut), true = BTU/s
        void set_power_unit_is_btu(bool v) { this->power_unit_is_btu_ = v; }

        // Opt-in (supports.restore_setpoints): persist HEAT_COOL band across reboots
        void set_restore_setpoints(bool v) { this->restore_setpoints_ = v; }

        // Configure the climate object with traits that we support.

        // Legacy booleans replaced by DriverState FSM (see state_)
        // bool isUARTConnected_  → isUARTReady_()
        // bool isHeatpumpConnected_ → isHeatpumpConnected()
        bool shouldSendExternalTemperature_ = false;
        float remoteTemperature_ = 0;

        unsigned long nbCompleteCycles_ = 0;
        unsigned long nbCycles_ = 0;
        unsigned int nbHeatpumpConnections_ = 0;


        void sendFirstConnectionPacket();
        void terminateCycle();
        //bool can_proceed() override;


        void functionsArrived();
        bool setFunctions(heatpumpFunctions const& functions);
        bool isGetFunctions_ = false;
        bool isSetFunctions_ = false;

        // helpers
        const char* getIfNotNull(const char* what, const char* defaultValue);

#ifdef TEST_MODE
        void testMutex();
        void testCase1();
        void logDelegate();

#ifdef USE_ESP32
        std::mutex esp32Mutex;
#else
        void testEmulateMutex(const char* retryName, std::function<void()>&& f);
        bool esp8266Mutex = false;
#endif
#endif


    protected:
        // HeatPump object using the underlying Arduino library.
        // same as PolingComponent
        uint32_t update_interval_;

        climate::ClimateTraits traits_;

        //Accessor method for the HardwareSerial pointer
        uart::UARTComponent* get_hw_serial_() {
            return this->parent_;
        }

        bool processInput(void);
        void processDataPacket();
        void getErrorInfoFromResponsePacket();
        void getTimersFromResponsePacket();
        void getMaintenanceFromResponsePacket();
        // Returns true only when an awaited INFO response completed a cycle step.
        bool getDataFromResponsePacket();
        void getPowerFromResponsePacket(); //NET added
        void getSettingsFromResponsePacket();
        void getRoomTemperatureFromResponsePacket();
        void getOperatingAndCompressorFreqFromResponsePacket();
        void getHVACOptionsFromResponsePacket();

        void updateSuccess();
        void processCommand();

        uint8_t checkSum(uint8_t bytes[], int len);

        const char* getModeSetting();
        const char* getPowerSetting();
        const char* getVaneSetting();
        const char* getWideVaneSetting();
        const char* getAirflowControlSetting();
        const char* getFanSpeedSetting();
        float getTemperatureSetting();
        bool getAirPurifierRunState();
        bool getNightModeRunState();
        bool getCirculatorRunState();

        void setModeSetting(const char* setting);
        void setPowerSetting(const char* setting);
        void setVaneSetting(const char* setting);
        void setWideVaneSetting(const char* setting);
        void setAirflowControlSetting(const char* setting);
        void setFanSpeed(const char* setting);

        void setHeatpumpConnected(bool state);

    private:
        friend struct cn105::CommonProtocol;
        friend class cn105::Cn105ProtocolProfile;
        friend class cn105::HeatPumpProfile;
        friend class cn105::LossnayProfile;
        std::unique_ptr<cn105::Cn105ProtocolProfile> profile_;
        void force_low_level_uart_reinit();
        void handleConnectionSuccess();

        int uart_port_ = -1;
        const char* lookupByteMapValue(const char* valuesMap[], const uint8_t byteMap[], int len, uint8_t byteValue, const char* debugInfo = "", const char* defaultValue = nullptr);
        int lookupByteMapValue(const int valuesMap[], const uint8_t byteMap[], int len, uint8_t byteValue, const char* debugInfo = "");
        int lookupByteMapIndex(const char* valuesMap[], int len, const char* lookupValue, const char* debugInfo = "");
        int lookupByteMapIndex(const int valuesMap[], int len, int lookupValue, const char* debugInfo = "");

        bool writePacket(uint8_t* packet, int length, bool checkIsActive = true,
                         PendingWriteKind kind = PendingWriteKind::GENERAL,
                         float value = 0.0f);
        bool enqueue_pending_write_(const uint8_t* packet, int length, bool checkIsActive,
                                    PendingWriteKind kind, float value);
        bool dequeue_pending_write_(PendingWrite& out);
        void finish_write_(PendingWriteKind kind, float value, uint32_t generation);
        void prepareInfoPacket(uint8_t* packet, int length);
        void prepareSetPacket(uint8_t* packet, int length);

        void publishStateToHA(heatpumpSettings& settings);
        void publishWantedSettingsStateToHA();
        void publishWantedRunStatesStateToHA();

        void heatpumpUpdate(heatpumpSettings& settings);

        void statusChanged(heatpumpStatus status);

        void checkPendingWantedSettings();
        void checkPendingWantedRunStates();


        //void statusChanged();
        void updateAction();

        void hpPacketDebug(const uint8_t* packet, unsigned int length, const char* packetDirection, const char* log_prefix = "");

        void debugSettings(const char* settingName, heatpumpSettings& settings);
        void debugSettings(const char* settingName, wantedHeatpumpSettings& settings);
        void debugStatus(const char* statusName, heatpumpStatus status);
        void debugSettingsAndStatus(const char* settingName, heatpumpSettings settings, heatpumpStatus status);
        void debugClimate(const char* settingName);

#ifndef USE_ESP32
        void emulateMutex(const char* retryName, std::function<void()>&& f);
#endif

        void controlDelegate(const esphome::climate::ClimateCall& call);
        // Refactor helpers for controlDelegate
        bool processModeChange(const esphome::climate::ClimateCall& call);
        bool processTemperatureChange(const esphome::climate::ClimateCall& call);
        bool processFanChange(const esphome::climate::ClimateCall& call);
        bool processSwingChange(const esphome::climate::ClimateCall& call);
        void finalizeControlIfUpdated(bool updated);
        void markWantedSettingsPending();
        void markWantedRunStatesPending();
        void restorePreBoostState_();
        void createPacket(uint8_t* packet);
        void createInfoPacket(uint8_t* packet, uint8_t code);
        void log_capability_manifest_() const;
        void warn_if_capability_denied_(ModelCapability capability, const char* option) const;
        bool capability_denied_(ModelCapability capability) const {
            return (this->capability_denied_mask_ & static_cast<uint16_t>(capability)) != 0;
        }
        heatpumpSettings currentSettings{};
        wantedHeatpumpSettings wantedSettings{};
        heatpumpRunStates currentRunStates{};
        wantedHeatpumpRunStates wantedRunStates{};
        cycleManagement loopCycle{};

        // Orchestrateur des requêtes INFO
        RequestScheduler scheduler_;
        void registerInfoRequests();
        void registerHardwareSettingsRequests();

#ifdef USE_ESP32
        std::mutex wantedSettingsMutex;
#else
        volatile bool wantedSettingsMutex = false;
#endif

        unsigned long lastResponseMs;


        uint32_t remote_temp_timeout_;
        uint32_t remote_temp_keepalive_interval_ms_ = DEFAULT_REMOTE_TEMP_KEEPALIVE_INTERVAL_MS;
        bool remote_temp_keepalive_active_ = false;
        uint32_t last_remote_temp_send_ms_ = 0;      // Timestamp of last remote temp packet sent
        float last_remote_temp_sent_ = 0;            // Last remote temp value actually sent (for change detection)
        uint8_t remote_temp_debounce_skip_count_ = 0;   // Counter for consecutive debounce skips
        bool remote_temp_heartbeat_warning_shown_ = false;  // Avoid spamming the warning
        uint32_t debounce_delay_;

        int tx_pin_ = -1;
        int rx_pin_ = -1;

        unsigned long lastSend;
        unsigned long lastConnectRqTimeMs;
        unsigned long lastReconnectTimeMs;

        cn105_protocol::FrameParser parser_;     // UART frame assembler (Phase 3A)
        uint8_t* data;

        // All fields are default-initialized via heatpumpStatus struct defaults (NAN, false, etc.)
        heatpumpStatus currentStatus{};
        heatpumpFunctions functions;

        bool use_msz_a24na_setpoint_table_ = false;

        // counter for status request for checking heatpump is still connected
        // is the counter > MAX_NON_RESPONSE_REQ then we conclude uart is not connected anymore
        int nonResponseCounter = 0;

        // foundStart, bytesRead, dataLength, command → moved into parser_ (Phase 3A)

        // Bounded FIFO for deferred writes: no stack-buffer capture and no
        // single-slot loss when two commands arrive during a reconnect.
        void try_write_pending_packet();
        void send_pending_functions_part2_();
        PendingWrite pending_writes_[MAX_PENDING_WRITES]{};
        uint8_t pending_write_count_ = 0;
        uint8_t pending_functions_part2_packet_[PACKET_LEN]{};
        bool has_pending_functions_part2_{ false };
        uint32_t boost_duration_ms_{ 15U * 60U * 1000U };
#ifndef USE_ESP32
        bool emulate_mutex_pending_{ false };
#endif

        std::map<uint8_t, std::string> error_code_translations_;
        std::string model_name_;
        std::string capability_source_{ "unconfigured" };
        uint16_t capability_supported_mask_ = 0;
        uint16_t capability_denied_mask_ = 0;
        bool strict_capabilities_ = false;

        // Connection lifecycle FSM
        DriverState state_ = DriverState::BOOT;
        uint32_t boot_ms_ = 0;
        uint32_t conn_bootstrap_delay_ms_{ 10000 };  // par défaut 10s
        uint32_t last_bootstrap_attempt_ms_ = 0;

        bool installer_mode_{ false };
        bool installer_mode_effective_{ false };
        bool installer_mode_fallback_done_{ false };

        bool power_unit_is_btu_{ false };  // true = la PAC envoie en BTU/s (conversion ×3600/1055.056 ≈ ×3.412)
        bool supports_dual_setpoint_ = false;
        VaneType vane_type_{ VaneType::STANDARD };

        // --- HEAT_COOL setpoint persistence (opt-in via supports.restore_setpoints) ---
        // The dual-setpoint band is synthetic (the heat pump only stores a single setpoint),
        // so it is lost on reboot. When enabled, we persist {mode, low, high} to flash and
        // re-seed it in setup() before the first settings read.
        bool restore_setpoints_ = false;
        void restore_setpoint_state_();
        void save_setpoint_state_();
    };
}
