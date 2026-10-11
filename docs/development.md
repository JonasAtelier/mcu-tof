# Architecture, driver development and tests

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Repository layout

```
include/                    public API, the only directory apps include
  tof.h                     struct tof_dev and tof_sample, start/stop/read, read_blocking, the data-ready hook
  tof_log.h                 tof_err/warn/info/dbg, tof_log_sample()
  vl53l1x.h                 esp-vl53l1x-tof's handle API, the HAL's, and vl53l1x_tof()
  tfluna.h                  the TF-Luna's handle API, serial and I2C, and tfluna_tof()
src/                        private to the library
  tof.c                     the core: bound checks, the stamp, the deadline, the hook counter
  tof_log.c                 tof_log_sample()
  tof_priv.h                bus ops, attach and probe, errno maps, sleep, yield and the clock
  tof_port_esp.c            all of that on ESP-IDF; empty elsewhere
  tof_port_stm.c            all of that on the STM32 HAL; empty under ESP-IDF
  vl53l1x/                  vl53l1x.c  vl53l1x_regs.h (ST's data, BSD-3, its notice kept)
  tfluna/                   tfluna.c  tfluna_proto.h
test/                       host tests, `make -C test test`
  test_tfluna_proto.c       the protocol against the manual's own examples
  core_cases.h              core cases, shared by esp/ and stm/
  vl53l1x_cases.h           a fake VL53L1X at register level and its cases, shared
  tfluna_cases.h            a fake TF-Luna, serial line and registers, and its cases, shared
  esp/                      suites on idf_stubs/, through tof_port_esp.c
  stm/                      suites on hal_stubs/, through tof_port_stm.c
  idf_stubs/  hal_stubs/    the slice of ESP-IDF and of the HAL the code calls
examples/
  esp/<chip>/               ESP-IDF projects: vl53l1x (esp-vl53l1x-tof's), tfluna_uart, tfluna_i2c
  stm/<chip>/               STM32 snippets: vl53l1x, tfluna
docs/                       the five guides; the PDFs here are gitignored
assets/                     README diagram (esp-vl53l1x-tof's)
CMakeLists.txt              ESP-IDF only; stm-forge globs src/**/*.c instead
Kconfig                     TOF and TOF_<CHIP>, for ESP-IDF and both forges' menus
idf_component.yml           ESP-IDF component manifest
LICENSE, LICENSE-ST         MIT; ST's BSD 3-clause notice for its data
```

Naming inside `src/<chip>/`, the same in every category library:

| File | Holds |
|---|---|
| `<chip>.c` | the driver: init, del, the ops, the chip's calls |
| `<chip>_regs.h` | register map: addresses, bit fields, values, each with its source citation |
| `<chip>_proto.h` | serial protocol, for parts without registers or with both |
| `<chip>_math.h` | pure conversions, compiled and asserted on the host |
| `<chip>_priv.h` | driver state, only if needed |

The VL53L1X has registers only, so `vl53l1x_regs.h`; the TF-Luna has a
serial protocol and a small register table, both pure, in `tfluna_proto.h`.
The chip drivers keep esp-vl53l1x-tof's four-space style; the library's
own files use tabs.

## Handles first

As in mcu-expander: esp-vl53l1x-tof's calls already had public names taking
a handle, and its ESP-IDF header had to stay as it was, so the handle stays
the device. `struct vl53l1x_t` and `struct tfluna_t` embed a
`struct tof_dev`; init fills it and points `priv` back at the handle, and
`<chip>_tof()` hands it out.

The category is what every ranging sensor shares: start and stop, a
non-blocking read of one sample in millimetres with a verdict in one scale,
and optionally data-ready, budget and period. `tof_read()` stamps the time
and decides `valid`; `tof_read_blocking()` loops on `-EAGAIN` against a
deadline on `tof_time_us()`. The data-ready hook is the same counter as
mcu-expander's INT hook.

## Platform ports

The drivers in `src/` are the same C on both platforms; what differs is in
`src/tof_port_esp.c` and `src/tof_port_stm.c`: the bus ops, attach,
`tof_i2c_probe()`, `tof_sleep_ms()`, `tof_yield()` and `tof_time_us()`.

What only one platform has stays in the driver behind `#ifdef
ESP_PLATFORM`: XSHUT through the gpio driver and the I2C clock on ESP-IDF;
XSHUT as an app's port and pin on the HAL, and each init built only with its
HAL module.

