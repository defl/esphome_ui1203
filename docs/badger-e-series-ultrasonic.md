# Badger E-Series Ultrasonic — tested meter

Everything established about the meter this component was developed against. Badger does not
publish the encoder's electrical interface, so every fact here is either measured on this unit or
taken from a named, publicly available source.

## The meter

| | |
|---|---|
| Model | Badger Meter **E-Series® Ultrasonic**, 1-inch |
| Body | stainless steel |
| Display | 9-digit LCD alternating two screens only: total (ft³) and rate of flow (gal/min, 0.01 resolution) |
| Generation | most likely **first-generation** E-Series: Badger's 2013 manual describes exactly these two screens, while the 2021 E-Series G2 manual adds temperature, alarm/operating-mode and firmware screens |
| Power | internal sealed battery — the meter measures on its own; the encoder interface is powered by whatever reads it |
| Encoder output | 3-conductor cable, labelled only with an **`HR`** sticker (high-resolution encoder) |
| Cable colours | red, black, white — no green |
| Colour code | **Itron ERT variant**: black = clock/power, red = data, white = common |
| Register units | cubic feet, three implied decimals (factory-programmed) |
| Acquired | bought on eBay, February 2026, $75 plus shipping |

Badger's manual says the output protocol "is indicated on the AMR output wire and is determined at
the time of order", and that the meter "may be ordered with either a Badger Meter 308 in-line
connector or an Itron connector". SCADAmetrics adds that the register "requires factory
pre-programming. Specify 'High-Resolution E-Series ADE Meter Mode'" — consistent with the `HR`
label.

## Colour codes

Badger ships the encoder cable in **two colour codes**, and a red/black/white cable fits both:

| Function | Standard (Sensus) code | **Itron ERT cable** |
|---|---|---|
| Clock / power | Red | **Black** |
| Data (open collector) | Green — sometimes White | **Red** |
| Common | Black | **White / shield** |

This meter uses the **Itron ERT** code. Wired to the standard code instead, its clock input sat on
ground: it never answered at any rate or voltage, and the floating lines picked up 60 Hz mains
that looked like a signal.

