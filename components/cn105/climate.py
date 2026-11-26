import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components.uptime.sensor import UptimeSecondsSensor

from esphome.components import (
    climate,
    uart,
    select,
    sensor,
    button,
    switch,
    binary_sensor,
    text_sensor,
    uptime,
    number,
)
from esphome.components.uart import UARTParityOptions

from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_ICON,
    CONF_UPDATE_INTERVAL,
    CONF_MODE,
    CONF_FAN_MODE,
    CONF_SWING_MODE,
    CONF_UART_ID,
    CONF_ENTITY_CATEGORY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    ENTITY_CATEGORY_CONFIG,
    DEVICE_CLASS_HUMIDITY,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_SECOND,
    ICON_TIMER,
    DEVICE_CLASS_DURATION,
    CONF_TX_PIN,
    CONF_RX_PIN,
    CONF_PLATFORM,
    CONF_INTERNAL,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_TEMPERATURE,
    UNIT_HERTZ,
    UNIT_WATT,
    UNIT_KILOWATT_HOURS,
    UNIT_CELSIUS,
    UNIT_HOUR,
    STATE_CLASS_MEASUREMENT,
)
from esphome.components.sensor import (
    CONF_UNIT_OF_MEASUREMENT as SENSOR_CONF_UNIT_OF_MEASUREMENT,
    CONF_ACCURACY_DECIMALS as SENSOR_CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS as SENSOR_CONF_DEVICE_CLASS,
    CONF_STATE_CLASS as SENSOR_CONF_STATE_CLASS,
)
from esphome.core import CORE, coroutine

# ... (AUTO_LOAD, DEPENDENCIES, toutes les constantes CONF_XXX_SENSOR, DEFAULT_MODES - identiques à votre version) ...
AUTO_LOAD = [
    "climate",
    "sensor",
    "select",
    "binary_sensor",
    "button",
    "switch",
    "text_sensor",
    "uart",
    "uptime",
    "number",
]
DEPENDENCIES = ["uart"]  # Garder uart ici aussi

_LOGGER = logging.getLogger(__name__)


def _keepalive_interval(value):
    """Validate a keep-alive interval without ESPHome coercing 0s to 1ms.

    ESPHome 2026.11 changed cv.update_interval() to turn literal 0ms into 1ms.
    For this component 0s is the documented way to disable the keep-alive, so
    use the lower-level period validator that preserves zero. "never" maps to
    the same disabled value.
    """
    if isinstance(value, str) and value.strip().lower() == "never":
        value = "0ms"
    return cv.time_period(value)


def _positive_time_period(value):
    """Accept a time period but reject zero/negative durations."""
    period = cv.time_period(value)
    if period.total_milliseconds <= 0:
        raise cv.Invalid("duration must be greater than zero")
    return period


def _nonempty_string(value):
    value = cv.string(value).strip()
    if not value:
        raise cv.Invalid("value must not be empty")
    return value


def _min_update_interval(value):
    period = cv.update_interval(value)
    if period.total_milliseconds < 500:
        raise cv.Invalid("update_interval must be at least 500ms")
    return period


COIL_DRYING_FAN_MAP = {
    "AUTO": "AUTO",
    "QUIET": "QUIET",
    "1": "1",
    "LOW": "1",
    "2": "2",
    "MEDIUM": "2",
    "3": "3",
    "MIDDLE": "3",
    "4": "4",
    "HIGH": "4",
}

COIL_DRYING_VANE_MAP = {
    "AUTO": "AUTO",
    "↑↑": "↑↑",
    "UP_MAX": "↑↑",
    "↑": "↑",
    "UP": "↑",
    "—": "—",
    "CENTER": "—",
    "MID": "—",
    "HORIZONTAL": "—",
    "↓": "↓",
    "DOWN": "↓",
    "↓↓": "↓↓",
    "DOWN_MAX": "↓↓",
    "SWING": "SWING",
}

HORIZONTAL_VANE_VALID_OPTIONS = {
    "←←",
    "←",
    "|",
    "→",
    "→→",
    "←→",
    "SWING",
    "AIRFLOW CONTROL",
    "SPLIT",
    "LEFT_RIGHT",
    "SPLIT_AIRFLOW",
}


def _coil_drying_fan_speed(value):
    key = cv.string(value).strip().upper()
    if key not in COIL_DRYING_FAN_MAP:
        raise cv.Invalid(
            f"Invalid coil_drying_fan_speed '{value}'. Allowed: {', '.join(sorted(COIL_DRYING_FAN_MAP))}"
        )
    return COIL_DRYING_FAN_MAP[key]


def _coil_drying_vane(value):
    raw = cv.string(value).strip()
    key = raw.upper()
    if raw in COIL_DRYING_VANE_MAP:
        return COIL_DRYING_VANE_MAP[raw]
    if key in COIL_DRYING_VANE_MAP:
        return COIL_DRYING_VANE_MAP[key]
    raise cv.Invalid(
        f"Invalid coil_drying_vane '{value}'. Allowed: AUTO, ↑↑, ↑, —, ↓, ↓↓, SWING (or UP_MAX, UP, CENTER, DOWN, DOWN_MAX)"
    )


def _horizontal_vane_options(value):
    items = cv.ensure_list(cv.string)(value)
    invalid = [item for item in items if item not in HORIZONTAL_VANE_VALID_OPTIONS]
    if invalid:
        raise cv.Invalid(
            "Invalid horizontal_vane_mode option(s): "
            + ", ".join(invalid)
            + ". Allowed: "
            + ", ".join(sorted(HORIZONTAL_VANE_VALID_OPTIONS))
        )
    return items


def _custom_fan_modes(value):
    """Only FULL is implemented, and BOOST depends on it being advertised."""
    # cv.In was removed from esphome.config_validation in ESPHome 2026.11;
    # cv.one_of is available across all supported versions (2025.11+).
    value = cv.ensure_list(cv.one_of(*DEFAULT_CUSTOM_FAN_MODES))(value)
    if "FULL" not in value:
        raise cv.Invalid("supports.custom_fan_mode must include FULL (currently the only implemented custom fan mode)")
    return value

CONF_SUPPORTS = "supports"
CONF_SUPPORTS_HORIZONTAL_VANE_MODE = "horizontal_vane_mode"
CONF_HORIZONTAL_VANES = "horizontal_vanes"
CONF_VANE_TYPE = "vane_type"

VANE_TYPES = {
    "standard": 0,
    "split_horizontal": 1,
    "split_vertical": 2,
}
CONF_HORIZONTAL_SWING_SELECT = "horizontal_vane_select"
CONF_VERTICAL_SWING_SELECT = "vertical_vane_select"
CONF_COMPRESSOR_FREQUENCY_SENSOR = "compressor_frequency_sensor"
CONF_INPUT_POWER_SENSOR = "input_power_sensor"
CONF_KWH_SENSOR = "kwh_sensor"
CONF_RUNTIME_HOURS_SENSOR = "runtime_hours_sensor"
CONF_OUTSIDE_AIR_TEMPERATURE_SENSOR = "outside_air_temperature_sensor"
CONF_TARGET_HUMIDITY_SENSOR = "target_humidity_sensor"
CONF_ISEE_SENSOR = "isee_sensor"
CONF_FUNCTIONS_SENSOR = "functions_sensor"
CONF_FUNCTIONS_BUTTON = "functions_get_button"
CONF_FUNCTIONS_SET_BUTTON = "functions_set_button"
CONF_FUNCTIONS_SET_CODE = "functions_set_code"
CONF_FUNCTIONS_SET_VALUE = "functions_set_value"
CONF_STAGE_SENSOR = "stage_sensor"
CONF_SUB_MODE_SENSOR = "sub_mode_sensor"
CONF_AUTO_SUB_MODE_SENSOR = "auto_sub_mode_sensor"
CONF_ERROR_CODE_SENSOR = "error_code_sensor"
CONF_REMOTE_TEMP_SOURCE = "remote_temperature_source"
CONF_REMOTE_TEMP_SOURCE_SENSOR_ID = "sensor_id"
CONF_REMOTE_TEMP_SOURCE_INFO = "info"
CONF_HP_UP_TIME_CONNECTION_SENSOR = "hp_uptime_connection_sensor"
CONF_USE_AS_OPERATING_FALLBACK = "use_as_operating_fallback"  # Nouvelle constante
CONF_FAHRENHEIT_SUPPORT_MODE = "fahrenheit_compatibility"
CONF_MSZ_A24NA_SETPOINT_TABLE = "msz_a24na_setpoint_table"
CONF_AIRFLOW_CONTROL_SELECT = "airflow_control_select"
CONF_AIR_PURIFIER_SWITCH = "air_purifier_switch"
CONF_NIGHT_MODE_SWITCH = "night_mode_switch"
CONF_CIRCULATOR_SWITCH = "circulator_switch"
CONF_POWER_SAVING_SWITCH = "power_saving_switch"
CONF_COIL_DRYING_SWITCH = "coil_drying_switch"
CONF_COIL_DRYING_SENSOR = "coil_drying_sensor"
CONF_COIL_DRYING_DURATION = "coil_drying_duration"
CONF_COIL_DRYING_FAN_SPEED = "coil_drying_fan_speed"
CONF_COIL_DRYING_VANE = "coil_drying_vane"
CONF_TIMER_MODE_SENSOR = "timer_mode_sensor"
CONF_ON_TIMER_SENSOR = "on_timer_sensor"
CONF_OFF_TIMER_SENSOR = "off_timer_sensor"
CONF_FILTER_DIRTY_SENSOR = "filter_dirty_sensor"
CONF_CONNECTION_SENSOR = "connection_status_sensor"
CONF_AWAY_TEMPERATURE = "away_temperature"
CONF_CHECKSUM_ERROR_SENSOR = "checksum_error_sensor"
CONF_PACKET_DROP_SENSOR = "packet_drop_sensor"
CONF_RETRANSMISSION_SENSOR = "retransmission_sensor"
CONF_LEVEL_SHIFTER_ENABLE_PIN = "level_shifter_enable_pin"
CONF_NATIVE_FIVE_SPEED_FAN = "native_five_speed_fan"
CONF_BOOST_REMAINING_SENSOR = "boost_remaining_sensor"
CONF_BOOST_REMAINING_TIME_SENSOR = "boost_remaining_time_sensor"
CONF_HARDWARE_SETTINGS = "hardware_settings"
CONF_CODE = "code"
CONF_OPTIONS = "options"
CONF_REMOTE_TEMPERATURE_CONTROL_SENSOR = "remote_temperature_control_sensor"
#CONF_REMOTE_TEMPERATURE_MARGIN = "remote_temperature_margin"
CONF_TEMPERATURE_MARGIN = "temperature_margin"
CONF_POWER_UNIT_IS_BTU = "power_unit_is_btu"
CONF_REPORT_WHEN_IDLE = "report_when_idle"

