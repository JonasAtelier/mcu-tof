# API usage

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Handles and the category view

Each chip has its own handle API, and the handle holds a `struct tof_dev`
that `<chip>_tof()` hands out:

| Chip | Init | The category view |
|---|---|---|
| VL53L1X | `vl53l1x_init(bus, &cfg, &h)` | `vl53l1x_tof(h)` |
| TF-Luna, serial | `tfluna_init_uart(port, &cfg, &h)` | `tfluna_tof(h)` |
| TF-Luna, I2C | `tfluna_init_i2c(bus, &cfg, &h)` | `tfluna_tof(h)` |

The view is valid until `<chip>_del(h)`. On ESP-IDF the chip calls return
`esp_err_t`; on the HAL, 0 or a negative errno. Every `tof_*` call returns 0
or a negative errno on both.

| Call | Does |
|---|---|
| `tof_start()`, `tof_stop()` | VL53L1X: ranging on and off. TF-Luna: its output (serial) or the LiDAR itself (I2C) on and off; it starts on |
| `tof_data_ready(t, &ready)` | VL53L1X: is a measurement waiting. `-EOPNOTSUPP` on the TF-Luna |
| `tof_read(t, &s)` | one sample; `-EAGAIN` if a VL53L1X has none yet; the TF-Luna's serial stream waits for its next frame, up to `timeout_ms` |
| `tof_read_blocking(t, &s, ms)` | `tof_read()` until a sample or `ms` on the clock; `-ETIMEDOUT` |
| `tof_set_budget_ms()`, `tof_set_period_ms()` | VL53L1X only; `-EOPNOTSUPP` on the TF-Luna, whose rate is `tfluna_set_rate()` |
| `tof_irq()`, `tof_irq_pending()` | the data-ready hook, below |
| `tof_log_sample(t, &s)` | one info line |

## Samples and validity

```c
struct tof_sample s;

if (tof_read_blocking(t, &s, 200) == 0 && s.valid)
    use(s.distance_mm);
```

Both chips give a distance whether or not they saw anything. `valid` is
`status == TOF_VALID`; `status` is the chip's verdict in one scale:

| `status` | VL53L1X (ST's code) | TF-Luna (Amp) |
|---|---|---|
| `TOF_VALID` | 0 | 100 to 32768 |
| `TOF_WEAK_SIGNAL` | 2, signal fail | under 100 |
| `TOF_OVEREXPOSED` | -- | 0xFFFF, or above 32768 (ambient light) |
| `TOF_NOISY` | 1, sigma fail | -- |
| `TOF_OUT_OF_RANGE` | 4 | -- |
| `TOF_WRAPAROUND` | 7 | -- |
| `TOF_HW_FAIL` | 5 | -- |
| `TOF_OTHER` | any other | -- |

`raw_status` keeps the VL53L1X's code (ST's renumbering, 255 for none) and
is 0 on the TF-Luna, which has none. `signal` is the VL53L1X's signal rate
in kcps or the TF-Luna's Amp; `ambient` the VL53L1X's ambient rate, 0 on the
TF-Luna; both stop at 65535. `temp_c` is the TF-Luna's, `NAN` on the
VL53L1X. `t_us` is `tof_time_us()` just before the read.

The VL53L1X's own `vl53l1x_result_t` is as esp-vl53l1x-tof had it: its
`signal_kcps` and `ambient_kcps` are the raw rate times 8 cut to 16 bits,
as ST's driver computes them, so they wrap above a raw 8191 (65528 kcps).
The category's `signal` and `ambient` stop at 65535 instead.

## Blocking reads

`tof_read_blocking()` and `vl53l1x_read_blocking()` wait on the clock: the
deadline is `timeout_ms` of `tof_time_us()`, not a number of polls, so it
holds at a 100 Hz FreeRTOS tick as at 1 kHz. Between polls they yield one
tick on ESP-IDF and wait 1 ms on the HAL. Allow at least the timing budget
plus a little.

## The data-ready hook

The VL53L1X's GPIO1 and the TF-Luna's pin 6 (I2C mode) mark a fresh
measurement. Polling works without them; with one wired:

```c
void gpio_isr(void *arg)            /* or HAL_GPIO_EXTI_Callback() */
{
    tof_irq(arg);                   /* counts; never touches the bus */
}

for (;;) {
    if (tof_irq_pending(t))         /* once per batch of edges */
        tof_read(t, &s);
}
```

On ESP-IDF `tof_irq()` is in IRAM, so it is safe in an ISR registered with
`ESP_INTR_FLAG_IRAM`. One thread per device calls `tof_irq_pending()`.

