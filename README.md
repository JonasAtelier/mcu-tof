<p align="center">
  <img src="assets/timing.svg"
       alt="The timing budget sits inside the inter-measurement period; the data-ready flag rises when a measurement finishes and clears when you read it. Below, long mode reaches 4 m in the dark against short mode's 1.3 m"
       width="720">
</p>

# mcu-tof

<p align="center">
  <img alt="platform: ESP-IDF | STM32 HAL" src="https://img.shields.io/badge/platform-ESP--IDF%20%7C%20STM32%20HAL-70e1b2?style=flat-square&labelColor=0d2238">
  <img alt="language: C" src="https://img.shields.io/badge/language-C-70e1b2?style=flat-square&labelColor=0d2238">
  <img alt="license: MIT, ST tables BSD-3-Clause" src="https://img.shields.io/badge/license-MIT%20%2B%20BSD--3-f2bd68?style=flat-square&labelColor=0d2238">
</p>

A small C time-of-flight distance library for **ESP-IDF 5.2+** and **STM32
HAL**. The ST VL53L1X (I2C) and the Benewake TF-Luna (UART or I2C) keep
their own handle APIs and hand out a `struct tof_dev`: start and stop, one
sample shape in millimetres with the chip's verdict on it, a blocking read
on a clock deadline, and an ISR-safe hook for the data-ready line. This
library replaces `esp-vl53l1x-tof`; the TF-Luna and the STM32 port are new.

The repository is named `mcu-tof`; the C API, `CONFIG_TOF_*` options and
forge package key are `tof`. The clone command below names the ESP-IDF
component directory `tof` as well.

> **Hardware unverified.** Host tests use simulated chips and platform stubs.
> Nothing here has run on a physical sensor.

