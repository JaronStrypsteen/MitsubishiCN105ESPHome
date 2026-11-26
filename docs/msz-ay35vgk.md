# MSZ-AY35VGK verification guide

This guide is for the exact model `MSZ-AY35VGK`. Evidence collected from `MSZ-AY35VGKP` is **not** treated as proof that a feature exists on the non-P model.

The tuned example is [`examples/msz-ay35vgk/msz-ay35vgk.yaml`](../examples/msz-ay35vgk/msz-ay35vgk.yaml). Copy it into your ESPHome config directory, provide `secrets.yaml` keys `wifi_ssid`, `wifi_password`, `ota_pwd`, and `encryption_key`, and adjust the UART pins if your board uses different GPIOs.

## What is enabled by default

- Standard modes, fan speeds, vertical swing, and the `FULL` custom fan mode.
- `BOOST`, `SLEEP`, and `ECO` presets. These are registered automatically for non-Lossnay climate entities. `SLEEP` drives the Mitsubishi NIGHT MODE bit; `BOOST` restores the saved fan/setpoint after 15 minutes.
- `night_mode_switch`, disabled by default in Home Assistant. Enable it after you confirm the `0x42` packet changes when you toggle it.
- Diagnostic entities, disabled by default: defrost, error code, runtime hours, input power, kWh, stage, sub-mode, auto sub-mode, outside air temperature, and compressor frequency.

`dual_setpoint` is deliberately `false`. The AY units generally use one hardware setpoint; the dual setpoint option creates a synthetic HEAT_COOL band and is not needed for normal AY operation.

## What is deliberately omitted

Do not enable these on `MSZ-AY35VGK` without model-specific evidence:

- `air_purifier_switch` — purifier support has only been observed on VGKP models.
- `isee_sensor` and `supports.airflow_control` — 3D i-See is not evidenced on this model.
- `lossnay: true`, `msz_a24na_setpoint_table: true`, `target_humidity_sensor`, `split_vertical`, and `supports.horizontal_vane_mode` with non-standard options.
- `hardware_settings` writes. Read-only investigation is fine; writing unknown function codes can switch some units off.

## Raw-log verification

Enable full frame logging temporarily:

```yaml
logger:
  hardware_uart: UART1
  level: DEBUG
  logs:
    CN105: DEBUG
    READ: DEBUG
    WRITE: DEBUG
    WRITE_SETTINGS: DEBUG
    Decoder: DEBUG
```

Flash, then watch the ESPHome logs. Frames look like:

```text
[D][READ]: |FC 62 01 30 10 |->[02 00 00 01 0B 19 00 00 00 00 80 AD 00 00 00 00 ](xx) <RESPONSE:Settings>
```

Use these checks one feature at a time:

| Feature | What to look for | Pass criteria |
| ------- | ---------------- | ------------- |
| `night_mode_switch` | `0x42` responses and `WRITE_SETTINGS` run-state packets | Bytes change in the 0x42 payload when the switch is toggled; the indoor unit responds/acknowledges. |
| `defrost_sensor` | `0x09` responses | Sub-mode byte changes during defrost/preheat. A persistent `0x01` is the filter reminder and should **not** be reported as defrost. |
| `error_code_sensor` | `0x04` responses | Payload bytes 4 and 5 are meaningful; a bare `0x80` bit is only an availability flag. |
| `runtime_hours_sensor` | `0x03` responses | Payload bytes 11–13 are non-zero or change over time. |
| `compressor_frequency_sensor` | `0x06` responses | Payload byte 3 changes while the compressor runs. |
| `input_power_sensor` / `kwh_sensor` | `0x06` and `0x09` responses | Values change with operating state; if they stay empty, disable the entity. |
| `stage_sensor`, `sub_mode_sensor`, `auto_sub_mode_sensor` | `0x09` responses | Values change between operating stages or auto sub-modes. |
| `outside_air_temperature_sensor` | `0x03` responses | Payload byte 5 is a plausible encoded temperature. VGKP evidence does not guarantee this on VGK. |
| Vertical vane | `0x02` settings responses | Vane byte changes after a vane command and matches the physical vane. |

If an entity never produces a state after several complete cycles, leave it disabled rather than exposing an empty or misleading entity. Return logging to `INFO` after verification; `DEBUG` frame logging adds load to the ESP.

## Preset checks

1. Set a normal setpoint and fan speed.
2. Select `BOOST`. Confirm a `WRITE_SETTINGS` frame with `FULL`/high fan and the boosted setpoint, then an ACK.
3. Wait 15 minutes or select `SLEEP`/`ECO`/`NONE`. Confirm a second `WRITE_SETTINGS` frame restores the original fan and setpoint.
4. Select `SLEEP`. Confirm the run-state packet sets the night-mode bit and the `night_mode_switch` entity follows it.
5. Select `ECO` or `NONE`. Confirm the night-mode bit is cleared and the switch entity follows it.

## Building

From the repository root:

```sh
esphome compile examples/msz-ay35vgk/msz-ay35vgk.yaml
```

The CI build matrix includes this config on ESPHome `2025.11.0` and `stable`.
