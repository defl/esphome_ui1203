# Badger E-Series Ultrasonic — tested meter

Everything established about the meter this component was developed against. Badger does not
publish the encoder's electrical interface, so every fact here is either measured on this unit or
taken from a named, publicly available source.

> **Independent project.** Not affiliated with or endorsed by Badger Meter or any other company
> named here; all product names are trademarks of their respective owners, used only to describe
> compatibility. See [Disclaimer](#disclaimer) at the end of this page.

## The meter

| | |
|---|---|
| Model | Badger Meter **E-Series® Ultrasonic**, 1-inch |
| Body | stainless steel |
| Display | 9-digit LCD, showing total and rate of flow |
| Power | internal sealed battery — the meter measures on its own; the encoder interface is powered by whatever reads it |
| Encoder output | 3-conductor cable, labelled only with an **`HR`** sticker (high-resolution encoder) |
| Cable colours | red, black, white — no green |
| Colour code | **Itron ERT variant**: black = clock/power, red = data, white = common |
| Register units | cubic feet, three implied decimals (factory-programmed) |

Badger's manual says the output protocol "is indicated on the AMR output wire and is determined at
the time of order", and that the meter "may be ordered with either a Badger Meter 308 in-line
connector or an Itron connector". SCADAmetrics adds that the register "requires factory
pre-programming. Specify 'High-Resolution E-Series ADE Meter Mode'" — consistent with the `HR`
label.

### Diode test

Meter disconnected, DMM in diode mode, red probe on the first wire:

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
| `GC` | `00` | not decoded; probably a status or alarm code |
| `M1D` | `0200,000000` | not decoded; probably the E-Series extended status Badger sends to ORION endpoints |

### Register resolution by meter size

From Badger's E-Series installation and operation manual. Which unit a meter uses is set at the
factory, so read your own LCD.

| Meter size | Gallons | Cubic feet | Cubic metres |
|---|---|---|---|
| 5/8" – 1" | 0.01 | 0.001 | 0.0001 |
| 1-1/2" – 2" | 0.1 | 0.01 | 0.001 |

## Traps

- **The colour code.** With the standard code the clock input sits on ground and the meter is
  silent at every rate, level and sampling point. The floating lines then look deceptively alive:
  the data wire follows the "clock" wire down through the output transistor's junction, and a
  weak pull-up lets 60 Hz mains through. See the diode test above.
- **Mains coupling frames as clean characters.** A 60 Hz square wave on an undriven line decodes at
  1200 baud 7E1 as a run of error-free `|` characters. The decoder now refuses any result with
  fewer than three distinct characters.
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
  battery-powered reader), [Application Note Badger.1](https://scadametrics.com/PDF/Badger_SCADAmetrics_01.pdf)
- Badger Meter — [E-Series Ultrasonic installation and operation manual](https://metervalveandcontrol.com/pdf/water-meters/11-Ultra/01e-BADGER%20E%20SERIES%20IOM.pdf),
  [HR-E LCD encoder manual](https://www.instrumart.com/assets/HR-E-manual.pdf)
- Patents — [US 5155481](https://patents.google.com/patent/US5155481A/en) and
  [US 5252967](https://patents.google.com/patent/US5252967A/en), the original two- and three-wire
  protocol
- [kmeter](https://github.com/rszimm/kmeter) and
  [sensus_protocol_lib](https://github.com/michlv/sensus_protocol_lib) — the power-as-clock read
  sequence, on Sensus and Neptune registers

## Disclaimer

**Independent project.** This page and the project it belongs to are not affiliated with,
endorsed by, sponsored by or supported by Badger Meter, Inc., Sensus / Xylem Inc., Itron, Inc.,
Elster / Honeywell, Neptune Technology Group, SCADAmetrics, AWWA, or any other company or
organisation named here.

**Trademarks.** Badger Meter, E-Series, ORION, BEACON, ADE and HR-E are trademarks of Badger
Meter, Inc. Sensus and ICE are trademarks of Sensus (Xylem Inc.). Itron and ERT are trademarks of
Itron, Inc. EtherMeter, TheMeterDisplay and Signalizer are trademarks of SCADAmetrics. All other
product names, company names and trademarks are the property of their respective owners, and
appear here only to identify the tested equipment and the documents consulted. No endorsement is
implied.

**How this information was obtained.** Only from the publicly available documents listed under
Sources, each linked rather than copied, with short quotations used solely to identify where a
fact comes from; from observing the external output cable of a meter the author owns, used as
intended — the meter was not opened, its firmware was not read, extracted, decompiled or
modified, and no technical protection measure was circumvented. No confidential,
proprietary or non-disclosure-agreement information was used. The purpose is interoperability with
the author's own home-automation system.

**No warranty.** Provided "as is", without warranty of any kind. Not suitable for billing, revenue
or custody-transfer metering. Wiring anything to a meter can damage it or void its warranty; the
findings here were verified on one meter only. **Only connect to meters you own or are authorised
to use** — utility-installed meters are usually the utility's property, and connecting to them may
be prohibited by the utility's terms or by law.