The handle calls return `esp_err_t` on ESP-IDF and errno on the HAL: the
drivers work in errno and `TOF_HANDLE_RET()` converts once where each public
call returns (`tof_esp_err()`).

Each chip file includes `tof_priv.h` before its `#if CONFIG_TOF_<CHIP>`: on
ESP-IDF that is what brings in `sdkconfig.h`, and a chip file that tested
the option first would compile to nothing.

**Waits.** `tof_sleep_ms()` is long, never short: on ESP-IDF it rounds up
to ticks and adds one, because `pdMS_TO_TICKS(2)` is 0 at the default
100 Hz tick -- the bug in esp-vl53l1x-tof's waits. Polls are bounded by
`tof_elapsed_ms()` on the clock, never by counting.

## Bus abstraction

The bus layer is mcu-imu's: `struct tof_bus_ops` with `transfer()`,
`receive(bus, rx, len, drop_stale)` for a part that streams, and `detach()`,
picked at run time by which attach the driver calls. Every category library
offers I2C, SPI and UART, used or not, and `test/{esp,stm}/test_port_*`
check all three. The I2C presence probe is mcu-encoder's: `-ENXIO` when
nothing ACKs, decided before anything is allocated.

**One extension, for the VL53L1X's 16-bit register index.** An I2C read
takes at most one `tx` byte before the repeated start: `cmd` is the index's
high byte, `tx[0]` its low one. ESP-IDF sends both with
`i2c_master_transmit_receive()`; the HAL passes them as one 16-bit memory
address to `HAL_I2C_Mem_Read(..., I2C_MEMADD_SIZE_16BIT, ...)`, which sends
it high byte first. More than one `tx` byte before a read is still
`-EINVAL`. A write needs nothing new: `cmd` and `tx` go out in order, the
HAL's 8-bit memory address being the index's high byte.

The TF-Luna's serial side reads its stream with `receive()`: a window over
the line keeps every byte not yet ruled out as a frame start, resyncs on
either header (0x59 0x59 for data, 0x5A for a reply), and takes a frame only
when its checksum matches -- the WT901 reader's approach in mcu-imu, with
two frame kinds.

## Adding a chip

1. `include/foo.h`: the chip's handle API, with the platform's handle types
   behind `#ifdef ESP_PLATFORM`, and `struct tof_dev *foo_tof(foo_handle_t)`.
2. `src/foo/foo.c`, in a folder of its own. Include `tof_priv.h` first, then
   wrap the driver in `#if CONFIG_TOF_FOO` (never `#ifdef`). Embed a
   `struct tof_dev` in the handle; give it `start`, `stop` and a `read` that
   returns `-EAGAIN` when nothing is waiting, and the optional ops it has.
   Map the chip's verdict onto `enum tof_status`; keep its own code in
   `raw_status`.
3. In init: refuse a bad config, check presence, allocate, attach,
   configure, and only on success set `tof.ops` and `tof.priv`. Check every
   bus transfer; wait with `tof_sleep_ms()` and deadlines on
   `tof_elapsed_ms()`. Work in errno; return `TOF_HANDLE_RET(err)`.
4. Add `CONFIG_TOF_FOO` to the no-chip checks in `CMakeLists.txt` and
   `src/tof_port_stm.c`, a HAL module `#error` to `src/tof_priv.h` if it
   needs one, its sources to `CMakeLists.txt`, a `config TOF_FOO` to
   `Kconfig`, `# TOF_FOO is not set` to both forges' defconfigs, and its
   tests to `test/Makefile`.

## Tests

```sh
make -C test test
```

Host-only; no toolchain, HAL or ESP-IDF needed. Nothing is checked against
real silicon. esp-vl53l1x-tof had no tests; all of these are new.

The fakes know nothing of the drivers. The VL53L1X fake keeps a 64 KiB
register space behind a 16-bit index with auto-increment, boots at a set
time, ranges when started, raises data-ready at the configured polarity
after a set measurement time, clears it on 0x0086, answers at a new address
from the next transfer, and goes off the bus while XSHUT is low. The TF-Luna
fake is a serial line -- bytes waiting, which a flush drops, and bytes still
to come -- that answers each command with the reply of Appendix II, and a
register table with "LUNA" at 0x3C. Expected values are written out in the
tests from the sources, not taken from the driver's headers -- except the
91-byte configuration, compared against the header's table (which a script
checked against ULD 3.4 and 3.3) with ten of its bytes also spelled out.