# Hidden/advanced options. These are valid but intentionally not shown in the
# short README example; see the "Advanced options" section instead.
CONF_UART_PORT = "uart_port"
CONF_BOOST_DURATION = "boost_duration"
CONF_ERROR_CODE_TRANSLATIONS = "error_code_translations"
CONF_MODEL = "model"
CONF_CAPABILITIES = "capabilities"
CONF_STRICT_CAPABILITIES = "strict_capabilities"
CONF_CAPABILITY_SUPPORTED = "supported"
CONF_CAPABILITY_DENIED = "denied"
CONF_CAPABILITY_SOURCE = "source"
DEFAULT_BOOST_DURATION_MS = 15 * 60 * 1000
DEFAULT_COIL_DRYING_DURATION_S = 15 * 60
DEFAULT_REMOTE_TEMP_KEEPALIVE_MS = 20 * 1000

# Bit values mirror esphome::cn105::ModelCapability in cn105.h. Keep stable:
# generated firmware stores these as a uint16_t.
CAPABILITY_BITS = {
    "night_mode": 1 << 0,
    "boost": 1 << 1,
    "sleep": 1 << 2,
    "eco": 1 << 3,
    "air_purifier": 1 << 4,
    "isee": 1 << 5,
    "airflow_control": 1 << 6,
    "target_humidity": 1 << 7,
    "dual_setpoint": 1 << 8,
    "hardware_settings_writes": 1 << 9,
}
CAPABILITY_NAMES = tuple(CAPABILITY_BITS)

# Exact-model manifests only. VGKP evidence must not be copied here: the only
# confirmed non-P quirk is that AY35VGK can report 0 Hz while operating.
BUILTIN_CAPABILITY_MANIFESTS = {
    "MSZ-AY35VGK": {
        "source": "built-in exact-model",
        "supported": ["night_mode", "boost", "sleep", "eco"],
        "denied": [
            "air_purifier",
            "isee",
            "airflow_control",
            "target_humidity",
            "dual_setpoint",
            "hardware_settings_writes",
        ],
    },
    "MSZ-AY35VGKP": {
        "source": "built-in exact-model",
        "supported": ["night_mode", "boost", "sleep", "eco", "air_purifier"],
        "denied": [
            "isee",
            "airflow_control",
            "dual_setpoint",
        ],
    },
    "MSZ-LN25VG2W": {
        "source": "built-in exact-model",
        "supported": [
            "night_mode",
            "boost",
            "sleep",
            "eco",
            "air_purifier",
            "isee",
            "airflow_control",
            "target_humidity",
        ],
        "denied": [],
    },
    "MSZ-LN35VGW": {
        "source": "built-in exact-model",
        "supported": [
            "night_mode",
            "boost",
            "sleep",
            "eco",
            "air_purifier",
            "isee",
            "airflow_control",
            "target_humidity",
        ],
        "denied": [],
    },
    "MSZ-GE24NA": {
        "source": "built-in exact-model",
        "supported": ["boost", "sleep", "eco"],
        "denied": ["air_purifier", "isee", "airflow_control"],
    },
    "MSZ-A24NA": {
        "source": "built-in exact-model",
        "supported": ["boost", "sleep", "eco"],
        "denied": ["air_purifier", "isee", "airflow_control", "dual_setpoint"],
    },
}

OPTION_CAPABILITIES = {
    CONF_NIGHT_MODE_SWITCH: "night_mode",
    CONF_AIR_PURIFIER_SWITCH: "air_purifier",
    CONF_ISEE_SENSOR: "isee",
    CONF_AIRFLOW_CONTROL_SELECT: "airflow_control",
    CONF_TARGET_HUMIDITY_SENSOR: "target_humidity",
    CONF_HARDWARE_SETTINGS: "hardware_settings_writes",
    CONF_FUNCTIONS_SET_BUTTON: "hardware_settings_writes",
}

CONF_DEFROST_SENSOR = "defrost_sensor"
CONF_CUSTOM_FAN_MODE = "custom_fan_mode"
DEFAULT_CUSTOM_FAN_MODES = ["FULL"]

# Support explicite du DUAL setpoint via YAML
CONF_DUAL_SETPOINT = "dual_setpoint"
CONF_RESTORE_SETPOINTS = "restore_setpoints"

DEFAULT_CLIMATE_MODES = ["AUTO", "COOL", "HEAT", "DRY", "FAN_ONLY", "HEAT_COOL"]
DEFAULT_FAN_MODES = ["AUTO", "MIDDLE", "QUIET", "LOW", "MEDIUM", "HIGH"]
DEFAULT_SWING_MODES = ["OFF", "VERTICAL", "HORIZONTAL", "BOTH"]

FAHRENHEIT_MODES = {
    "disabled": 0,
    "standard": 1,
    "alt": 2,
    "msz_a24na": 3,
    "false": 0,
    "true": 1,
}


CN105Climate = cg.global_ns.class_(
    "CN105Climate", climate.Climate, cg.Component, uart.UARTDevice
)
CONF_REMOTE_TEMP_TIMEOUT = "remote_temperature_timeout"
CONF_REMOTE_TEMP_KEEPALIVE_INTERVAL = "remote_temperature_keepalive_interval"
CONF_DEBOUNCE_DELAY = "debounce_delay"
CONF_CONNECTION_BOOTSTRAP_DELAY = "connection_bootstrap_delay"
CONF_INSTALLER_MODE = "installer_mode"
CONF_LOSSNAY = "lossnay"

LOSSNAY_CLIMATE_MODES = ["AUTO", "HEAT", "FAN_ONLY"]
LOSSNAY_FAN_MODES = ["LOW", "MEDIUM", "MIDDLE", "HIGH"]

LOSSNAY_UNSUPPORTED_OPTIONS = {
    CONF_HORIZONTAL_SWING_SELECT,
    CONF_VERTICAL_SWING_SELECT,
    CONF_COMPRESSOR_FREQUENCY_SENSOR,
    CONF_RUNTIME_HOURS_SENSOR,
    CONF_OUTSIDE_AIR_TEMPERATURE_SENSOR,
    CONF_DEFROST_SENSOR,
    CONF_ERROR_CODE_SENSOR,
    CONF_ISEE_SENSOR,
    CONF_TARGET_HUMIDITY_SENSOR,
    CONF_FUNCTIONS_SENSOR,
    CONF_FUNCTIONS_BUTTON,
    CONF_FUNCTIONS_SET_BUTTON,
    CONF_FUNCTIONS_SET_CODE,
    CONF_FUNCTIONS_SET_VALUE,
    CONF_STAGE_SENSOR,
    CONF_SUB_MODE_SENSOR,
    CONF_AUTO_SUB_MODE_SENSOR,
    CONF_REMOTE_TEMP_SOURCE,
    CONF_AIRFLOW_CONTROL_SELECT,
    CONF_AIR_PURIFIER_SWITCH,
    CONF_NIGHT_MODE_SWITCH,
    CONF_CIRCULATOR_SWITCH,
    CONF_POWER_SAVING_SWITCH,
    CONF_COIL_DRYING_SWITCH,
    CONF_COIL_DRYING_SENSOR,
    CONF_TIMER_MODE_SENSOR,
    CONF_ON_TIMER_SENSOR,
    CONF_OFF_TIMER_SENSOR,
    CONF_HARDWARE_SETTINGS,
    CONF_REMOTE_TEMPERATURE_CONTROL_SENSOR,
}

