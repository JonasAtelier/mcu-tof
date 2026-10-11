# Installation

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

The repository is `mcu-tof`; the forge package key, C API and configuration
options are `tof`. The clone command below names the ESP-IDF component
directory `tof`. It replaces `esp-vl53l1x-tof` (forge key `vl53l1x`): a
project that `REQUIRES vl53l1x` now `REQUIRES tof`; `vl53l1x.h` and its calls
are unchanged.

## With forge

stm-forge must be at `ee921c3` or later ("build: libraries may keep sources
in src/ subfolders"); older ones only compile `src/*.c`, so the chip drivers
in `src/vl53l1x/` and `src/tfluna/` are never built and the link fails with
undefined references to the chip functions. esp-forge is unaffected: the
component's CMakeLists lists its sources.

With [esp-forge](https://github.com/JonasAtelier/esp-forge) or
[stm-forge](https://github.com/JonasAtelier/stm-forge). The catalog line in
`profiles/pkgversion` is the same for both:

```make
tof_GIT=https://github.com/JonasAtelier/mcu-tof@stable
```

and a target turns the library and its chips on in
`configs/<target>_defconfig`:

```make
PKG_tof             = y
TOF_VL53L1X         = y
TOF_TFLUNA          = y
# TOF_DEBUG is not set
```

or with `make menu`. esp-forge turns each line into `CONFIG_TOF_<CHIP>=y` in
`sdkconfig.defaults`; stm-forge passes it as `-DCONFIG_TOF_<CHIP>=1`.
`TOF_DEBUG = y` turns on `tof_dbg()` lines on the HAL; on ESP-IDF the log
level decides.

## ESP-IDF

Clone into your project's `components/` directory. A local component is
named after its directory, and that name is what you `REQUIRES`:

```sh
git clone https://github.com/JonasAtelier/mcu-tof components/tof
```

```cmake
idf_component_register(SRCS "main.c" REQUIRES tof driver)
```

Enable the chips under `idf.py menuconfig` → **Component config →
Time-of-flight distance subsystem**, or put `CONFIG_TOF_<CHIP>=y` in your
`sdkconfig.defaults` as each example in `examples/esp/` does. With the
subsystem on and no chip picked, the build warns. ESP-IDF 5.2 or newer is
required for the `i2c_master` API.

## STM32 HAL

Copy `include/` and `src/` into an STM32 project, compile every `.c` under
`src/`, chip folders included, add `include/` and `src/` to the include path
(`src/` only needs to reach this library's own sources), and define what the
forge would: `CMSIS_device_header` as your family header (`"stm32f4xx.h"`,
`"stm32f7xx.h"`, `"stm32g0xx.h"`), `USE_HAL_DRIVER`, and
`CONFIG_TOF_<CHIP>=1`.

**No chip driver is built by default.** The core is always built and each
driver is opt-in: a chip whose option is absent or 0 compiles to nothing.
With the library on and no chip picked, the ESP-IDF CMakeLists warns and
`src/tof_port_stm.c` emits a `#warning`.

**The HAL modules are yours to enable:**

| Part | Needs in hal_conf | HAL source | Gives |
|---|---|---|---|
| VL53L1X | `HAL_I2C_MODULE_ENABLED` | `stm32<family>xx_hal_i2c.c` | `vl53l1x_init()` |
| TF-Luna, serial | `HAL_UART_MODULE_ENABLED` | `stm32<family>xx_hal_uart.c` | `tfluna_init_uart()` |
| TF-Luna, I2C | `HAL_I2C_MODULE_ENABLED` | `stm32<family>xx_hal_i2c.c` | `tfluna_init_i2c()` |

The TF-Luna takes either; each init is built only with its own module.
Without the modules a chip needs, the build stops with an `#error` in
`src/tof_priv.h` that says which defines are missing. The bus layer's SPI
code sits under its own module switch and builds to nothing without it.

**The clock.** Samples are stamped with `tof_time_us()`, by default
`HAL_GetTick() * 1000`: millisecond steps. It is weak; define your own for
real microseconds (the DWT cycle counter, or a free-running timer).

**printf() is the log.** Lines go out through `printf()`, so retarget
newlib's `_write()` to a UART.
