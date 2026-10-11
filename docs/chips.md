# Chip reference

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root. Where
every value the drivers write or read comes from, and what was checked.

## VL53L1X

### Sources

| Short | Document | Used for |
|---|---|---|
| DS | ST VL53L1X datasheet, DocID031281 Rev 3, `docs/vl53l1x.pdf` | the address, the 16-bit index and byte order, the identity registers, boot time, pins |
| ULD 3.4 | ST's VL53L1X ultra lite driver, STSW-IMG009 3.4.0: `VL53L1X_api.h` and `VL53L1X_api.c`, as mirrored at github.com/linkjumper/VL53L1X_ULD_API, commit 7d71660a | every other register and value; cited `api.h:N`, `api.c:N` |
| ULD 3.3 | the same files at 3.3.0, github.com/shizacat/st-vl53l1x-uld-linux, commit e161c2c6 | cross-check only |

**UM2510** (the ULD user manual) and **UM2356** (the full API manual, which
DS 4.2 points to) were not consulted: ST's site would not serve them to the
fetch. The ULD's code is the source of truth here; where it is silent --
the names of most range status codes -- nothing was invented.

The datasheet says outright that the register map is not exposed (DS 4.2,
p.22). The register addresses, the 91-byte configuration, the distance-mode
and timing-budget tables, the VHV writes, the period formula and the status
renumbering are ST's data, copied from ULD 3.4 under its BSD 3-clause
licence with ST's notice kept (`src/vl53l1x/vl53l1x_regs.h`, `LICENSE-ST`).
No ST function is copied.

### What was checked

Every value in `vl53l1x_regs.h` carries its ULD 3.4 line. Against
esp-vl53l1x-tof (master `7e0b040`), which had the same values with no
citation, and against ULD 3.3:

| Data | ULD 3.4 | esp-vl53l1x-tof | ULD 3.3 |
|---|---|---|---|
| 23 register addresses | `api.h:81-119`; VHV start `api.c:241` | same | same |
| configuration, 0x2D-0x87, 91 bytes | `api.c:107-199` | same, byte for byte (checked by script) | same |
| short / long mode tuning | `api.c:461-475` | same | same |
| timing budgets, 7 short and 6 long | `api.c:309-409` | same | same |
| period: calibration & 0x3FF x ms x 1.075 | `api.c:505-507`, in double | same, in float | same |
| status renumbering, 24 entries | `api.c:201-204` | same, as a switch | same |
| start 0x40, stop 0x00, clear 0x01 | `api.c:279, 287, 249` | same | same |
| VHV: 0x09 to 0x0008, 0x00 to 0x000B | `api.c:240-241` | same | same |
| data ready: bit 0 of 0x0031 against !(bit 4 of 0x0030) | `api.c:264-307` | same, polarity read once | same |
| signal and ambient: raw x 8 into 16 bits | `api.c:588, 609, 637, 639` | same | same |
| result block from 0x0089, 17 bytes | `api.c:626-643` | read field by field | same |
| model ID 0xEACC at 0x010F | DS Table 11; `api.h:243` comments "0xEEAC" | 0xEACC | same comment |

**ULD 3.3 to 3.4** changes nothing this driver uses: the configuration
array's name, the per-SPAD rate factor (2000 to 200, `GetSignalPerSpad()`
and `GetAmbientPerSpad()`, not used), the crosstalk read width and
`SetDistanceThreshold()` (not used).

**`api.h:243` against the datasheet.** ULD 3.4's comment says the sensor
ID "must be 0xEEAC"; the datasheet's Table 11 gives 0xEA at 0x010F and 0xCC
at 0x0110, read together 0xEACC. ST's code never compares it. The datasheet
is followed, as esp-vl53l1x-tof did.

### Differences from ST's driver

- **Calibration order.** ST's `SensorInit()` runs the throwaway VHV
  measurement right after the configuration (`api.c:233-241`); this driver,
  like esp-vl53l1x-tof, after mode, budget and period. The chip ends in the
  same state either way.
- **Waits are bounded.** ST waits for data ready with no limit
  (`api.c:235-237`); this driver gives the calibration 1 s and the boot
  20 ms, on the clock.
- **Booted** is bit 0 of 0x00E5. ST returns the whole byte and documents it
  as 1 booted, 0 not (`api.h:237`).
- **The address** is the 7-bit one; ST's `SetI2CAddress()` takes the 8-bit
  form and shifts it (`api.c:221`).

### Range status

ST's renumbering (`api.c:201-204`) maps the chip's raw status 0-23 to its
own codes. ULD 3.4 names four of them (`api.h:278-279`): 0 no error,
1 sigma failed, 2 signal failed, 7 wrap-around. esp-vl53l1x-tof also named
4 "out of bounds" and 5 "hardware failure", with no source given; they are
kept. Codes 3, 6 and 9-13 have no name in ULD 3.4 -- UM2510 has the table,
and was not consulted -- so `vl53l1x_status_str()` calls them "not valid (a
code ST does not name)" and the category `TOF_OTHER`.

