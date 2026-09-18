# Badger Water Meter ESPHome Component

ESPHome external component for reading Badger water meters over the **Sensus UI-1203** wired
encoder interface.

> **Status: diagnostic.** No reading has been decoded from real hardware yet. The component
> captures the data line and tries to decode it; the framing below is what the reference
> implementations use, not something a vendor datasheet confirmed. See "What is actually known".

## Hardware

Any ESP32 or ESP8266 board with one free GPIO for data, plus a second one only if the ESP is to
power the meter.

## Wiring

The encoder has three wires:

| Wire | Function | Connect to |
|------|----------|-----------|
| RED | Power, and the clock — toggling it shifts out bits | 5 V through a switch the ESP drives |
| GREEN | Data, open-collector | ESP GPIO (input) + external 4.7k–10k pull-up |
| BLACK | Ground | ESP GND |

Those colours are Badger's own, from the HR-E LCD manual's "Encoder Cable with Flying Lead"
table: `RED - Power`, `GREEN - Data`, `BLACK - Ground`. Badger's documentation uses no white
conductor anywhere on this meter — its other outputs are scaled (red +, black −), unscaled
(green +, black −) and 4-20 mA (red, black). **If the cable in front of you has a white wire,
establish what it is before assuming it is data.**

**RED wants 5 V.** An ESP32 GPIO at 3.3 V is marginal at best; drive a high-side switch or level
shifter from the 5 V rail instead, keeping the ESP able to toggle it.

**Do not configure `clock_pin` when the meter has its own supply.** An ESPHome output pin
initialises LOW, so naming the pin would pull the supply rail to ground. Left out of the config
the pin is never touched. Configured, it is driven HIGH and held there, powering the meter.

**Data line:** open-collector — the meter pulls it low. Use the internal pull-up or a 10k to
the ESP's rail.

## What is actually known

- No manufacturer timing or electrical specification is published for this meter.
- Everything else here comes from [kmeter](https://github.com/rszimm/kmeter) and
  [sensus_protocol_lib](https://github.com/michlv/sensus_protocol_lib), neither validated
  against an E-Series ultrasonic.
- **The interface is synchronous and reader-clocked.** Badger describes the HR-E as a "3-wire
  synchronous signal type"; the reader powers the register through RED and toggles it to shift
  out one bit per cycle, at roughly 1 ms/bit. A powered but unclocked meter says nothing, so
  passive listening cannot work — `mode: passive` exists only to characterise the line.
- kmeter also notes that holding the clock **low for about a second resets the register's send
  buffer**, which is how a read is started from the beginning of the message.
- Framing, as implemented: start (0), 7 data bits LSB-first, even parity, stop (1); ASCII
  terminated by `\r`; data inverted (LOW = 1) — that last one confirmed on hardware, which
  rejected kmeter's non-inverted reading with stop-bit errors.

## How the capture works

`read_interval` (or the `request_read()` lambda) arms the component. Arming is non-blocking: the
data pin is sampled once per loop until it moves, for up to 8 s. The first edge starts a blocking
capture that records every transition with microsecond offsets until the burst ends (an idle gap),
the window expires, or the buffer fills.

It then logs the transition list, a 50 µs-bucket pulse-width histogram and the narrowest pulse,
and tries to decode the capture against every combination of {narrowest pulse, 833, 1000, 416,
208, 104, 2083 µs} × {inverted, non-inverted} × {7E1, 8N1, 7N1, 8E1}, reporting the one that
yields the most well-framed printable characters. A successful decode is published to the
sensors like any other read.

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/defl/esphome_ui1203
      ref: main
    components: [badger_meter]
    refresh: 0s
```

## Configuration

```yaml
badger_meter:
  id: badger_meter_component
  data_pin:
    number: GPIO17
    mode:
      input: true
      pullup: true
  # clock_pin: GPIO16      # ONLY if the ESP powers the meter — see Wiring
  capture_window: 1200ms   # hard stop for one capture
  idle_gap: 250ms          # end the capture this long after the last edge
  read_interval: 60s

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

| Sensor | Type | Description |
|-----------------|--------|------------------------------------------|
| `meter_reading` | sensor | Parsed numeric reading from the meter |
| `raw_value` | sensor | Full numeric value (all digits after 'R') |
| `raw_string` | text | Complete decoded ASCII string |
| `meter_id` | text | Meter serial/ID (trailing digits) |

## Tuning

- **Reading digits**: the parser defaults to 7 digits for the reading. The split is
  meter-model-specific and unverified; edit `reading_digits` in `badger_meter.cpp`.
- **Unit**: gallons, cubic feet or cubic metres depending on the meter's configuration. Apply a
  `multiply` filter in YAML once a decoded string has established which.

## License

This project uses the same dual-license as ESPHome:

- **MIT** — Python code and all other parts
- **GPL-3.0** — C++/runtime code (`.h`, `.cpp` files)

See [LICENSE](LICENSE) for full text.

## Credits

Protocol implementation based on:
- [kmeter](https://github.com/rszimm/kmeter) by rszimm — original Linux kernel module implementation
- [sensus_protocol_lib](https://github.com/michlv/sensus_protocol_lib) by michlv — Arduino/ESP8266 port