# Définitions des classes C++ (identiques à votre version)
VaneOrientationSelect = cg.global_ns.class_(
    "VaneOrientationSelect", select.Select, cg.Component
)
CompressorFrequencySensor = cg.global_ns.class_(
    "CompressorFrequencySensor", sensor.Sensor, cg.Component
)
InputPowerSensor = cg.global_ns.class_("InputPowerSensor", sensor.Sensor, cg.Component)
kWhSensor = cg.global_ns.class_("kWhSensor", sensor.Sensor, cg.Component)
RuntimeHoursSensor = cg.global_ns.class_(
    "RuntimeHoursSensor", sensor.Sensor, cg.Component
)
OutsideAirTemperatureSensor = cg.global_ns.class_(
    "OutsideAirTemperatureSensor", sensor.Sensor, cg.Component
)
ISeeSensor = cg.global_ns.class_("ISeeSensor", binary_sensor.BinarySensor, cg.Component)
TargetHumiditySensor = cg.global_ns.class_(
    "TargetHumiditySensor", sensor.Sensor, cg.Component
)
StageSensor = cg.global_ns.class_("StageSensor", text_sensor.TextSensor, cg.Component)
FunctionsSensor = cg.global_ns.class_(
    "FunctionsSensor", text_sensor.TextSensor, cg.Component
)
FunctionsButton = cg.global_ns.class_("FunctionsButton", button.Button, cg.Component)
FunctionsNumber = cg.global_ns.class_("FunctionsNumber", number.Number, cg.Component)
SubModSensor = cg.global_ns.class_("SubModSensor", text_sensor.TextSensor, cg.Component)
AutoSubModSensor = cg.global_ns.class_(
    "AutoSubModSensor", text_sensor.TextSensor, cg.Component
)
ErrorCodeSensor = cg.global_ns.class_(
    "ErrorCodeSensor", text_sensor.TextSensor, cg.Component
)
RemoteTempSourceInfo = cg.global_ns.class_(
    "RemoteTempSourceInfo", text_sensor.TextSensor, cg.Component
)
cn105_ns = cg.esphome_ns.namespace("cn105")
HpUpTimeConnectionSensor = cn105_ns.class_(
    "HpUpTimeConnectionSensor", sensor.Sensor, cg.PollingComponent
)
FlowControlSensor = cg.global_ns.class_(
    "FlowControlSensor", text_sensor.TextSensor, cg.Component
)
HVACOptionSwitch = cg.global_ns.class_("HVACOptionSwitch", switch.Switch, cg.Component)
HardwareSettingSelect = cg.global_ns.class_(
    "HardwareSettingSelect", select.Select, cg.Component
)


# --- Fonction d'aide pour récupérer les pins TX/RX (identique à votre version corrigée) ---
def get_uart_pins_from_config(core_config, target_uart_id_str):
    tx_pin_num = -1
    rx_pin_num = -1
    uart_config_found = {}
    for uart_conf_item in core_config.get("uart", []):
        if str(uart_conf_item[CONF_ID]) == target_uart_id_str:
            uart_config_found = uart_conf_item
            break
    if uart_config_found:
        tx_pin_schema = uart_config_found.get(CONF_TX_PIN)
        if tx_pin_schema:
            if isinstance(tx_pin_schema, dict) and "number" in tx_pin_schema:
                tx_pin_num = tx_pin_schema["number"]
            elif isinstance(tx_pin_schema, int):
                tx_pin_num = tx_pin_schema
        rx_pin_schema = uart_config_found.get(CONF_RX_PIN)
        if rx_pin_schema:
            if isinstance(rx_pin_schema, dict) and "number" in rx_pin_schema:
                rx_pin_num = rx_pin_schema["number"]
            elif isinstance(rx_pin_schema, int):
                rx_pin_num = rx_pin_schema
    return tx_pin_num, rx_pin_num


def get_uart_port_index(core_config, target_uart_id_str):
    # ESPHome ne fournit pas directement l'index de contrôleur; on l'infère
    # via l'ordre de déclaration ou restons à 0 par défaut.
    # On tente d'associer l'objet id() à sa position.
    idx = 0
    for i, uart_conf_item in enumerate(core_config.get("uart", [])):
        if str(uart_conf_item[CONF_ID]) == target_uart_id_str:
            idx = i  # souvent 0 => UART0, 1 => UART1, 2 => UART2
            break
    # Clamp 0..2
    if idx < 0:
        idx = 0
    if idx > 2:
        idx = 2
    return idx


# --- FIN de la fonction d'aide ---

# Schémas pour les entités optionnelles (identiques à votre version)
SELECT_SCHEMA = select.select_schema(VaneOrientationSelect).extend(
    {cv.GenerateID(CONF_ID): cv.declare_id(VaneOrientationSelect)}
)
COMPRESSOR_FREQUENCY_SENSOR_SCHEMA = sensor.sensor_schema(
    CompressorFrequencySensor,
    unit_of_measurement=UNIT_HERTZ,
    device_class=DEVICE_CLASS_FREQUENCY,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=1,
).extend({
    cv.GenerateID(CONF_ID): cv.declare_id(CompressorFrequencySensor),
    cv.Optional(CONF_REPORT_WHEN_IDLE): cv.boolean
})
INPUT_POWER_SENSOR_SCHEMA = sensor.sensor_schema(
    InputPowerSensor,
    unit_of_measurement=UNIT_WATT,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=0,
).extend({cv.GenerateID(CONF_ID): cv.declare_id(InputPowerSensor)})
KWH_SENSOR_SCHEMA = sensor.sensor_schema(
    kWhSensor,
    unit_of_measurement=UNIT_KILOWATT_HOURS,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    accuracy_decimals=1,
).extend({cv.GenerateID(CONF_ID): cv.declare_id(kWhSensor)})
RUNTIME_HOURS_SENSOR_SCHEMA = sensor.sensor_schema(
    RuntimeHoursSensor,
    unit_of_measurement=UNIT_HOUR,
    device_class=DEVICE_CLASS_DURATION,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    accuracy_decimals=2,
).extend({cv.GenerateID(CONF_ID): cv.declare_id(RuntimeHoursSensor)})
OUTSIDE_AIR_TEMPERATURE_SENSOR_SCHEMA = sensor.sensor_schema(
    OutsideAirTemperatureSensor,
    unit_of_measurement=UNIT_CELSIUS,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=1,
).extend({cv.GenerateID(CONF_ID): cv.declare_id(OutsideAirTemperatureSensor)})
ISEE_SENSOR_SCHEMA = binary_sensor.binary_sensor_schema(ISeeSensor).extend(
    {cv.GenerateID(CONF_ID): cv.declare_id(ISeeSensor)}
)
TARGET_HUMIDITY_SENSOR_SCHEMA = sensor.sensor_schema(
    TargetHumiditySensor,
    unit_of_measurement="%",
    device_class=DEVICE_CLASS_HUMIDITY,
    state_class=STATE_CLASS_MEASUREMENT,
    accuracy_decimals=0,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
).extend({cv.GenerateID(CONF_ID): cv.declare_id(TargetHumiditySensor)})
FUNCTIONS_SENSOR_SCHEMA = text_sensor.text_sensor_schema(FunctionsSensor).extend(
    {cv.GenerateID(CONF_ID): cv.declare_id(FunctionsSensor)}
)
FUNCTIONS_BUTTON_SCHEMA = button.button_schema(
    FunctionsButton, entity_category=ENTITY_CATEGORY_CONFIG
).extend(
    {cv.GenerateID(CONF_ID): cv.declare_id(FunctionsButton)}
)
FUNCTIONS_NUMBER_SCHEMA = number.number_schema(
    FunctionsNumber, entity_category=ENTITY_CATEGORY_CONFIG
).extend(
    {cv.GenerateID(CONF_ID): cv.declare_id(FunctionsNumber)}
)
SUB_MODE_SENSOR_SCHEMA = text_sensor.text_sensor_schema(SubModSensor).extend(
    {cv.GenerateID(CONF_ID): cv.declare_id(SubModSensor)}
)
AUTO_SUB_MODE_SENSOR_SCHEMA = text_sensor.text_sensor_schema(AutoSubModSensor).extend(
    {cv.GenerateID(CONF_ID): cv.declare_id(AutoSubModSensor)}
)

ERROR_CODE_SENSOR_SCHEMA = text_sensor.text_sensor_schema(
    ErrorCodeSensor, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
).extend(
    {cv.GenerateID(CONF_ID): cv.declare_id(ErrorCodeSensor)}
)

REMOTE_TEMP_SOURCE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_REMOTE_TEMP_SOURCE_SENSOR_ID): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_REMOTE_TEMP_SOURCE_INFO): text_sensor.text_sensor_schema(
            RemoteTempSourceInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(
            {cv.GenerateID(CONF_ID): cv.declare_id(RemoteTempSourceInfo)}
        ),
    }
)

