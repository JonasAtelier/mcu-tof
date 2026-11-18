# Installation

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

The repository is `mcu-tof`; the forge package key, C API and configuration
options are `tof`. The clone command below names the ESP-IDF component
directory `tof`. It replaces `esp-vl53l1x-tof` (forge key `vl53l1x`): a
project that `REQUIRES vl53l1x` now `REQUIRES tof`; `vl53l1x.h` and its calls
are unchanged.

Three step-by-step guides come first: [ESP-IDF](#esp-idf-step-by-step),
[STM32 with CMake](#with-cmake-a-cubemx-cmake-project) and
[STM32 without CMake](#without-cmake-stm32cubeide-a-makefile). None
needs a forge; if you build with esp-forge or stm-forge, see
[With forge](#with-forge). The steps use the VL53L1X; for the TF-Luna,
change the chip name. The reference follows the guides.

## ESP-IDF, step by step

**You need** ESP-IDF 5.2 or newer, with `idf.py` working in your terminal, and
an ESP-IDF project: a folder with a `CMakeLists.txt` and a `main/` folder.
`idf.py create-project my-project` makes one.

ESP-IDF calls a library a **component**: a folder with its own
`CMakeLists.txt` that the build finds and compiles. This library is the
component `tof`.

1. **Get the library.** Pick one way.

   - **Component manager**, the simplest: the first build downloads the
     library into `managed_components/tof`. Create `main/idf_component.yml`:

     ```yaml
     dependencies:
       tof:
         git: https://github.com/JonasAtelier/mcu-tof.git
         version: stable
     ```

   - **Clone**, if you want to edit the library or build offline. In the
     project folder:

     ```sh
     git clone https://github.com/JonasAtelier/mcu-tof components/tof
     ```

     Keep the folder name `tof`: ESP-IDF names a component after its folder.

   ```
   my-project/
   ├── CMakeLists.txt
   ├── sdkconfig.defaults       step 2
   ├── main/
   │   ├── CMakeLists.txt       step 3
   │   ├── idf_component.yml    step 1, component manager
   │   └── my-project.c         step 4
   └── components/
       └── tof/                 step 1, clone
   ```

2. **Turn your chip on.** No chip driver is built until you do. Create
   `sdkconfig.defaults` in the project folder, a text file of settings you
   choose, with one line per chip:

   ```ini
   CONFIG_TOF_VL53L1X=y
   ```

   The other chip is `CONFIG_TOF_TFLUNA`. ESP-IDF copies these lines into
   `sdkconfig`, the project's full settings file, only when it creates that
   file: **if `sdkconfig` already exists, delete it** so your line is read.
   `idf.py menuconfig` → **Component config → Time-of-flight distance
   subsystem** does the same from a menu.

3. **Let your code see the library.** A new project's `main/CMakeLists.txt`
   has no `REQUIRES` and already sees every component. If yours has a
   `REQUIRES` list, add `tof` and `driver` to it:

   ```cmake
   idf_component_register(SRCS "main.c" REQUIRES tof driver)
   ```

4. **Write the smallest test.** Copy
   [examples/esp/vl53l1x/main/main.c](../examples/esp/vl53l1x/main/main.c)
   over your main source file and set `SDA_GPIO` and `SCL_GPIO` to your
   wiring. [API usage](usage.md) explains the calls.

5. **Build.**

   ```sh
   idf.py set-target esp32    # your chip: esp32s3, esp32c3, ...
   idf.py build
   ```

   A good build ends with `Project build complete.` Flash it as any ESP-IDF
   app and open the serial monitor: the example prints one line per
   reading, such as `vl53l1x-example:  842 mm   signal  2210 kcps   ambient    35 kcps`,
   with your numbers.

**If it fails**

| The build prints | Why | Fix |
|---|---|---|
| `Failed to resolve component 'tof' required by component 'main': unknown name.` | The clone is not in a folder named `tof` | Rename it to `components/tof` |
| `fatal error: vl53l1x.h: No such file or directory` | `main/CMakeLists.txt` has a `REQUIRES` list without `tof` | Step 3 |
| `tof: CONFIG_TOF is on but no chip driver is selected` (a CMake warning) | No chip turned on | Step 2 |
| ``undefined reference to `vl53l1x_init'`` | That chip is not turned on, often because `sdkconfig` existed before you edited `sdkconfig.defaults` | Delete `sdkconfig`, build again |

## STM32, step by step

**You need** a project for an STM32F4, F7 or G0 that uses ST's **HAL**, the
vendor library behind `HAL_Init()`, `HAL_I2C_Init()` and friends, with the
I2C (or UART) your chip is wired to already set up. STM32CubeMX makes such a
project; with **Toolchain / IDE: CMake** it makes a CMake one, the main case
below.

Two terms. A **HAL module** is the part of the HAL for one peripheral: I2C,
SPI, UART. It is compiled only when it is switched on in **hal_conf**, the
file `Core/Inc/stm32f4xx_hal_conf.h` (`stm32f7xx_hal_conf.h` on an F7,
`stm32g0xx_hal_conf.h` on a G0). CubeMX switches a module on when you enable
that peripheral.

### With CMake (a CubeMX CMake project)

1. **Get the library.** In the project folder, next to the top-level
   `CMakeLists.txt`:

   ```sh
   git clone https://github.com/JonasAtelier/mcu-tof
   ```

   ```
   my-board/
   ├── CMakeLists.txt                  step 2
   ├── cmake/stm32cubemx/              made by CubeMX
   ├── Core/
   │   ├── Inc/stm32f4xx_hal_conf.h    hal_conf, step 3
   │   └── Src/main.c                  step 4
   ├── Drivers/                        the HAL, copied in by CubeMX
   └── mcu-tof/                        step 1
   ```

2. **Add it to the build.** In the top `CMakeLists.txt`, after the line
   `add_subdirectory(cmake/stm32cubemx)`, add:

   ```cmake
   set(CONFIG_TOF_VL53L1X ON)          # your chip; one line per chip
   set(MCU_HAL_TARGET stm32cubemx)
   set(MCU_DEVICE_HEADER stm32f4xx.h)  # stm32f7xx.h on an F7, stm32g0xx.h on a G0
   add_subdirectory(mcu-tof)
   ```

   and add `tof` to the link line CubeMX already wrote, so it reads:

   ```cmake
   target_link_libraries(${CMAKE_PROJECT_NAME} stm32cubemx tof)
   ```

   Do not add `PRIVATE`: CubeMX's line has no such keyword, and CMake refuses
   to mix the two forms on one target.

   `stm32cubemx` is a CMake *target*, a named bundle of settings, that CubeMX
   makes to carry the HAL's include folders and defines; `MCU_HAL_TARGET`
   lends those to the library, which works with any CubeMX version.
   `MCU_DEVICE_HEADER` is your chip family's main header.

3. **Check the HAL module.** The VL53L1X talks I2C, so hal_conf must have
   `#define HAL_I2C_MODULE_ENABLED`; enabling I2C1 (or any I2C) in CubeMX
   does that. The [HAL module table](#stm32-hal) lists what every chip
   needs.

4. **Write the smallest test.** It is
   [examples/stm/vl53l1x/main.c](../examples/stm/vl53l1x/main.c) fitted into
   CubeMX's `Core/Src/main.c`, with XSHUT and the GPIO1 interrupt left out:
   `tof_read_blocking()` polls the chip instead, so only SDA and SCL need
   wiring. Paste each part between the `USER CODE BEGIN` and `END` comments
   named, so a regenerate keeps it.

   After `/* USER CODE BEGIN Includes */`:

   ```c
   #include "tof.h"
   #include "tof_log.h"
   #include "vl53l1x.h"
   ```

   After `/* USER CODE BEGIN 2 */`, which comes after `MX_I2C1_Init()`:

   ```c
     /* As in examples/stm/vl53l1x/main.c: 20 Hz in short mode. */
     vl53l1x_config_t cfg = {
       .mode = VL53L1X_SHORT,  /* 1.3 m, steadier in daylight */
       .budget_ms = 33,
       .period_ms = 50,
       .xshut_port = NULL,     /* not wired: breakouts pull it high */
     };
     vl53l1x_handle_t h;
     struct tof_dev *tof;

     /* -ENXIO: nothing at 0x29. -ENODEV: it never booted, or is not a
      * VL53L1X. */
     if (vl53l1x_init(&hi2c1, &cfg, &h))
       Error_Handler();
     tof = vl53l1x_tof(h);
     tof_start(tof);
   ```

   After `/* USER CODE BEGIN 3 */`, inside the `while (1)` loop:

   ```c
       struct tof_sample s;

       if (tof_read_blocking(tof, &s, 200) == 0)
         tof_log_sample(tof, &s);  /* s.valid before trusting it */
   ```

   The lines go out through `printf()`: send that to a UART by writing
   newlib's `_write()`. [API usage](usage.md) explains the calls.

5. **Build** as before, for example `cmake --build` with your build folder.
   A good build shows a `Linking C executable` line and no error. On the
   UART, one line per reading, such as
   `tof: vl53l1x: t 1500000 us    842 mm  valid (0)  signal 50  ambient 7`.

**If it fails**

| You see | Why | Fix |
|---|---|---|
| `tof: no chip driver is selected; set(CONFIG_TOF_<CHIP> ON) before add_subdirectory()` (a CMake warning), then ``undefined reference to `vl53l1x_init'`` | No chip turned on, or turned on after `add_subdirectory(mcu-tof)` | Step 2 |
| `#error "TOF_VL53L1X needs the HAL I2C module: ...` | The HAL module is off | Enable the peripheral in CubeMX and regenerate |
| `#error "tof: CMSIS_device_header is not defined; ...` | `MCU_DEVICE_HEADER` is missing | Step 2 |
| `fatal error: stm32f4xx.h: No such file or directory` | `MCU_HAL_TARGET` is missing | Step 2 |
| `Target "stm32cubmx" not found.` (a CMake error) | `MCU_HAL_TARGET` is misspelt | Step 2 |
| `The plain signature for target_link_libraries has already been used with the target` | `PRIVATE` added to the link line | Step 2 |

### Without CMake (STM32CubeIDE, a Makefile)

1. Copy `include/` and `src/` of this repository into your project, for
   example to `Middlewares/mcu-tof/`, keeping the chip folders inside
   `src/`.
2. Add every `.c` file under `src/` to the build, chip folders included.
3. Add two include paths, `Middlewares/mcu-tof/include` and
   `Middlewares/mcu-tof/src`, and two defines:
   `CMSIS_device_header="stm32f4xx.h"`, quotes included, and
   `CONFIG_TOF_VL53L1X=1`, one per chip. CubeMX already defines
   `USE_HAL_DRIVER` and your device, such as `STM32F405xx`.
4. Steps 3 to 5 of the CMake guide apply as they are.

Where these settings usually live: in STM32CubeIDE, **Project → Properties →
C/C++ Build → Settings → Tool Settings → MCU GCC Compiler** (Include paths,
Preprocessor); in a CubeMX Makefile, `C_SOURCES`, `C_INCLUDES` and `C_DEFS`.
The build errors are the ones in the table above; the fix is the matching
define or include path instead of a CMake line. One more:
`#include expects "FILENAME" or <FILENAME>` means `CMSIS_device_header`
lost its quotes.

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

Setup: [ESP-IDF, step by step](#esp-idf-step-by-step). For reference:

- In `main/idf_component.yml`, `version` is a git ref; `stable` is the
  released one.
- A clone in `components/` is a local component, named after its folder;
  that name is what other components `REQUIRES`. The examples in
  `examples/esp/` need no name: their `main` lists no `REQUIRES`, and ESP-IDF
  gives `main` every component, so they build from any checkout name.
- With the subsystem on and no chip picked, the build warns.
- ESP-IDF 5.2 or newer is required for the `i2c_master` API.

## STM32 HAL

Setup: [With CMake](#with-cmake-a-cubemx-cmake-project) or
[Without CMake](#without-cmake-stm32cubeide-a-makefile). The
`add_subdirectory()` route builds a static library `tof`. For reference:

- Each `CONFIG_TOF_<CHIP>` (and `CONFIG_TOF_DEBUG`) that is true reaches the
  compiler as `CONFIG_TOF_<CHIP>=1`, for the library and your app.
- `MCU_HAL_TARGET` is a target that carries your `USE_HAL_DRIVER`, device
  define (`STM32F405xx`, ...) and the include dirs of the HAL, CMSIS and your
  `stm32<family>xx_hal_conf.h`. The library copies only those include dirs
  and defines, never the target's sources, so this works with any CubeMX
  version. `MCU_DEVICE_HEADER` becomes `CMSIS_device_header`, which CubeMX
  does not define. Every `mcu-*` library reads these two, so set them once.
  Leave them unset if your project already provides both.
- The CPU flags (`-mcpu`, `-mfloat-abi`, ...) must reach the library too;
  CubeMX's toolchain file sets them for every target.
- Without CMake, `src/` only needs to be on the include path of this
  library's own sources; the defines are what stm-forge passes.

**No chip driver is built by default.** The core is always built and each
driver is opt-in: a chip whose option is absent or 0 compiles to nothing.
With the library on and no chip picked, the CMakeLists warns (ESP-IDF and
`add_subdirectory()`) and `src/tof_port_stm.c` emits a `#warning`.

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
newlib's `_write()` to a UART. stm-forge links newlib-nano
(`--specs=nano.specs`), whose `printf()` drops `%f` unless the app links with
`-u _printf_float`: add `target_link_options(firmware PRIVATE -u _printf_float)`
to your app's `CMakeLists.txt` (in a CubeMX project the target is
`${CMAKE_PROJECT_NAME}`), or `tof_log_sample()` prints a blank where the
TF-Luna's temperature goes.
