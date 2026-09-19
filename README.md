# Badger Water Meter ESPHome Component

ESPHome external component that reads Badger water meters over the three-wire **Sensus (UI-1203)
encoder protocol** — the same interface AMR/AMI endpoints use.

**Status: working** on a Badger E-Series® Ultrasonic meter with the HR (high-resolution) encoder
output, reading into Home Assistant every 60 s. Everything specific to that meter — its cable
colour codes, electrical and timing details, and the message fields — is in
[docs/badger-e-series-ultrasonic.md](docs/badger-e-series-ultrasonic.md).

## Before you wire anything

**Badger ships its encoder cable in more than one colour code, and they are easy to confuse.** On
the tested meter, wiring to the wrong code left the clock input on ground: the meter never
answered, and the floating lines looked deceptively alive. Identify your cable's clock, data and
common conductors first — the docs describe the codes and a DMM test that tells them apart.

## Wiring

| Meter signal | ESP32 |
|---|---|
| Clock / power | a GPIO, driven directly (`clock_pin`) |
| Data — open collector | a GPIO input (`data_pin`), plus an **external 4.7–10 kΩ pull-up to 3.3 V** |
| Common | GND |

The internal pull-up alone is too weak against mains coupling on a cable run.

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
  clock_pin: GPIO16
  data_pin:
    number: GPIO17
    mode:
      input: true
      pullup: true
  mode: clocked
  read_interval: 60s

sensor:
  - platform: badger_meter
    meter_reading:
      name: "Water Meter"
      unit_of_measurement: "ft³"     # from your meter's display
      device_class: water
      state_class: total_increasing
      accuracy_decimals: 3
      filters:
        - multiply: 0.001            # from your meter's display
        # Drop reversals and implausible jumps: TOTAL_INCREASING reads a reversal as a meter
        # reset, and a mis-framed digit would put a spike in Home Assistant's statistics.
        - lambda: |-
            static float last = NAN;
            if (!std::isnan(last) && (x < last || x - last > 100.0f)) return {};
            last = x;
            return x;
    raw_value:
      name: "Water Meter Raw Value"
    flow_rate:
      name: "Water Meter Flow Rate"

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

**Take the unit and multiplier from your own meter's display.** Badger factory-programs gallons,
cubic feet or cubic metres at a size-dependent resolution; compare the reading field of the raw
string with the LCD before enabling `meter_reading`, because a wrong unit written into Home
Assistant's long-term statistics is painful to undo. [badger_meter.yaml](badger_meter.yaml) is a
complete device file.

## Sensors

| Sensor | Type | Description |
|---|---|---|
| `meter_reading` | sensor | The register reading as a number; scale it with a `multiply` filter |
| `raw_value` | sensor | The same number, unscaled |
| `flow_rate` | sensor | Instantaneous flow in gal/min, from the E-Series `GC` field — see the docs for how it was established |
| `raw_string` | text | The complete decoded message |
| `meter_id` | text | The meter's ID field |

Both numeric sensors are 32-bit floats, as all ESPHome sensors are, so a 9-digit register loses
its last digit above ~16.7 million counts.

## How a read works

1. **Reset** — the clock line is held low (`reset_hold`, default 1.2 s), restarting the register's
   message from the beginning.
2. **Power up** — held high (`power_up_time`, default 3 s). Both waits are non-blocking.
3. **Clock** — the line is toggled once per bit and the data line sampled after each rising edge,
   up to 1000 bits (~420 ms, blocking).
4. **Decode** — 7E1, 7E2, 8N1, 7N1 and 8E1 are tried in both polarities; the best result is kept
   up to the first `CR`, and it must contain at least three distinct characters, so a stuck or
   mains-coupled line cannot frame as a repeated character.
5. **Parse** — `V;RB<reading>;IB<id>;…`, or a bare `R<digits>` string.

The component still carries scaffolding from its diagnostic phase — a boot-time pin scan, a
`passive` capture mode, and options (`bit_period`, `capture_window`, `idle_gap`) that the clocked
path currently ignores; the clock timing is fixed in `badger_meter.cpp`.

## Disclaimer

This disclaimer covers the whole project, including everything under `docs/`.

**Independent project.** This project is not affiliated with, endorsed by, sponsored by or
supported by Badger Meter, Inc., Sensus / Xylem Inc., Itron, Inc., Elster / Honeywell, Neptune
Technology Group, SCADAmetrics, AWWA, or any other company or organisation named in it.

**Trademarks.** Badger Meter, E-Series, ORION, BEACON, ADE and HR-E are trademarks of Badger
Meter, Inc. Sensus and ICE are trademarks of Sensus (Xylem Inc.). Itron and ERT are trademarks of
Itron, Inc. EtherMeter, TheMeterDisplay and Signalizer are trademarks of SCADAmetrics. All other
product names, company names and trademarks are the property of their respective owners. They
appear here only to identify the equipment this component was tested with and the documents that
were consulted; no endorsement is implied.

**How this information was obtained.** Everything in this project comes from:

1. **Publicly available documents** — manufacturer manuals and datasheets, third-party product
   documentation, and expired patents — each linked where it is used. No vendor document is copied
   or redistributed here; short quotations are included only to identify the source of a fact, and
   all rights in those documents remain with their owners.
2. **Observation of a meter the author owns**, through its external output cable, used as that
   cable is intended: supplying power and a clock, and reading the data line. The meter was not
   opened. Its firmware was not read, extracted, decompiled or modified. No encryption, access
   control or other technical protection measure was circumvented.

No confidential, proprietary or non-disclosure-agreement information was used. The purpose is
interoperability: reading a meter the author owns into the author's own home-automation system.

**No warranty.** This software and documentation are provided "as is", without warranty of any
kind; see [LICENSE](LICENSE). Nothing here is suitable for billing, revenue or custody-transfer
metering. Connecting equipment to a meter can damage it or void its warranty, and the wiring
described here was verified on one meter only — check your own cable before connecting anything.

**Only connect to meters you own or are authorised to use.** Meters installed by a water utility
are usually the utility's property, and connecting to or interfering with them may be prohibited
by the utility's terms of service or by law.

## License

This project uses the same dual-license as ESPHome:

- **MIT** — Python code and all other parts
- **GPL-3.0** — C++/runtime code (`.h`, `.cpp` files)

See [LICENSE](LICENSE) for full text.

## Credits

- [kmeter](https://github.com/rszimm/kmeter) by rszimm — the power-as-clock read sequence
- [sensus_protocol_lib](https://github.com/michlv/sensus_protocol_lib) by michlv — Arduino/ESP8266 port
- [SCADAmetrics](https://scadametrics.com) — whose public datasheets and compatibility matrix
  document the Badger colour codes and E-Series behaviour