REMOTE_TEMPERATURE_CONTROL_SENSOR_SCHEMA = binary_sensor.binary_sensor_schema(
    binary_sensor.BinarySensor,
    device_class="connectivity",
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    icon="mdi:thermometer-check",
).extend(
    {
        cv.GenerateID(CONF_ID): cv.declare_id(binary_sensor.BinarySensor),
        cv.Optional(CONF_TEMPERATURE_MARGIN, default=0.4): cv.positive_float,
    }
)

# Schéma pour STAGE_SENSOR (qui est un text_sensor) AVEC la nouvelle sous-option
STAGE_SENSOR_CONFIG_SCHEMA = text_sensor.text_sensor_schema(StageSensor).extend(
    {
        # L'ID pour l'objet StageSensor C++ est géré par text_sensor.TEXT_SENSOR_SCHEMA (via CONF_ID)
        cv.Optional(CONF_USE_AS_OPERATING_FALLBACK, default=False): cv.boolean,
    }
)

# Schéma pour HP_UP_TIME_CONNECTION_SENSOR (identique à votre version)
HP_UP_TIME_CONNECTION_SENSOR_SCHEMA = sensor.sensor_schema(
    HpUpTimeConnectionSensor,
    unit_of_measurement=UNIT_SECOND,
    icon=ICON_TIMER,
    accuracy_decimals=0,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    device_class=DEVICE_CLASS_DURATION,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
).extend(cv.polling_component_schema("60s"))

HVAC_OPTION_SWITCH_SCHEMA = switch.switch_schema(HVACOptionSwitch).extend(
    {cv.GenerateID(CONF_ID): cv.declare_id(HVACOptionSwitch)}
)

HARDWARE_SETTING_ITEM_SCHEMA = select.select_schema(
    HardwareSettingSelect, entity_category=ENTITY_CATEGORY_CONFIG
).extend(
    {
        cv.Required(CONF_CODE): cv.int_range(min=101, max=128),
        cv.Required(CONF_OPTIONS): cv.Schema({cv.int_range(min=1, max=3): cv.string}),
    }
)

CONF_HARDWARE_SETTINGS_LIST = "list"

HARDWARE_SETTING_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_UPDATE_INTERVAL, default="24h"): cv.update_interval,
        cv.Required(CONF_HARDWARE_SETTINGS_LIST): cv.ensure_list(
            HARDWARE_SETTING_ITEM_SCHEMA
        ),
    }
)

CAPABILITY_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_CAPABILITY_SUPPORTED, default=[]): cv.ensure_list(
            cv.one_of(*CAPABILITY_NAMES)
        ),
        cv.Optional(CONF_CAPABILITY_DENIED, default=[]): cv.ensure_list(
            cv.one_of(*CAPABILITY_NAMES)
        ),
        cv.Optional(CONF_CAPABILITY_SOURCE, default="user"): _nonempty_string,
    }
)

ERROR_CODE_TRANSLATIONS_SCHEMA = cv.Schema(
    {cv.int_range(min=1, max=127): cv.string}
)


def resolve_capability_manifest(config):
    """Merge built-in exact-model data with explicit user overrides."""
    model = config.get(CONF_MODEL, "")
    explicit = config.get(CONF_CAPABILITIES)
    builtin = BUILTIN_CAPABILITY_MANIFESTS.get(model)

    supported = list(builtin["supported"]) if builtin else []
    denied = list(builtin["denied"]) if builtin else []
    source = builtin["source"] if builtin else "unconfigured"

    if explicit:
        explicit_supported = explicit.get(CONF_CAPABILITY_SUPPORTED, [])
        explicit_denied = explicit.get(CONF_CAPABILITY_DENIED, [])
        supported = [name for name in supported if name not in explicit_denied]
        denied = [name for name in denied if name not in explicit_supported]
        supported.extend(name for name in explicit_supported if name not in supported)
        denied.extend(name for name in explicit_denied if name not in denied)
        if builtin:
            source = f"built-in exact-model + {explicit.get(CONF_CAPABILITY_SOURCE, 'user')}"
        else:
            source = explicit.get(CONF_CAPABILITY_SOURCE, "user")

    mask = 0
    for name in supported:
        mask |= CAPABILITY_BITS[name]
    denied_mask = 0
    for name in denied:
        denied_mask |= CAPABILITY_BITS[name]

    return {
        "model": model,
        "source": source,
        "supported": supported,
        "denied": denied,
        "supported_mask": mask,
        "denied_mask": denied_mask,
    }


def validate_capability_config(config):
    explicit = config.get(CONF_CAPABILITIES)
    strict = config.get(CONF_STRICT_CAPABILITIES, False)
    model = config.get(CONF_MODEL, "")

    if (explicit is not None or strict) and not model:
        raise cv.Invalid("model is required when capabilities or strict_capabilities are configured")

    if explicit:
        overlap = sorted(
            set(explicit.get(CONF_CAPABILITY_SUPPORTED, []))
            & set(explicit.get(CONF_CAPABILITY_DENIED, []))
        )
        if overlap:
            raise cv.Invalid(
                "capabilities.supported and capabilities.denied overlap: "
                + ", ".join(overlap)
            )

    if not strict:
        return config

    manifest = resolve_capability_manifest(config)
    denied = set(manifest["denied"])
    enabled = [
        f"{option}={capability}"
        for option, capability in OPTION_CAPABILITIES.items()
        if option in config and capability in denied
    ]
    if config.get(CONF_SUPPORTS, {}).get(CONF_DUAL_SETPOINT, False) and "dual_setpoint" in denied:
        enabled.append("supports.dual_setpoint=dual_setpoint")
    if config.get(CONF_SUPPORTS, {}).get(CONF_CUSTOM_FAN_MODE, DEFAULT_CUSTOM_FAN_MODES) and "boost" in denied:
        enabled.append("supports.custom_fan_mode=boost")

    if enabled:
        raise cv.Invalid(
            "strict_capabilities rejected options denied by the manifest for model "
            f"'{model}': {', '.join(enabled)}"
        )

    return config


def validate_config_dependencies(config):
    supports = config.get(CONF_SUPPORTS, {})
    if supports.get(CONF_DUAL_SETPOINT, False):
        modes = supports.get(CONF_MODE, DEFAULT_CLIMATE_MODES)
        if "HEAT_COOL" not in modes:
            raise cv.Invalid(
                "supports.dual_setpoint requires HEAT_COOL in supports.mode"
            )

    if (
        CONF_AIRFLOW_CONTROL_SELECT in config
        and CONF_ISEE_SENSOR not in config
    ):
        raise cv.Invalid(
            "airflow_control_select requires isee_sensor so the select has a real i-See state source"
        )

    if config.get(CONF_ERROR_CODE_TRANSLATIONS) and CONF_ERROR_CODE_SENSOR not in config:
        _LOGGER.warning(
            "error_code_translations is configured without error_code_sensor; no entity will show the translated text"
        )

    core_config = getattr(CORE, "config", None)
    is_esp_idf = (
        core_config is not None
        and isinstance(core_config.get("esp32"), dict)
        and core_config["esp32"].get("framework", {}).get("type") == "esp-idf"
    )
    if (
        core_config is not None
        and not is_esp_idf
        and len(core_config.get("uart", [])) > 1
        and CONF_UART_PORT not in config
    ):
        _LOGGER.warning(
            "Multiple UARTs configured. Set uart_port explicitly so low-level UART reinit targets the CN105 port."
        )

    return config


