# Badger Water Meter ESPHome Component

ESPHome external component for reading Badger water meters via the **Sensus UI-1203** wired encoder protocol.

## Hardware

Any ESP32 or ESP8266 board with 2 available GPIO pins. The meter runs at 3.3V.

## Wiring

The meter has a 3-wire encoder output. The "clock" is achieved by toggling
power to the meter — each power cycle clocks out one bit. You can toggle
from either the high side or the low side:

### Option A: High-side switching (ESP32 or boards without boot-pin constraints)

| Wire   | Function      | Connect To            |
|--------|---------------|-----------------------|
| RED    | Clock / Power | ESP GPIO (output)     |
| GREEN  | Data          | ESP GPIO (input)      |
| BLACK  | Ground        | ESP GND               |

The `clock_pin` GPIO drives RED. HIGH = meter powered, LOW = meter off.

### Option B: Low-side switching (ESP8266 / ESP-01)

| Wire   | Function      | Connect To            |
|--------|---------------|-----------------------|
| RED    | Clock / Power | 3.3V (constant)       |
| GREEN  | Data          | ESP GPIO0 (input)     |
| BLACK  | Ground        | ESP GPIO2 (output)    |

The `clock_pin` GPIO drives BLACK (ground side). LOW = meter powered,
HIGH = meter off. This avoids ESP8266 boot issues since GPIO0/GPIO2 are
pulled high at startup, keeping the meter unpowered during boot.

**Important:** When using Option B, set `inverted: true` on the clock pin
in your YAML config so the logic is correct.

**Data line:** Open-collector — the meter pulls it low to signal a bit.
Use the internal pull-up or add an external 10k pull-up resistor.

## Protocol Summary

- There is no separate clock signal; power toggling IS the clock
- Each power cycle (off → on → read data pin) clocks out one bit
- Each byte: start bit (0) + 7 data bits (LSB first) + even parity + stop bit (1)
- Meter needs ~3 seconds of continuous power before first transmission
- Transmits ASCII string terminated by `\r`, e.g. `R226107229550`
- First digits after 'R' = meter reading, remaining = meter ID

## Installation

Add this to your ESPHome YAML to use the component directly from GitHub:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/defl/esphome_ui1203
      ref: main
    components: [badger_meter]
```

Or clone the repo and use a local path:

```yaml
external_components:
  - source:
      type: local
      path: /path/to/esphome_ui1203/components
```

## Configuration

```yaml
badger_meter:
  clock_pin: GPIO16
  data_pin: GPIO17
  power_up_time: 3s   # optional, default 3s

sensor:
  - platform: badger_meter
    meter_reading:
      name: "Water Meter Reading"
    raw_value:
      name: "Water Meter Raw Value"

text_sensor:
  - platform: badger_meter
    raw_string:
      name: "Water Meter Raw String"
    meter_id:
      name: "Water Meter ID"
```

## Sensors

| Sensor          | Type   | Description                              |
|-----------------|--------|------------------------------------------|
| `meter_reading` | sensor | Parsed numeric reading from the meter    |
| `raw_value`     | sensor | Full numeric value (all digits after 'R')|
| `raw_string`    | text   | Complete raw ASCII string from meter     |
| `meter_id`      | text   | Meter serial/ID (trailing digits)        |

## Unit Conversion

The raw reading unit depends on your meter's configuration (gallons, cubic feet, cubic meters). Apply filters in YAML:

```yaml
sensor:
  - platform: badger_meter
    meter_reading:
      name: "Water Usage"
      filters:
        - multiply: 0.00378541  # gallons to cubic meters
```

## Tuning

- **Reading digits**: The parser defaults to 7 digits for the reading. If your meter uses a different split, edit the `reading_digits` value in `badger_meter.cpp`.
- **Power-up time**: Some meters need more than 3 seconds. Increase `power_up_time` if you get empty reads.

## License

This project uses the same dual-license as ESPHome:

- **MIT** — Python code and all other parts
- **GPL-3.0** — C++/runtime code (`.h`, `.cpp` files)

See [LICENSE](LICENSE) for full text.

## Credits

Protocol implementation based on:
- [kmeter](https://github.com/rszimm/kmeter) by rszimm — original Linux kernel module implementation
- [sensus_protocol_lib](https://github.com/michlv/sensus_protocol_lib) by michlv — Arduino/ESP8266 port
