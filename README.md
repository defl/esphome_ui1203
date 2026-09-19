# Badger Water Meter ESPHome Component

ESPHome external component for reading Badger water meters over the three-wire **Sensus
(UI-1203) encoder protocol** — the same interface AMR/AMI endpoints use.

**Status: working** on a Badger E-Series® Ultrasonic meter with the HR (high-resolution) encoder
output, reading every 60 s into Home Assistant since 2026-09-18:

```
V;RB003549269;IB0017118249;GC00;M1D0200,000000
```

That is 3,549.269 ft³, matching the meter's own LCD. Full details of the meter, the message and
the timings are in [docs/badger-e-series-ultrasonic.md](docs/badger-e-series-ultrasonic.md).

## Check your cable's colour code first

This cost a full day. Badger ships the encoder cable in **two colour codes**, and a
red/black/white cable fits both:

| Function | Standard (Sensus) code | **Itron ERT cable** |
|---|---|---|
| Clock / power | Red | **Black** |
| Data (open collector) | Green — sometimes White | **Red** |
| Common | Black | **White / shield** |

The tested meter uses the **Itron ERT** code. Wired to the standard code instead, its clock input
sat on ground: it never answered at any rate or voltage, and the floating lines picked up 60 Hz
mains that looked like a signal.

**Tell them apart with a DMM diode test**, meter disconnected (a few volts at ~1 mA, harmless).
The data output's transistor has a body diode from its common to its collector, so the only
junction you should find is **common (+) → data (−), ~0.5 V**. On the tested meter:
white (+) → red (−) = 0.527 V, every other pair open in both directions.

Sources: SCADAmetrics' [EtherMeter compatibility matrix](https://scadametrics.com/PDF/Compatibility_Matrix_209.pdf)
("On certain Badger Meters that are built to be connected to an Itron ERT… BLACK=TX, RED=RX,
DRAIN=CMN"), and the wiring tables in the
[TheMeterDisplay](https://scadametrics.com/PDF/TMD_v5.pdf) and
[Signalizer](https://scadametrics.com/PDF/EMP_vEVOQ4.pdf) datasheets.

## Wiring

Tested on an ESP32 (ESP32 rev 3.1, esp-idf):

| Meter wire (Itron code) | Function | ESP32 |
|---|---|---|
| **Black** | clock and power | **GPIO16**, driven directly |
| **Red** | data, open collector | **GPIO17** + external pull-up to 3.3 V (7.5 kΩ used; 4.7–10 kΩ is fine) |
| **White** | common | **GND** |

- **3.3 V straight from a GPIO is enough** — no level shifter, no 5 V; the pin powers the encoder
  interface directly. SCADAmetrics' TheMeterDisplay likewise reads Badger registers from a single
  ~3.2 V lithium cell.
- **Use an external pull-up.** The ESP32's internal ~45 kΩ is too weak against mains coupling on
  a cable run.
- **Keep the cable short.** SCADAmetrics reports communication problems with this meter on
  "medium to long" encoder cable runs.

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

The configuration running on the tested meter. See [badger_meter.yaml](badger_meter.yaml) for a
complete device file.

```yaml
badger_meter:
  id: badger_meter_component
  clock_pin: GPIO16        # meter BLACK (Itron code): power and clock
  data_pin:
    number: GPIO17         # meter RED (Itron code): open-collector data
    mode:
      input: true
      pullup: true
  mode: clocked
  read_interval: 60s       # 15 s or more — faster and the E-Series holds its reading

sensor:
  - platform: badger_meter
    meter_reading:
      name: "Water Meter"
      unit_of_measurement: "ft³"
      device_class: water
      state_class: total_increasing
      accuracy_decimals: 3
      filters:
        - multiply: 0.001  # this unit reports cubic feet with 3 implied decimals
        # Drop reversals and implausible jumps: TOTAL_INCREASING reads a reversal as a meter
        # reset, and a mis-framed digit would put a spike in Home Assistant's statistics.
        - lambda: |-
            static float last = NAN;
            if (!std::isnan(last) && (x < last || x - last > 100.0f)) return {};
            last = x;
            return x;
    raw_value:
      name: "Water Meter Raw Value"

text_sensor:
  - platform: badger_meter
    raw_string:
      name: "Water Meter Raw String"
    meter_id:
      name: "Water Meter ID"

button:
  - platform: template
    name: "Read Water Meter"
    on_press:
      - lambda: id(badger_meter_component).request_read();
```

**Set the unit and multiplier from your own meter's LCD.** The E-Series is factory-programmed for
gallons, cubic feet or cubic metres, and the implied decimal depends on the size — compare the
`RB` field of the raw string with the display before enabling `meter_reading`. A wrong unit
written into Home Assistant's long-term statistics is painful to undo.

## Sensors

| Sensor | Type | Description |
|---|---|---|
| `meter_reading` | sensor | The `RB` register value as a number; scale it with a `multiply` filter |
| `raw_value` | sensor | The same number, unscaled |
| `raw_string` | text | The complete decoded message |
| `meter_id` | text | The `IB` field — the meter's serial number |

Both numeric sensors are 32-bit floats, as all ESPHome sensors are. A 9-digit register loses its
last digit above ~16.7 million counts (16,777 ft³ at the tested resolution).

## How a read works

1. **Reset** — hold the clock line low for 1.2 s (`reset_hold`). This restarts the register's
   message from the beginning.
2. **Power up** — hold it high for 3 s (`power_up_time`). Both waits are non-blocking.
3. **Clock** — toggle the line: 100 µs low, then high, sampling the data line 220 µs after the
   rising edge; 417 µs per bit, up to 1000 bits (~420 ms, blocking).
4. **Decode** — try 7E1, 7E2, 8N1, 7N1 and 8E1 in both polarities, keep the best, stop at the
   first `CR`, and require at least three distinct characters so a stuck or mains-coupled line
   cannot frame as a repeated character.
5. **Parse** — `V;RB<reading>;IB<id>;…` or a bare `R<digits>` string.

The tested meter answers 7E1, non-inverted, and decodes identically at 417, 833 and 1000 µs per
bit, so the rate is not critical.

The component still carries scaffolding from the diagnostic phase — a boot-time pin scan, a
`passive` capture mode, and options (`bit_period`, `capture_window`, `idle_gap`) that the clocked
path currently ignores. The clock timing above is fixed in `badger_meter.cpp`.

## License

This project uses the same dual-license as ESPHome:

- **MIT** — Python code and all other parts
- **GPL-3.0** — C++/runtime code (`.h`, `.cpp` files)

See [LICENSE](LICENSE) for full text.

## Credits

- [kmeter](https://github.com/rszimm/kmeter) by rszimm — the power-as-clock read sequence
- [sensus_protocol_lib](https://github.com/michlv/sensus_protocol_lib) by michlv — Arduino/ESP8266 port
- [SCADAmetrics](https://scadametrics.com) — whose public datasheets and compatibility matrix
  document the Badger colour codes and E-Series quirks that no vendor would