def validate_lossnay_config(config):
    if not config.get(CONF_LOSSNAY, False):
        return config

    supports = config.get(CONF_SUPPORTS, {})
    modes = supports.get(CONF_MODE, LOSSNAY_CLIMATE_MODES)
    allowed_modes = set(LOSSNAY_CLIMATE_MODES) | {"OFF"}
    invalid_modes = sorted(set(modes) - allowed_modes)
    if invalid_modes:
        raise cv.Invalid(
            "lossnay supports only AUTO, HEAT, FAN_ONLY and OFF climate modes; "
            f"unsupported modes: {', '.join(invalid_modes)}"
        )

    fan_modes = supports.get(CONF_FAN_MODE, LOSSNAY_FAN_MODES)
    invalid_fans = sorted(set(fan_modes) - set(LOSSNAY_FAN_MODES))
    if invalid_fans:
        raise cv.Invalid(
            "lossnay supports only LOW, MEDIUM, MIDDLE and HIGH fan modes; "
            f"unsupported fan modes: {', '.join(invalid_fans)}"
        )

    swing_modes = supports.get(CONF_SWING_MODE, [])
    if swing_modes:
        raise cv.Invalid("lossnay does not support swing modes")

    if supports.get(CONF_DUAL_SETPOINT, False):
        raise cv.Invalid("lossnay does not support dual target temperatures")

    unsupported_options = sorted(
        option for option in LOSSNAY_UNSUPPORTED_OPTIONS if option in config
    )
    if unsupported_options:
        raise cv.Invalid(
            "lossnay does not support these options: "
            + ", ".join(unsupported_options)
        )

    if config.get(CONF_INSTALLER_MODE, False):
        raise cv.Invalid("lossnay does not support installer_mode")

    if config.get(CONF_MSZ_A24NA_SETPOINT_TABLE, False):
        raise cv.Invalid("lossnay does not support msz_a24na_setpoint_table")

    if supports.get(CONF_RESTORE_SETPOINTS, False):
        raise cv.Invalid("lossnay does not support setpoint restoration")

    if CONF_SUPPORTS_HORIZONTAL_VANE_MODE in supports:
        raise cv.Invalid("lossnay does not support horizontal vane modes")

    if supports.get(CONF_HORIZONTAL_VANES, 1) != 1:
        raise cv.Invalid("lossnay does not support horizontal vanes")

    if supports.get(CONF_VANE_TYPE, "standard") != "standard":
        raise cv.Invalid("lossnay does not support vane types")

    remote_temp_timeout = config.get(CONF_REMOTE_TEMP_TIMEOUT)
    if remote_temp_timeout is not None:
        # 2025.11 returns the integer sentinel 4294967295 for "never"; newer
        # versions may return a TimePeriodMilliseconds wrapping that sentinel.
        timeout_ms = getattr(remote_temp_timeout, "total_milliseconds", remote_temp_timeout)
        if timeout_ms not in (0, 4294967295):
            raise cv.Invalid("lossnay does not support remote temperature")
    # The schema default (20s) is harmless because Lossnay has no remote
    # temperature sensor; only reject a non-default, explicitly useful value.
    keepalive = config.get(CONF_REMOTE_TEMP_KEEPALIVE_INTERVAL)
    if keepalive is not None and getattr(
        keepalive, "total_milliseconds", DEFAULT_REMOTE_TEMP_KEEPALIVE_MS
    ) != DEFAULT_REMOTE_TEMP_KEEPALIVE_MS:
        raise cv.Invalid("lossnay does not support remote temperature keep-alive")
    fahrenheit_mode = config.get(CONF_FAHRENHEIT_SUPPORT_MODE, "disabled")
    fahrenheit_value = getattr(
        fahrenheit_mode,
        "enum_value",
        FAHRENHEIT_MODES.get(str(fahrenheit_mode).lower(), 0),
    )
    if fahrenheit_value:
        raise cv.Invalid("lossnay does not support Fahrenheit compatibility tables")
    if config.get(CONF_POWER_UNIT_IS_BTU, False):
        raise cv.Invalid("lossnay does not support power_unit_is_btu")
    if config.get(CONF_ERROR_CODE_TRANSLATIONS):
        raise cv.Invalid("lossnay does not support error_code_translations")
    boost_duration = config.get(CONF_BOOST_DURATION)
    if boost_duration is not None and getattr(boost_duration, "total_milliseconds", DEFAULT_BOOST_DURATION_MS) != DEFAULT_BOOST_DURATION_MS:
        raise cv.Invalid("lossnay does not support presets, so boost_duration cannot be changed")
    coil_duration = config.get(CONF_COIL_DRYING_DURATION)
    if coil_duration is not None and getattr(coil_duration, "total_seconds", DEFAULT_COIL_DRYING_DURATION_S) != DEFAULT_COIL_DRYING_DURATION_S:
        raise cv.Invalid("lossnay does not support coil drying, so coil_drying_duration cannot be changed")
    if config.get(CONF_COIL_DRYING_FAN_SPEED, "QUIET") != "QUIET":
        raise cv.Invalid("lossnay does not support coil drying, so coil_drying_fan_speed cannot be changed")
    if config.get(CONF_COIL_DRYING_VANE, "—") != "—":
        raise cv.Invalid("lossnay does not support coil drying, so coil_drying_vane cannot be changed")
    if config.get(CONF_AWAY_TEMPERATURE, 10.0) != 10.0:
        raise cv.Invalid("lossnay does not support presets, so away_temperature cannot be changed")

    return config