Sources: SCADAmetrics' [EtherMeter compatibility matrix](https://scadametrics.com/PDF/Compatibility_Matrix_209.pdf)
("On certain Badger Meters that are built to be connected to an Itron ERT… BLACK=TX, RED=RX,
DRAIN=CMN"), and the wiring tables in the [TheMeterDisplay](https://scadametrics.com/PDF/TMD_v5.pdf)
and [Signalizer](https://scadametrics.com/PDF/EMP_vEVOQ4.pdf) datasheets, which also note that
"manufacturers occasionally substitute a WHITE wire for a GREEN wire".

### Diode test

Tells the two codes apart without powering anything: the data output's transistor has a body diode
from its common to its collector, so the only junction to find is **common (+) → data (−),
~0.5 V**. Meter disconnected, DMM in diode mode (a few volts at ~1 mA — harmless), red probe on
the first wire:

| | reading |
|---|---|
| White → Red | **0.527 V** |
| Red → White | OL |
| Red ↔ Black, both directions | OL |
| Black ↔ White, both directions | OL |

One junction, from common to data: the body diode of the open-collector output transistor. Under
the standard colour code the same diode would have appeared as Black → White, which reads open.
Black isolated in both directions is typical of a power/clock input feeding a rectifier and
regulator.

## Wiring used

On an ESP32 (rev 3.1, esp-idf):

| Meter wire (Itron code) | Function | ESP32 |
|---|---|---|
| **Black** | clock and power | **GPIO16**, driven directly |
| **Red** | data, open collector | **GPIO17** + 7.5 kΩ pull-up to 3.3 V |
| **White** | common | **GND** |

- **3.3 V straight from a GPIO is enough** — no level shifter, no 5 V; the pin powers the encoder
  interface directly. SCADAmetrics' TheMeterDisplay likewise reads Badger registers from a single
  ~3.2 V lithium cell.
- **Keep the cable short.** SCADAmetrics reports problems communicating with this meter on "medium
  to long" encoder cable runs.

## Electrical

| | |
|---|---|
| Clock / power level | **3.3 V from an ESP32 GPIO**, driven directly |
| Data line | open collector, idles high; **7.5 kΩ pull-up to 3.3 V** |
| Polarity | **non-inverted** — high is 1 |
| Framing | **7E1**: start bit, 7 data bits LSB-first, even parity, 1 stop bit |
| Terminator | `CR` |

The data line holds each bit through the whole clock cycle, including the low phase, so the
sampling instant is not critical.

## Timing

| | |
|---|---|
| Bit period | works at **417 µs, 833 µs and 1000 µs** — identical decode at all three |
| Clock low per bit | 100–500 µs all work |
| Reset | clock held low **1.2 s** restarts the message from the beginning |
| Power-up | clock held high **3 s** before clocking |
| Message length | 47 characters incl. terminator — about 470 bits at 7E1 |
| Minimum sample period | **15 s** — per SCADAmetrics, faster polling makes "this register hold its AMI encoder reading (not advance)" |

## The message

```
V;RB003549269;IB0017118249;GC00;M1D0200,000000<CR>
```

| Field | Example | Meaning |
|---|---|---|
| `V` | | message start |
| `RB` | `003549269` | register reading, 9 digits. Here **3,549.269 ft³**, confirmed against the LCD (`003549.269 ft³`) |
| `IB` | `0017118249` | meter serial number — matches the number stamped on the meter |
| `GC` | `00` | **instantaneous flow rate** — consistent with whole gallons per minute, rounded up (see below) |
| `M1D` | `0200,000000` | not decoded; static — likely configuration or identity data rather than a measurement (see below) |

### Where `GC` and `M1D` come from

`V`, `RB` and `IB` are the Sensus protocol's base message; `GC` and `M1D` are not part of it.
Sensus's [iPERL manual](https://www.manualslib.com/manual/1415987/Sensus-Iperl.html?page=13)
describes three reading-string modes a register can be set to:

| Mode | Contents |
|---|---|
| Normal | reading and meter ID only |
| Fixed | reading and an 8-digit customer ID |
| Extended | adds "information such as the manufacturer fields and/or meter register specific data" |

This meter's message fits the extended mode, with `GC` and `M1D` as Badger's own fields. The only
extensions SCADAmetrics' [Minimum Sensus Protocol](https://www.scadametrics.com/PDF/Minimum_Sensus_Protocol_2025_01.pdf)
paper names as defined by Sensus are an `NB` field for non-billable digits and a multiplier and
units suffix on `RB` (`RB123456789,-1,04`); this meter sends neither. No public document from
Badger, Sensus, Itron, Neptune, Aclara or SCADAmetrics, and no public code, was found that defines
`GC` or `M1D` — the reading of `GC` below is measured, not documented.

### `GC` against measured flow

A tap run at two steady rates, with the true rate taken from successive `RB` readings:

| Condition | Measured rate | `GC` |
|---|---|---|
| no flow | 0 | `00` |
| tap opening | transient | `03` |
| steady, low | 0.064 ft³ per 65 s ≈ **0.44 gpm** | `01` |
| steady, higher | 0.168 ft³ per 65 s ≈ **1.16 gpm** | `02` |

Both steady points fit whole gallons per minute rounded up (0.44 → 1, 1.16 → 2); plain rounding
would give 0 and 1. Two points do not prove the scale — a third, well above 2 gpm, would. `M1D`
held `0200,000000` throughout.

The component publishes `GC` as the `flow_rate` sensor in gal/min. It has only ever been seen as
two decimal digits; if a letter appears the field is hex, so the component logs a warning and
publishes nothing rather than a wrong value.

### `M1D`

Static through every read, idle and at both flow rates. Badger's E-Series G2 manual says the
extended encoder message can carry alarms, temperature, pressure and maximum flow rate, and lists
its alarm codes as a hex bitmask ending `200` = exceeding max flow — which `0200` resembles. But
the alarm reading does not hold: E-Series meters show an alarm screen whenever an alarm is present,
and this meter shows none. And as a likely first-generation unit, the G2 code table may not apply
to it at all. With no firmware screen to compare against either, and no public definition of the
field, `M1D` stays undecoded: most plausibly a firmware, model or size code.

### Register resolution by meter size

From Badger's E-Series installation and operation manual. Which unit a meter uses is set at the
factory, so read your own LCD.

| Meter size | Gallons | Cubic feet | Cubic metres |
|---|---|---|---|
| 5/8" – 1" | 0.01 | 0.001 | 0.0001 |
| 1-1/2" – 2" | 0.1 | 0.01 | 0.001 |

## Home Assistant configuration used

This meter's `RB` is cubic feet with three implied decimals — `003549269` read `003549.269 ft³` on
the LCD — so its reading sensor is:

```yaml
    meter_reading:
      name: "Water Meter"
      unit_of_measurement: "ft³"
      device_class: water
      state_class: total_increasing
      accuracy_decimals: 3
      filters:
        - multiply: 0.001
```

plus the reversal/jump guard from the README. At 32-bit float precision the third decimal becomes
approximate above ~10,000 ft³ and is lost above ~16,384 ft³.

## Traps

- **The colour code.** With the standard code the clock input sits on ground and the meter is
  silent at every rate, level and sampling point. The floating lines then look deceptively alive:
  the data wire follows the "clock" wire down through the output transistor's junction, and a
  weak pull-up lets 60 Hz mains through. See the diode test above.
- **Mains coupling frames as clean characters.** A 60 Hz square wave on an undriven line decodes at
  1200 baud 7E1 as a run of error-free `|` characters. The decoder now discards any read with a
  single framing or parity error, or without a numeric `RB` field.
- **The patent says 7E2; the meter says 7E1.** The original Rockwell/Sensus patents specify two
  stop bits and a 1200 or 2400 Hz clock. This register answers 7E1, and is not fussy about rate.

## Standard

The governing standard is **AWWA C707**, *Encoder-Type Remote-Registration Systems for Cold-Water
Meters* (current edition C707-22). It is paywalled and was not used here.

## Sources

- SCADAmetrics — [EtherMeter compatibility matrix](https://scadametrics.com/PDF/Compatibility_Matrix_209.pdf)
  (field-tested E-Series entry, the Itron colour code, the 15 s sample period, the cable-length
  warning), [EtherMeter manual](https://scadametrics.com/PDF/EtherMeter_Manual_208x.pdf)
  (7E1 framing, `V;RB…;IB…` sample), [TheMeterDisplay](https://scadametrics.com/PDF/TMD_v5.pdf) and
  [Signalizer](https://scadametrics.com/PDF/EMP_vEVOQ4.pdf) datasheets (colour tables, ~3 V
  battery-powered reader), [Application Note Badger.1](https://scadametrics.com/PDF/Badger_SCADAmetrics_01.pdf),
  [Minimum Sensus Protocol](https://www.scadametrics.com/PDF/Minimum_Sensus_Protocol_2025_01.pdf)
  (the `NB`, multiplier and units fields)
- Sensus — [iPERL technical manual](https://www.manualslib.com/manual/1415987/Sensus-Iperl.html?page=13)
  (normal, fixed and extended reading strings)
- Badger Meter — [E-Series Ultrasonic installation and operation manual](https://metervalveandcontrol.com/pdf/water-meters/11-Ultra/01e-BADGER%20E%20SERIES%20IOM.pdf),
  [HR-E LCD encoder manual](https://www.instrumart.com/assets/HR-E-manual.pdf)
- Patents — [US 5155481](https://patents.google.com/patent/US5155481A/en) and
  [US 5252967](https://patents.google.com/patent/US5252967A/en), the original two- and three-wire
  protocol
- [kmeter](https://github.com/rszimm/kmeter) and
  [sensus_protocol_lib](https://github.com/michlv/sensus_protocol_lib) — the power-as-clock read
  sequence, on Sensus and Neptune registers

## Disclaimer

The [Disclaimer in the README](../README.md#disclaimer) applies to this page in full.