## The VL53L1X

As esp-vl53l1x-tof: `mode` (`VL53L1X_SHORT`, 1.3 m and steadier in ambient
light, or `VL53L1X_LONG`, 4 m in the dark; 0 is long), `budget_ms` (15
short only, 20, 33, 50, 100, 200 or 500; 0 is 50; anything else refused),
`period_ms` (at least the budget; 0 is the budget). `vl53l1x_read()` takes
what the chip holds without checking it is ready; `vl53l1x_data_ready()`
first, or `vl53l1x_read_blocking()`. `vl53l1x_set_address()` moves it, for
several on one bus ([hardware](hardware.md#vl53l1x)).

Through the category, `tof_set_budget_ms()` takes a budget from the mode's
table that is no longer than the period, and `tof_set_period_ms()` a period
no shorter than the budget and at most 65535 ms. Set them while stopped.

## The TF-Luna

```c
tfluna_config_t cfg = {
    .timeout_ms  = 0,       /* serial: per frame or reply; 0 is 100 ms */
    .millimetres = false,   /* serial: the module saved in 9-byte/mm */
    .addr        = 0,       /* I2C: 0 is 0x10 */
};
```

| Call | Serial | I2C |
|---|---|---|
| `tfluna_read(h, &r)` | the next frame after fresh data, checksum checked | DIST, AMP and TEMP in one read |
| `tfluna_set_rate(h, hz)` | SAMPLE_FREQ, the echo checked | MODE and FPS |
| `tfluna_set_rate(h, 0)` | trigger mode | trigger mode |
| `tfluna_trigger(h)` | SAMPLE_TRIG; the next read keeps its frame | TRIG_ONE_SHOT |
| `tfluna_save(h)` | SAVE, its status checked | SAVE |

`r.distance_mm` is the module's cm times 10 (serial in 9-byte/cm, and I2C
always), or its mm with `millimetres` set. `tfluna_valid(&r)` applies the Amp
rule above. Rates run 1 to 250 Hz and must be 500/n Hz (UM 6.5, p.11);
the driver sends what it is given and checks the echo. Nothing is written to the module's flash except by
`tfluna_save()`.

On the serial line every read drops what was waiting and takes the next
frame, so a sample is never older than one frame -- except right after
`tfluna_trigger()`, when the frame it asked for is kept.

## Errors

| errno | ESP-IDF | Means |
|---|---|---|
| `-EINVAL` | `ESP_ERR_INVALID_ARG` | NULL, a budget not in the table, a rate over 250, an address out of range |
| `-ENXIO` | `ESP_ERR_NOT_FOUND` | nothing at the address, or nothing on the serial line |
| `-ENODEV` | `ESP_ERR_NOT_FOUND` | a VL53L1X that never booted or has the wrong model ID; a TF-Luna not signed "LUNA", or one whose frames come but which never answers |
| `-EBADMSG` | `ESP_FAIL` | serial bytes, but never a valid checksum: the baud rate |
| `-ETIMEDOUT` | `ESP_ERR_TIMEOUT` | no sample in time, or the bus's own timeout |
| `-EAGAIN` | -- | `tof_read()`: nothing waiting yet |
| `-EIO` | `ESP_FAIL` | a bus fault, or a TF-Luna reply that does not match |
| `-ENOMEM` | `ESP_ERR_NO_MEM` | out of heap |

## Changes from esp-vl53l1x-tof

- `vl53l1x.h` keeps every declaration; the token stream of the old header
  under ESP-IDF is unchanged, with additions only.
- **Waits are on the clock.** The old driver waited
  `vTaskDelay(pdMS_TO_TICKS(2))` between polls and counted 2 ms per poll; at
  IDF's default 100 Hz tick that is no wait, so the boot wait gave up in a
  few transfers, XSHUT's low pulse and boot wait were nothing, and
  `read_blocking()`'s timeout counted polls. All three now wait at least
  what they say.
- **Bus errors are themselves.** A failed transfer during init comes back as
  its own code; `ESP_ERR_NOT_FOUND` now means nothing ACKed at 0x29, the
  chip never booted in 20 ms, or the model ID is wrong.
- The result is read in one 17-byte transfer from 0x0089, as ST's
  `GetResult()` does, instead of four.
- `vl53l1x_status_str()` names ST's 255 "no measurement" and every other
  unnamed code "not valid (a code ST does not name)", where the old one
  called all of them "no measurement".
- The component is `tof`, not `vl53l1x`.