| ST code | `vl53l1x_status_t` | `tof_status` | Name from |
|---|---|---|---|
| 0 | `VL53L1X_VALID` | `TOF_VALID` | `api.h:279` |
| 1 | `VL53L1X_SIGMA_FAIL` | `TOF_NOISY` | `api.h:279` |
| 2 | `VL53L1X_SIGNAL_FAIL` | `TOF_WEAK_SIGNAL` | `api.h:279` |
| 4 | `VL53L1X_OUT_OF_BOUNDS` | `TOF_OUT_OF_RANGE` | esp-vl53l1x-tof |
| 5 | `VL53L1X_HARDWARE_FAIL` | `TOF_HW_FAIL` | esp-vl53l1x-tof |
| 7 | `VL53L1X_WRAPAROUND` | `TOF_WRAPAROUND` | `api.h:279` |
| 3, 6, 9-13 | -- | `TOF_OTHER` | none |
| 255 | -- | `TOF_OTHER` | ST's "no code" |

### Signal and ambient rates

ULD 3.4 computes both as the raw register times 8 into a `uint16_t`
(`api.c:588, 609, 637, 639`), so they wrap above a raw 8191.
`vl53l1x_result_t` keeps that, as esp-vl53l1x-tof did; `struct tof_sample`
stops them at 65535 instead.

### Start-up

XSHUT low then high, at least 2 ms each (`tof_sleep_ms()`, long, never
short); boot is 1.2 ms at most (DS 2.6.1, p.12). Then: presence at 0x29
(`-ENXIO`), the booted state within 20 ms (`-ENODEV`), the model ID
(`-ENODEV`), the configuration, the polarity, mode, budget, period, and the
VHV measurement. A bus error at any step is returned as itself.

## TF-Luna

### Sources

| Short | Document |
|---|---|
| UM | Benewake "TF-Luna User Manual", REV 01/04/2024, `docs/tfluna_um_2024.pdf`; printed page numbers (the PDF's are four more) |
| A05 | Benewake "SJ-PM-TF-Luna A05 Product Manual", `docs/tfluna_A05_pm.pdf`; cross-check |

Every value in `src/tfluna/tfluna_proto.h` cites UM. A05 agrees on each one
checked: 115200 8N1, the 0x5A command frame and its checksum, the 9-byte
frames, Temp / 8 - 256, the Amp rules, 500/n Hz, and the I2C registers
(TEMP in 0.01 degC, SAVE, MODE, TRIG_ONE_SHOT, ENABLE, FPS, SIGNATURE).

### Values

| What | Value | UM |
|---|---|---|
| interface | pin 5: ground I2C, open or 3.3 V serial | 6.1, Table 6, p.8 |
| serial | 115200 8N1 | 6.2, p.8 |
| command | 0x5A, Len (whole frame, 4-255), ID, payload, checksum | 6.2, p.8-9 |
| checksum | low 8 bits of the sum of every byte before it | 6.2, p.9 |
| upward checksum | always valid | Appendix II item 8, p.25 |
| data frame | 0x59 0x59, Dist, Amp, Temp (low byte first), checksum | Appendix I items 1 and 3, p.20 |
| Temp, serial | Temp / 8 - 256 degC | 6.4, p.10 |
| unreliable | Amp < 100, or 0xFFFF | 6.4, p.10; Appendix I, p.20 |
| ambient overexposure | Amp > 32768 | 6.4, p.10 |
| dummy distance | Amp < amp_threshold x 10 (100): Dist is 0 | 6.8, p.12 |
| rate | 1-250 Hz, 500/n; 0 is trigger mode | 6.5-6.6, p.10-11 |
| GET_VERSION 0x01 | reply: three bytes, revision, minor, major | Appendix II item 1, p.22 |
| SAMPLE_FREQ 0x03 | Freq, two bytes; the reply echoes it | item 3, p.22-23 |
| SAMPLE_TRIG 0x04 | the reply is a data frame | item 4, p.23 |
| OUTPUT_EN 0x07 | 0 off, 1 on; echoed | item 7, p.24 |
| SAVE 0x11 | reply: status, 0 success | item 11, p.26 |
| I2C address | 0x10; 0x08-0x77; up to 400 kHz | 6.3, p.9 |
| pin 6, I2C mode | high on new data, low after any register read | 6.3, p.10 |
| I2C registers | DIST 0x00 cm, AMP 0x02, TEMP 0x04 (0.01 degC), VERSION 0x0A, SAVE 0x20, MODE 0x23, TRIG_ONE_SHOT 0x24, ENABLE 0x25, FPS 0x26, SIGNATURE 0x3C "LUNA" | Appendix III, p.32-33 |

**Read here, not in the manual:**

- The I2C TEMP register is read as signed. Appendix III gives its unit, not
  its sign; unsigned would make anything below 0 degC absurd.
- A reply frame is searched for only up to 16 bytes long. The longest this
  driver asks for is GET_VERSION's 7.
- The manual's command examples mostly end in 00, a checksum the module
  ignores by default (6.2, p.9). The driver fills in the real one; the one
  example with a real checksum (item 8, p.25) matches it.
- Distances under 20 cm or over 8 m "may be unreliable" (6.9, p.12); the
  driver does not flag them, since the module's own Amp rule is the one the
  manual gives for validity.