CONFIG_SCHEMA = (
    climate.climate_schema(CN105Climate)
    .extend(
        {
            cv.GenerateID(): cv.declare_id(CN105Climate),
            cv.GenerateID(CONF_UART_ID): cv.use_id(uart.UARTComponent),
            cv.Optional("baud_rate"): cv.invalid(
                "baud_rate' option is not supported anymore. Please add a separate UART component with baud_rate configured."
            ),
            cv.Optional("hardware_uart"): cv.invalid(
                "'hardware_uart' options is not supported anymore. Please add a separate UART component with the correct rx and tx pin."
            ),
            # cv.Optional(CONF_HARDWARE_UART, default="UART0"): valid_uart,
            cv.Optional(CONF_UPDATE_INTERVAL, default="2s"): _min_update_interval,
            cv.Optional(CONF_HORIZONTAL_SWING_SELECT): SELECT_SCHEMA,
            cv.Optional(CONF_VERTICAL_SWING_SELECT): SELECT_SCHEMA,
            cv.Optional(
                CONF_COMPRESSOR_FREQUENCY_SENSOR
            ): COMPRESSOR_FREQUENCY_SENSOR_SCHEMA,
            cv.Optional(CONF_INPUT_POWER_SENSOR): INPUT_POWER_SENSOR_SCHEMA,
            cv.Optional(CONF_KWH_SENSOR): KWH_SENSOR_SCHEMA,
            cv.Optional(CONF_RUNTIME_HOURS_SENSOR): RUNTIME_HOURS_SENSOR_SCHEMA,
            cv.Optional(
                CONF_OUTSIDE_AIR_TEMPERATURE_SENSOR
            ): OUTSIDE_AIR_TEMPERATURE_SENSOR_SCHEMA,
            cv.Optional(CONF_ISEE_SENSOR): ISEE_SENSOR_SCHEMA,
            cv.Optional(CONF_TARGET_HUMIDITY_SENSOR): TARGET_HUMIDITY_SENSOR_SCHEMA,
            cv.Optional(CONF_FUNCTIONS_SENSOR): FUNCTIONS_SENSOR_SCHEMA,
            cv.Optional(CONF_FUNCTIONS_BUTTON): FUNCTIONS_BUTTON_SCHEMA,
            cv.Optional(CONF_FUNCTIONS_SET_BUTTON): FUNCTIONS_BUTTON_SCHEMA,
            cv.Optional(CONF_FUNCTIONS_SET_CODE): FUNCTIONS_NUMBER_SCHEMA,
            cv.Optional(CONF_FUNCTIONS_SET_VALUE): FUNCTIONS_NUMBER_SCHEMA,
            cv.Optional(CONF_FAHRENHEIT_SUPPORT_MODE, default="disabled"): cv.enum(
                FAHRENHEIT_MODES, lower=True
            ),
            cv.Optional(CONF_MSZ_A24NA_SETPOINT_TABLE, default=False): cv.boolean,
            cv.Optional(
                CONF_STAGE_SENSOR
            ): STAGE_SENSOR_CONFIG_SCHEMA,  # Modifié pour le nouveau schéma
            cv.Optional(CONF_SUB_MODE_SENSOR): SUB_MODE_SENSOR_SCHEMA,
            cv.Optional(CONF_AUTO_SUB_MODE_SENSOR): AUTO_SUB_MODE_SENSOR_SCHEMA,
            cv.Optional(CONF_ERROR_CODE_SENSOR): ERROR_CODE_SENSOR_SCHEMA,
            cv.Optional(CONF_REMOTE_TEMP_SOURCE): REMOTE_TEMP_SOURCE_SCHEMA,
            cv.Optional(CONF_REMOTE_TEMP_TIMEOUT, default="never"): cv.All(
                cv.update_interval
            ),
            # Keep-alive interval for remote temperature (like Kumo does every ~20s)
            # Set to 0s to disable keep-alive
            cv.Optional(CONF_REMOTE_TEMP_KEEPALIVE_INTERVAL, default="20s"): cv.All(
                _keepalive_interval
            ),
            cv.Optional(CONF_DEBOUNCE_DELAY, default="100ms"): cv.All(
                cv.update_interval
            ),
            cv.Optional(CONF_CONNECTION_BOOTSTRAP_DELAY, default="10s"): cv.All(
                cv.update_interval
            ),
            cv.Optional(CONF_INSTALLER_MODE, default=False): cv.boolean,
            cv.Optional(CONF_LOSSNAY, default=False): cv.boolean,
            cv.Optional(
                CONF_HP_UP_TIME_CONNECTION_SENSOR
            ): HP_UP_TIME_CONNECTION_SENSOR_SCHEMA,
            cv.Optional(CONF_AIRFLOW_CONTROL_SELECT): SELECT_SCHEMA,
            cv.Optional(CONF_AIR_PURIFIER_SWITCH): HVAC_OPTION_SWITCH_SCHEMA,
            cv.Optional(CONF_NIGHT_MODE_SWITCH): HVAC_OPTION_SWITCH_SCHEMA,
            cv.Optional(CONF_DEFROST_SENSOR): binary_sensor.binary_sensor_schema(
                binary_sensor.BinarySensor,
                icon="mdi:snowflake-melt",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_CIRCULATOR_SWITCH): HVAC_OPTION_SWITCH_SCHEMA,
            cv.Optional(CONF_POWER_SAVING_SWITCH): HVAC_OPTION_SWITCH_SCHEMA,
            cv.Optional(CONF_COIL_DRYING_SWITCH): HVAC_OPTION_SWITCH_SCHEMA,
            cv.Optional(CONF_COIL_DRYING_SENSOR): binary_sensor.binary_sensor_schema(
                binary_sensor.BinarySensor,
                icon="mdi:fan-clock",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_COIL_DRYING_DURATION, default="15min"): cv.positive_time_period_seconds,
            cv.Optional(CONF_COIL_DRYING_FAN_SPEED, default="QUIET"): _coil_drying_fan_speed,
            cv.Optional(CONF_COIL_DRYING_VANE, default="—"): _coil_drying_vane,
            cv.Optional(CONF_TIMER_MODE_SENSOR): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:timer-outline",
            ),
            cv.Optional(CONF_ON_TIMER_SENSOR): sensor.sensor_schema(
                unit_of_measurement="min",
                icon="mdi:timer-plus-outline",
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_DURATION,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_OFF_TIMER_SENSOR): sensor.sensor_schema(
                unit_of_measurement="min",
                icon="mdi:timer-minus-outline",
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_DURATION,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_FILTER_DIRTY_SENSOR): binary_sensor.binary_sensor_schema(
                binary_sensor.BinarySensor,
                icon="mdi:air-filter",
                device_class="problem",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_CONNECTION_SENSOR): binary_sensor.binary_sensor_schema(
                binary_sensor.BinarySensor,
                device_class="connectivity",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:connection",
            ),
            cv.Optional(CONF_CHECKSUM_ERROR_SENSOR): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                icon="mdi:alert-circle-outline",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PACKET_DROP_SENSOR): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                icon="mdi:transit-connection-variant",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_RETRANSMISSION_SENSOR): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                icon="mdi:repeat",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_LEVEL_SHIFTER_ENABLE_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_NATIVE_FIVE_SPEED_FAN, default=False): cv.boolean,
            cv.Optional(CONF_HARDWARE_SETTINGS): HARDWARE_SETTING_SCHEMA,
            cv.Optional(
                CONF_REMOTE_TEMPERATURE_CONTROL_SENSOR
            ): REMOTE_TEMPERATURE_CONTROL_SENSOR_SCHEMA,
            #cv.Optional(CONF_REMOTE_TEMPERATURE_MARGIN, default=0.4): cv.positive_float,
            cv.Optional(CONF_POWER_UNIT_IS_BTU, default=False): cv.boolean,
            cv.Optional(CONF_UART_PORT): cv.int_range(min=0, max=2),
            cv.Optional(CONF_BOOST_DURATION, default="15min"): _positive_time_period,
            cv.Optional(CONF_AWAY_TEMPERATURE, default=10.0): cv.All(
                cv.temperature, cv.float_range(min=10.0, max=25.0)
            ),
            cv.Optional(CONF_BOOST_REMAINING_SENSOR): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                icon="mdi:timer-sand",
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_DURATION,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BOOST_REMAINING_TIME_SENSOR): text_sensor.text_sensor_schema(
                icon="mdi:timer-sand",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(
                CONF_ERROR_CODE_TRANSLATIONS, default={}
            ): ERROR_CODE_TRANSLATIONS_SCHEMA,
            cv.Optional(CONF_MODEL): _nonempty_string,
            cv.Optional(CONF_CAPABILITIES): CAPABILITY_SCHEMA,
            cv.Optional(CONF_STRICT_CAPABILITIES, default=False): cv.boolean,
            # mode/fan_mode/swing_mode defaults depend on the profile and are applied in to_code.
            cv.Optional(CONF_SUPPORTS, default={}): cv.Schema(
                {
                    cv.Optional(
                        CONF_MODE
                    ): cv.ensure_list(climate.validate_climate_mode),
                    cv.Optional(
                        CONF_FAN_MODE
                    ): cv.ensure_list(climate.validate_climate_fan_mode),
                    cv.Optional(
                        CONF_CUSTOM_FAN_MODE, default=DEFAULT_CUSTOM_FAN_MODES
                    ): _custom_fan_modes,
                    cv.Optional(
                        CONF_SWING_MODE
                    ): cv.ensure_list(climate.validate_climate_swing_mode),
                    cv.Optional(CONF_DUAL_SETPOINT, default=False): cv.boolean,
                    cv.Optional(CONF_RESTORE_SETPOINTS, default=False): cv.boolean,
                    cv.Optional(CONF_SUPPORTS_HORIZONTAL_VANE_MODE): _horizontal_vane_options,
                    cv.Optional(CONF_HORIZONTAL_VANES, default=1): cv.int_range(
                        min=1, max=2
                    ),
                    cv.Optional(CONF_VANE_TYPE, default="standard"): cv.enum(
                        VANE_TYPES, lower=True
                    ),
                }
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)

CONFIG_SCHEMA = cv.All(
    CONFIG_SCHEMA,
    validate_config_dependencies,
    validate_capability_config,
    validate_lossnay_config,
)


@coroutine
def to_code(config):
    uart_id_object = config[CONF_UART_ID]
    uart_var = yield cg.get_variable(uart_id_object)
    var = cg.new_Pvariable(config[CONF_ID], uart_var, config[CONF_LOSSNAY])

    cg.add(var.set_installer_mode(config[CONF_INSTALLER_MODE]))
    cg.add(var.set_power_unit_is_btu(config[CONF_POWER_UNIT_IS_BTU]))

    cg.add(uart_var.set_data_bits(8))
    cg.add(uart_var.set_parity(UARTParityOptions.UART_CONFIG_PARITY_EVEN))
    cg.add(uart_var.set_stop_bits(1))

    uart_id_str_for_lookup = str(uart_id_object)
    tx_pin, rx_pin = get_uart_pins_from_config(CORE.config, uart_id_str_for_lookup)
    cg.add(var.set_tx_rx_pins(tx_pin, rx_pin))
    if CONF_UART_PORT in config:
        cg.add(var.set_uart_port(config[CONF_UART_PORT]))
    else:
        uart_port_index = get_uart_port_index(CORE.config, uart_id_str_for_lookup)
        cg.add(var.set_uart_port(uart_port_index))

    cg.add(
        var.set_boost_duration(
            int(config[CONF_BOOST_DURATION].total_milliseconds)
        )
    )
    cg.add(var.set_away_temperature(config[CONF_AWAY_TEMPERATURE]))

    translations = config.get(CONF_ERROR_CODE_TRANSLATIONS, {})
    if translations:
        map_entries = ", ".join(
            f"{{{int(code)}, {cg.safe_exp(str(label))}}}"
            for code, label in translations.items()
        )
        cg.add(
            var.set_error_code_translations(
                cg.RawExpression(f"std::map<uint8_t, std::string>{{{map_entries}}}")
            )
        )

    manifest = resolve_capability_manifest(config)
    if config.get(CONF_MODEL) or CONF_CAPABILITIES in config or config.get(CONF_STRICT_CAPABILITIES, False):
        cg.add(
            var.set_model_capability_manifest(
                cg.safe_exp(manifest["model"]),
                cg.safe_exp(manifest["source"]),
                manifest["supported_mask"],
                manifest["denied_mask"],
                config.get(CONF_STRICT_CAPABILITIES, False),
            )
        )

    # Empty list means use all options from WIDEVANE_MAP (C++ is source of truth)
    horizontal_vane_options = []

    if CONF_SUPPORTS in config:
        supports = config[CONF_SUPPORTS]
        traits = var.config_traits()

        # Configurer les modes supportés
        supported_modes = supports.get(
            CONF_MODE,
            LOSSNAY_CLIMATE_MODES if config[CONF_LOSSNAY] else DEFAULT_CLIMATE_MODES,
        )
        for mode_str in supported_modes:
            if mode_str == "OFF":
                continue
            if mode_str in climate.CLIMATE_MODES:
                cg.add(traits.add_supported_mode(climate.CLIMATE_MODES[mode_str]))

        # Configure the horizontal vane options
        horizontal_vane_options = supports.get(CONF_SUPPORTS_HORIZONTAL_VANE_MODE, [])

        # Set vane type (legacy "horizontal_vanes: 2" maps to split_horizontal)
        vane_type_conf = supports.get(CONF_VANE_TYPE, "standard")
        if vane_type_conf == "standard" and supports.get(CONF_HORIZONTAL_VANES, 1) > 1:
            vane_type_conf = "split_horizontal"
        if vane_type_conf == "split_vertical":
            _LOGGER.warning(
                "vane_type: split_vertical is not implemented yet and behaves like 'standard' (see issue #505)"
            )
        vane_type_val = VANE_TYPES.get(vane_type_conf, 0)
        vane_type_enum = cg.RawExpression(
            f"static_cast<esphome::CN105Climate::VaneType>({vane_type_val})"
        )
        cg.add(var.set_vane_type(vane_type_enum))

        # Set dual setpoint support via YAML (default: False if missing)
        # Note: this enables global dual setpoint support in HA.
        yaml_dual = supports.get(CONF_DUAL_SETPOINT, False)

        if yaml_dual:
            dual_flag = cg.RawExpression(
                "climate::CLIMATE_REQUIRES_TWO_POINT_TARGET_TEMPERATURE"
            )
            cg.add(traits.add_feature_flags(dual_flag))

        # Note: If yaml_dual is False, we simply do NOT set the dual-setpoint trait.

        # Opt-in: persist the HEAT_COOL band (mode + low/high) to flash and restore
        # it on boot, so a reboot/OTA doesn't drop the unit back to hardware AUTO.
        cg.add(var.set_restore_setpoints(supports.get(CONF_RESTORE_SETPOINTS, False)))
        # ESPHome's default behavior for modes like COOL/HEAT is to enable single-point target temperature.
        # We don't need to explicitly force single-point or clear the dual flag (it's off by default).

        for fan_mode_str in supports.get(
            CONF_FAN_MODE,
            LOSSNAY_FAN_MODES if config[CONF_LOSSNAY] else DEFAULT_FAN_MODES,
        ):
            if fan_mode_str in climate.CLIMATE_FAN_MODES:
                cg.add(
                    traits.add_supported_fan_mode(
                        climate.CLIMATE_FAN_MODES[fan_mode_str]
                    )
                )
        if not config[CONF_LOSSNAY]:
            custom_fans = supports.get(CONF_CUSTOM_FAN_MODE, DEFAULT_CUSTOM_FAN_MODES)
            if custom_fans:
                cg.add(var.set_supported_custom_fan_modes_config(list(custom_fans)))
        for swing_mode_str in supports.get(
            CONF_SWING_MODE,
            [] if config[CONF_LOSSNAY] else DEFAULT_SWING_MODES,
        ):
            if swing_mode_str in climate.CLIMATE_SWING_MODES:
                cg.add(
                    traits.add_supported_swing_mode(
                        climate.CLIMATE_SWING_MODES[swing_mode_str]
                    )
                )

    cg.add(var.set_remote_temp_timeout(config[CONF_REMOTE_TEMP_TIMEOUT]))
    cg.add(var.set_debounce_delay(config[CONF_DEBOUNCE_DELAY]))
    cg.add(
        var.set_remote_temp_keepalive_interval(
            int(config[CONF_REMOTE_TEMP_KEEPALIVE_INTERVAL].total_milliseconds)
        )
    )
    cg.add(
        var.set_connection_bootstrap_delay(
            int(config[CONF_CONNECTION_BOOTSTRAP_DELAY].total_milliseconds)
        )
    )

    # --- Configuration des entités optionnelles (style original) ---
    if CONF_HORIZONTAL_SWING_SELECT in config:
        conf_item = config[CONF_HORIZONTAL_SWING_SELECT]
        # new_select s'occupe de l'enregistrement. options=[] est important.
        swing_select_var = yield select.new_select(conf_item, options=[])
        if horizontal_vane_options:
            # cg.safe_exp() escapes quotes/backslashes before we emit the
            # explicit vector type expected by set_horizontal_vane_select().
            options_vector = cg.RawExpression(
                "std::vector<std::string>" + str(cg.safe_exp(list(horizontal_vane_options)))
            )
        else:
            options_vector = cg.RawExpression("std::vector<std::string>{}")
        cg.add(var.set_horizontal_vane_select(swing_select_var, options_vector))

    if CONF_VERTICAL_SWING_SELECT in config:
        conf_item = config[CONF_VERTICAL_SWING_SELECT]
        swing_select_var = yield select.new_select(conf_item, options=[])
        cg.add(var.set_vertical_vane_select(swing_select_var))

    if CONF_AIRFLOW_CONTROL_SELECT in config:
        conf_item = config[CONF_AIRFLOW_CONTROL_SELECT]
        control_select_var = yield select.new_select(conf_item, options=[])
        cg.add(var.set_airflow_control_select(control_select_var))

    # Pour les capteurs, text_sensors, etc., utiliser la méthode .new_... standard
    # Ces fonctions s'occupent de l'enregistrement du composant.
    if CONF_COMPRESSOR_FREQUENCY_SENSOR in config:
        # conf = config[CONF_COMPRESSOR_FREQUENCY_SENSOR] # 'conf' est déjà utilisé comme argument de to_code
        # conf["force_update"] = False # Ceci était dans votre code original, le garder si pertinent
        conf_item = config[CONF_COMPRESSOR_FREQUENCY_SENSOR]
        if (
            "force_update" not in conf_item
        ):  # S'assurer de ne pas l'écraser si l'user l'a mis
            conf_item["force_update"] = False
        sensor_var = yield sensor.new_sensor(conf_item)
        cg.add(var.set_compressor_frequency_sensor(sensor_var))
        if CONF_REPORT_WHEN_IDLE in conf_item:
            cg.add(var.set_compressor_frequency_report_when_idle(conf_item[CONF_REPORT_WHEN_IDLE]))

    if CONF_INPUT_POWER_SENSOR in config:
        conf_item = config[CONF_INPUT_POWER_SENSOR]
        if "force_update" not in conf_item:
            conf_item["force_update"] = False
        sensor_var = yield sensor.new_sensor(conf_item)
        cg.add(var.set_input_power_sensor(sensor_var))

    if CONF_KWH_SENSOR in config:
        conf_item = config[CONF_KWH_SENSOR]
        if "force_update" not in conf_item:
            conf_item["force_update"] = False
        sensor_var = yield sensor.new_sensor(conf_item)
        cg.add(var.set_kwh_sensor(sensor_var))

    if CONF_RUNTIME_HOURS_SENSOR in config:
        conf_item = config[CONF_RUNTIME_HOURS_SENSOR]
        if "force_update" not in conf_item:
            conf_item["force_update"] = False
        sensor_var = yield sensor.new_sensor(conf_item)
        cg.add(var.set_runtime_hours_sensor(sensor_var))

    if CONF_OUTSIDE_AIR_TEMPERATURE_SENSOR in config:
        conf_item = config[CONF_OUTSIDE_AIR_TEMPERATURE_SENSOR]
        if "force_update" not in conf_item:
            conf_item["force_update"] = False
        sensor_var = yield sensor.new_sensor(conf_item)
        cg.add(var.set_outside_air_temperature_sensor(sensor_var))

    if CONF_ISEE_SENSOR in config:
        bsensor_var = yield binary_sensor.new_binary_sensor(config[CONF_ISEE_SENSOR])
        cg.add(var.set_isee_sensor(bsensor_var))

    if CONF_TARGET_HUMIDITY_SENSOR in config:
        conf_item = config[CONF_TARGET_HUMIDITY_SENSOR]
        if "force_update" not in conf_item:
            conf_item["force_update"] = False
        sensor_var = yield sensor.new_sensor(conf_item)
        cg.add(var.set_target_humidity_sensor(sensor_var))

    if CONF_FUNCTIONS_SENSOR in config:
        tsensor_var = yield text_sensor.new_text_sensor(config[CONF_FUNCTIONS_SENSOR])
        cg.add(var.set_functions_sensor(tsensor_var))

    if CONF_FUNCTIONS_BUTTON in config:
        button_var = yield button.new_button(config[CONF_FUNCTIONS_BUTTON])
        cg.add(var.set_functions_get_button(button_var))

    if CONF_FUNCTIONS_SET_BUTTON in config:
        button_var = yield button.new_button(config[CONF_FUNCTIONS_SET_BUTTON])
        cg.add(var.set_functions_set_button(button_var))

    if CONF_FUNCTIONS_SET_CODE in config:
        conf_item = config[CONF_FUNCTIONS_SET_CODE]
        number_var = yield number.new_number(
            conf_item, min_value=101.0, max_value=128.0, step=1.0
        )
        cg.add(var.set_functions_set_code(number_var))

    if CONF_FUNCTIONS_SET_VALUE in config:
        conf_item = config[CONF_FUNCTIONS_SET_VALUE]
        number_var = yield number.new_number(
            conf_item, min_value=1.0, max_value=3.0, step=1.0
        )
        cg.add(var.set_functions_set_value(number_var))

    if CONF_AIR_PURIFIER_SWITCH in config:
        switch_var = yield switch.new_switch(config[CONF_AIR_PURIFIER_SWITCH])
        cg.add(var.set_air_purifier_switch(switch_var))

    if CONF_NIGHT_MODE_SWITCH in config:
        switch_var = yield switch.new_switch(config[CONF_NIGHT_MODE_SWITCH])
        cg.add(var.set_night_mode_switch(switch_var))

    if CONF_DEFROST_SENSOR in config:
        sensor_var = yield binary_sensor.new_binary_sensor(config[CONF_DEFROST_SENSOR])
        cg.add(var.set_defrost_sensor(sensor_var))

    if CONF_CIRCULATOR_SWITCH in config:
        switch_var = yield switch.new_switch(config[CONF_CIRCULATOR_SWITCH])
        cg.add(var.set_circulator_switch(switch_var))

    if CONF_POWER_SAVING_SWITCH in config:
        switch_var = yield switch.new_switch(config[CONF_POWER_SAVING_SWITCH])
        cg.add(var.set_power_saving_switch(switch_var))

    if CONF_COIL_DRYING_DURATION in config:
        cg.add(
            var.set_coil_drying_duration(
                int(config[CONF_COIL_DRYING_DURATION].total_seconds)
            )
        )

    if CONF_COIL_DRYING_SWITCH in config:
        switch_var = yield switch.new_switch(config[CONF_COIL_DRYING_SWITCH])
        cg.add(var.set_coil_drying_switch(switch_var))

    if CONF_COIL_DRYING_SENSOR in config:
        sensor_var = yield binary_sensor.new_binary_sensor(config[CONF_COIL_DRYING_SENSOR])
        cg.add(var.set_coil_drying_sensor(sensor_var))

    if CONF_COIL_DRYING_FAN_SPEED in config:
        cg.add(var.set_coil_drying_fan_speed(config[CONF_COIL_DRYING_FAN_SPEED]))

    if CONF_COIL_DRYING_VANE in config:
        cg.add(var.set_coil_drying_vane(config[CONF_COIL_DRYING_VANE]))

    if CONF_TIMER_MODE_SENSOR in config:
        tsensor_var = yield text_sensor.new_text_sensor(config[CONF_TIMER_MODE_SENSOR])
        cg.add(var.set_timer_mode_sensor(tsensor_var))

    if CONF_ON_TIMER_SENSOR in config:
        sensor_var = yield sensor.new_sensor(config[CONF_ON_TIMER_SENSOR])
        cg.add(var.set_on_timer_sensor(sensor_var))

    if CONF_OFF_TIMER_SENSOR in config:
        sensor_var = yield sensor.new_sensor(config[CONF_OFF_TIMER_SENSOR])
        cg.add(var.set_off_timer_sensor(sensor_var))

    if CONF_FILTER_DIRTY_SENSOR in config:
        sensor_var = yield binary_sensor.new_binary_sensor(config[CONF_FILTER_DIRTY_SENSOR])
        cg.add(var.set_filter_dirty_sensor(sensor_var))

    if CONF_CONNECTION_SENSOR in config:
        sensor_var = yield binary_sensor.new_binary_sensor(config[CONF_CONNECTION_SENSOR])
        cg.add(var.set_connection_sensor(sensor_var))

    if CONF_CHECKSUM_ERROR_SENSOR in config:
        sensor_var = yield sensor.new_sensor(config[CONF_CHECKSUM_ERROR_SENSOR])
        cg.add(var.set_checksum_error_sensor(sensor_var))

    if CONF_PACKET_DROP_SENSOR in config:
        sensor_var = yield sensor.new_sensor(config[CONF_PACKET_DROP_SENSOR])
        cg.add(var.set_packet_drop_sensor(sensor_var))

    if CONF_RETRANSMISSION_SENSOR in config:
        sensor_var = yield sensor.new_sensor(config[CONF_RETRANSMISSION_SENSOR])
        cg.add(var.set_retransmission_sensor(sensor_var))

    if CONF_LEVEL_SHIFTER_ENABLE_PIN in config:
        pin = yield cg.gpio_pin_expression(config[CONF_LEVEL_SHIFTER_ENABLE_PIN])
        cg.add(var.set_level_shifter_enable_pin(pin))

    if CONF_NATIVE_FIVE_SPEED_FAN in config:
        cg.add(var.set_native_five_speed_fan(config[CONF_NATIVE_FIVE_SPEED_FAN]))

    if CONF_BOOST_REMAINING_SENSOR in config:
        sensor_var = yield sensor.new_sensor(config[CONF_BOOST_REMAINING_SENSOR])
        cg.add(var.set_boost_remaining_sensor(sensor_var))

    if CONF_BOOST_REMAINING_TIME_SENSOR in config:
        tsensor_var = yield text_sensor.new_text_sensor(config[CONF_BOOST_REMAINING_TIME_SENSOR])
        cg.add(var.set_boost_remaining_time_sensor(tsensor_var))

    # Set Fahrenheit compatibility mode (cast int to FahrenheitMode enum)
    # The enum validator returns the integer value from FAHRENHEIT_MODES dict
    fahrenheit_mode = config.get(CONF_FAHRENHEIT_SUPPORT_MODE)
    # Ensure it's an integer (enum validator should already return int, but be explicit)
    fahrenheit_value = (
        int(fahrenheit_mode)
        if isinstance(fahrenheit_mode, int)
        else FAHRENHEIT_MODES.get(str(fahrenheit_mode).lower(), 0)
    )
    mode_enum = cg.RawExpression(
        f"static_cast<esphome::FahrenheitMode>({fahrenheit_value})"
    )
    cg.add(var.set_use_fahrenheit_support_mode(mode_enum))

    cg.add(var.set_msz_a24na_setpoint_table(config[CONF_MSZ_A24NA_SETPOINT_TABLE]))

    # --- TRAITEMENT POUR STAGE_SENSOR AVEC LA NOUVELLE OPTION ---
    if CONF_STAGE_SENSOR in config:
        conf_stage_dict = config[CONF_STAGE_SENSOR]
        # new_text_sensor gère la création et l'enregistrement de base du text_sensor
        stage_ts_var = yield text_sensor.new_text_sensor(conf_stage_dict)
        cg.add(var.set_stage_sensor(stage_ts_var))

        # Passer l'option de fallback au C++
        if conf_stage_dict.get(CONF_USE_AS_OPERATING_FALLBACK, False):
            cg.add(var.set_use_stage_for_operating_status(True))
    # --- FIN DU TRAITEMENT POUR STAGE_SENSOR ---

    if CONF_REMOTE_TEMPERATURE_CONTROL_SENSOR in config:
        conf = config[CONF_REMOTE_TEMPERATURE_CONTROL_SENSOR]
        sensor_var = yield binary_sensor.new_binary_sensor(conf)
        cg.add(var.set_remote_temperature_control_sensor(sensor_var))
        if CONF_TEMPERATURE_MARGIN in conf:
            cg.add(var.set_remote_temperature_margin(conf[CONF_TEMPERATURE_MARGIN]))

    #if CONF_REMOTE_TEMPERATURE_MARGIN in config:
    #    cg.add(var.set_remote_temperature_margin(config[CONF_REMOTE_TEMPERATURE_MARGIN]))

    if CONF_SUB_MODE_SENSOR in config:
        tsensor_var = yield text_sensor.new_text_sensor(config[CONF_SUB_MODE_SENSOR])
        cg.add(var.set_sub_mode_sensor(tsensor_var))

    if CONF_AUTO_SUB_MODE_SENSOR in config:
        tsensor_var = yield text_sensor.new_text_sensor(
            config[CONF_AUTO_SUB_MODE_SENSOR]
        )
        cg.add(var.set_auto_sub_mode_sensor(tsensor_var))

    if CONF_ERROR_CODE_SENSOR in config:
        tsensor_var = yield text_sensor.new_text_sensor(
            config[CONF_ERROR_CODE_SENSOR]
        )
        cg.add(var.set_error_code_sensor(tsensor_var))

    if CONF_REMOTE_TEMP_SOURCE in config:
        rts_config = config[CONF_REMOTE_TEMP_SOURCE]
        source_sensor = yield cg.get_variable(rts_config[CONF_REMOTE_TEMP_SOURCE_SENSOR_ID])
        cg.add(var.set_remote_temp_source(source_sensor))
        if CONF_REMOTE_TEMP_SOURCE_INFO in rts_config:
            info_sensor = yield text_sensor.new_text_sensor(rts_config[CONF_REMOTE_TEMP_SOURCE_INFO])
            cg.add(var.set_remote_temp_source_info_sensor(info_sensor))

    if CONF_HP_UP_TIME_CONNECTION_SENSOR in config:
        conf = config[CONF_HP_UP_TIME_CONNECTION_SENSOR]
        hp_connection_sensor_ = yield sensor.new_sensor(conf)
        yield cg.register_component(hp_connection_sensor_, conf)
        cg.add(var.set_hp_uptime_connection_sensor(hp_connection_sensor_))

    if CONF_HARDWARE_SETTINGS in config:
        hw_config = config[CONF_HARDWARE_SETTINGS]

        # Extract and set the update interval
        interval_ms = int(hw_config[CONF_UPDATE_INTERVAL].total_milliseconds)
        cg.add(var.set_hardware_settings_interval(interval_ms))

        # Iterate over the list of hardware settings
        for setting_conf in hw_config[CONF_HARDWARE_SETTINGS_LIST]:
            code = setting_conf[CONF_CODE]
            options_map = setting_conf[CONF_OPTIONS]

            # Build inline initializer list for std::map: {{key1, "val1"}, {key2, "val2"}, ...}
            # cg.safe_exp() escapes quotes/backslashes in user-supplied labels.
            map_entries = ", ".join(
                [f'{{{val}, {cg.safe_exp(label)}}}' for val, label in options_map.items()]
            )
            map_expr = cg.RawExpression(f"std::map<int, std::string>{{{map_entries}}}")

            setting_var = cg.new_Pvariable(setting_conf[CONF_ID], code, map_expr)

            # Extract options list sorted by key (1, 2, 3...) to ensure consistent order
            options_list = [options_map[k] for k in sorted(options_map.keys())]
            yield select.register_select(
                setting_var, setting_conf, options=options_list
            )

            cg.add(var.add_hardware_setting(setting_var))

    yield cg.register_component(var, config)
    yield climate.register_climate(var, config)