Each fix is checked by a test that fails without it, on both platforms:
counting polls instead of waiting on the clock (boot, data ready, and the
core's deadline), `pdMS_TO_TICKS()` for XSHUT, a bus error masked as "not
found", the model ID check, the interrupt clear, saturation in the
category, the polarity, the trigger's frame dropped as stale. So is each
datum: the configuration's start address and a byte of it, a status map
entry, a budget, a mode value, the period factor and mask, the 16-bit index
byte order on both ports, the frame checksum, the resync window, every Amp
rule and both temperature scales.

| Binary | Checks |
|---|---|
| `test_tfluna_proto` | checksum against the manual's one real example (5A 05 08 00 67); frame lengths and headers; every command the driver sends against the manual's bytes; the Amp rules at 99/100/32768/32769/65534/65535; both temperature encodings |
| `esp/test_core`, `stm/test_core` | the ops; the stamp and `valid`, set by the core whatever the op wrote; `-EAGAIN` leaves the sample alone; **`tof_read_blocking()` catches a sample 30 ms late with a 50 ms wait, misses it with 20 ms and reports the miss only after 20 ms on the clock**; any other error ends the wait; optional ops `-EOPNOTSUPP` without a call; the hook once per batch, across its wrap; unbound devices; the log line with and without temperature; on the HAL `tof_dbg()` |
| `esp/test_vl53l1x`, `stm/test_vl53l1x` | **init's whole write sequence: the 91 configuration bytes one at a time from 0x2D, long mode, 50 ms, the period 26875, then start, clear, stop, 0x09 and 0x00 for the VHV, and nothing after**; every budget in both modes and short mode's values; 15 ms in long mode, a budget not in the table, a period under the budget and a bad mode refused, nothing leaked; **a chip that boots 5 ms late is waited for; one that never boots is -ENODEV after 20 ms on the clock**; nothing at 0x29 before allocating; **a bus error in the boot wait or the configuration returned as itself**; the wrong model ID (ULD's comment's 0xEEAC) refused; out of memory; the read: data ready at the polarity, the block, ST's renumbering for all 32 raw values, x8 wrapping as ST's, the interrupt cleared; polarity active low; read_blocking's deadline; a move of address and a failed one; the category: `-EAGAIN`, samples, **saturation at 65535**, the status scale, budget and period through the ops. ESP-IDF: the device at 0x29 and its clock, removed at del, kept clock across a move; **XSHUT held low and then high at least 2 ms each at a 100 Hz tick**; the `esp_err_t` codes. HAL: reads through a 16-bit memory address, XSHUT as a port and pin, GPIO1 through an EXTI callback |
| `esp/test_tfluna`, `stm/test_tfluna` | serial: init finds the version reply among data frames; silence `-ENXIO`, no valid checksum `-EBADMSG`, frames but no answer `-ENODEV`, nothing allocated; reads drop stale data and resync past garbage, a stray header byte, a bad frame and a reply; **a stray 0x59 before a real header loses no byte**; cm and mm; rate, trigger and save byte for byte with their replies checked; **the frame a trigger asked for is kept**; the category with its Amp verdicts. I2C: init at the default and another address, ranges and clock refused, nobody there, not "LUNA"; the measurement registers; MODE, FPS, TRIG_ONE_SHOT, SAVE and ENABLE; a failed transfer. ESP-IDF: the device and its clock, `ESP_FAIL` for no checksum. HAL: pin 6 through an EXTI callback |
| `esp/test_port_esp`, `stm/test_port_stm` | the bus ops as in mcu-imu: I2C with **the 16-bit index, high byte first, or the HAL's 16-bit memory address**, SPI, UART transfer and `receive()` with and without `drop_stale`; the presence probe with its codes; `tof_sleep_ms(2)` is 20 ms at 100 Hz on ESP-IDF, and the HAL's waits and default clock |

All build under `-std=gnu11 -Wall -Wextra -Werror -pedantic` with UBSan
(`-fsanitize=undefined -fno-sanitize-recover=all`), and `-Wl,--wrap` (GNU
ld) to count allocations and capture `printf()`.