[Supported hardware](#supported-hardware) · [Install](#install) ·
[Quick start](#quick-start) · [Examples](#examples) ·
[Documentation](#documentation) · [Tests](#tests) · [License](#license)

## Supported hardware

| Chip | Interface | Range | Notes |
|---|---|---|---|
| **VL53L1X** | I2C, 0x29, up to 400 kHz | 1.3 m short mode, 4 m long, in the dark | budgets 15–500 ms from ST's table; XSHUT for several on one bus |
| **TF-Luna** | UART 115200 8N1, or I2C 0x10 | 0.2–8 m | **pin 5 picks the interface at power-up**: ground for I2C, open for UART |

One Kconfig option each, `TOF_VL53L1X` and `TOF_TFLUNA`. Wiring is in the
[wiring guide](docs/hardware.md).

## Install

### ESP-IDF

Clone into your project's components directory:

```sh
git clone https://github.com/JonasAtelier/mcu-tof components/tof
```

Enable the chips in `idf.py menuconfig` → **Component config →
Time-of-flight distance subsystem**, or add to `sdkconfig.defaults`:

```ini
CONFIG_TOF=y
CONFIG_TOF_VL53L1X=y
CONFIG_TOF_TFLUNA=y
```

The `main` component sees the library automatically. Other components declare
`REQUIRES tof driver` in `idf_component_register()`.

### esp-forge / stm-forge

stm-forge must be at `ee921c3` or later, which builds the chip folders in
`src/`.

Add to `profiles/pkgversion`:

```make
tof_GIT=https://github.com/JonasAtelier/mcu-tof@stable
```

Enable the package and the chips in `configs/<target>_defconfig`:

```make
PKG_tof             = y
TOF_VL53L1X         = y
TOF_TFLUNA          = y
```

### STM32 HAL without forge

Compile every `.c` under `src/`, chip folders included, add `include/` and
`src/` to the include path, and define `CMSIS_device_header` as your family
header, `USE_HAL_DRIVER`, and `CONFIG_TOF_<CHIP>=1`. The VL53L1X needs the
HAL I2C module; the TF-Luna the UART or the I2C module.

**No chip driver is enabled by default.** See the
[installation guide](docs/installation.md) for all options.

## Quick start

On ESP-IDF, with an `i2c_master` bus created, exactly as esp-vl53l1x-tof:

```c
#include "vl53l1x.h"

vl53l1x_config_t cfg = {
    .mode       = VL53L1X_LONG,
    .budget_ms  = 50,
    .xshut_gpio = GPIO_NUM_NC,
};
vl53l1x_handle_t sensor;
vl53l1x_result_t r;

ESP_ERROR_CHECK(vl53l1x_init(bus, &cfg, &sensor));
ESP_ERROR_CHECK(vl53l1x_start(sensor));

if (vl53l1x_read_blocking(sensor, &r, 200) == ESP_OK &&
    r.status == VL53L1X_VALID)
    printf("%u mm\n", r.distance_mm);
```

The same through the category, for either chip:

```c
struct tof_dev *tof = vl53l1x_tof(sensor);     /* or tfluna_tof(lidar) */
struct tof_sample s;

if (tof_read_blocking(tof, &s, 200) == 0 && s.valid)
    printf("%u mm\n", s.distance_mm);
```

- The TF-Luna starts with `tfluna_init_uart()` or `tfluna_init_i2c()`,
  matching how its pin 5 is wired.
- On ESP-IDF the chip calls return `esp_err_t`, as esp-vl53l1x-tof did; on
  the HAL, **0 or a negative errno**. `tof_*` is errno on both.
- **Check `valid`** (or `status`): both chips give a number whether or not
  they saw anything. See [API usage](docs/usage.md).

## Examples

| Chip | ESP-IDF | STM32 HAL |
|---|---|---|
| VL53L1X | [vl53l1x](examples/esp/vl53l1x/) (esp-vl53l1x-tof's) | [vl53l1x](examples/stm/vl53l1x/main.c), GPIO1 through EXTI |
| TF-Luna | [tfluna_uart](examples/esp/tfluna_uart/), [tfluna_i2c](examples/esp/tfluna_i2c/) | [tfluna](examples/stm/tfluna/main.c), UART |

ESP-IDF examples locate this library through `EXTRA_COMPONENT_DIRS`, which
assumes the repository is checked out as `tof`. With ESP-IDF activated:

```sh
cd examples/esp/vl53l1x
idf.py set-target esp32 # Replace with your board's target.
idf.py build
```

STM32 examples are snippets to integrate into your application.

## Scope and limitations

- **VL53L1X: no calibration, ROI or thresholds.** ST's offset and crosstalk
  calibration, a narrower field of view and distance-window interrupts are
  not offered, as in esp-vl53l1x-tof. Mode, budget and period are.
- **TF-Luna: the 9-byte/cm and 9-byte/mm frames only.** PIX, the timestamped
  and device-ID formats, on/off mode and the ultra-low-power mode are not
  read. Nothing is saved to the module unless `tfluna_save()` is called.
- **No locks.** One thread per device, or your own mutex.

## Documentation

| Guide | Contents |
|---|---|
| [Installation](docs/installation.md) | Forge, ESP-IDF, STM32 HAL, build options and logging setup |
| [Hardware](docs/hardware.md) | Pins and wiring for both chips, several VL53L1X on one bus, the TF-Luna's pin 5 |
| [API usage](docs/usage.md) | The handles and the category view, samples and validity, blocking reads, the data-ready hook, errors, changes from esp-vl53l1x-tof |
| [Chip reference](docs/chips.md) | Where every value comes from: ST's ultra lite driver, line by line, and the TF-Luna manual |
| [Development](docs/development.md) | Layout, platform ports, bus abstraction, adding a driver and test coverage |

## Tests

```sh
make -C test test
```

Host-only tests use stubbed HAL and ESP-IDF APIs; no embedded toolchain or
physical chip is needed. See the [test reference](docs/development.md#tests).
Error logs during the suite are expected: tests deliberately exercise
failure paths.

## License

[MIT](LICENSE), except the VL53L1X register addresses and configuration
tables in `src/vl53l1x/vl53l1x_regs.h`, which are ST's, from its ultra lite
driver (STSW-IMG009) 3.4.0, under the BSD 3-clause licence: ST's notice is
kept in that file and in [LICENSE-ST](LICENSE-ST). Firmware shipped in
binary form with the VL53L1X driver must reproduce LICENSE-ST in its
documentation or other materials (BSD-3 clause 2).
