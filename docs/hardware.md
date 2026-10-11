# Hardware

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root. "DS" is
the VL53L1X datasheet, `docs/vl53l1x.pdf` (DocID031281 Rev 3); "UM" the
TF-Luna user manual, `docs/tfluna_um_2024.pdf` (REV 01/04/2024), by printed
page. Both PDFs are gitignored.

## VL53L1X

| Pin | Goes to | Note |
|---|---|---|
| AVDD | 2.6–3.5 V | DS Table 14, p.23; 3.6 V absolute maximum (Table 13), so not 5 V tolerant; a breakout regulates |
| AVDDVCSEL | AVDD | the laser's supply; its capacitor as close to the pin as it gets (DS 1.4, p.6) |
| GND, GND2–4, AVSSVCSEL | GND | all of them (DS Table 2, p.5) |
| SDA, SCL | the I2C bus | pull-ups once per bus, near the host (DS Table 3, p.6); 400 kHz at most (DS 4, p.19) |
| XSHUT | 3.3 V, or a GPIO | active low, **always driven** -- a pull-up if the host's state is unknown (DS 1.4 note, p.6) |
| GPIO1 | an interrupt input, or nothing | open drain, so a pull-up (10 kΩ, DS 1.4 note); leave unconnected if unused |
| DNC | nothing | must float |

The address is 0x29 (0x52 as the datasheet writes it, with the R/W bit,
DS 4, p.19) at every power-up.

**Several on one bus.** Wire each XSHUT to its own GPIO (ESP-IDF:
`xshut_gpio`; HAL: `xshut_port`/`xshut_pin`, an output you set up) and bring
them up one at a time, moving each before the next:

```c
for (int i = 0; i < N; i++) {
    vl53l1x_config_t cfg = { .xshut_gpio = xshut[i] };

    ESP_ERROR_CHECK(vl53l1x_init(bus, &cfg, &sensor[i]));
    ESP_ERROR_CHECK(vl53l1x_set_address(sensor[i], 0x30 + i));
}
```

`vl53l1x_init()` drives XSHUT low then high itself, at least 2 ms each, so
each sensor boots at 0x29 (boot is 1.2 ms at most, DS 2.6.1, p.12) while the
ones already moved answer elsewhere. Hold the others in reset first: drive
every XSHUT low before the loop. The new address lives in RAM; redo it on
every boot. With a `vl53l1x_config_t` from `{ 0 }` on ESP-IDF, `xshut_gpio`
is GPIO 0 -- say `GPIO_NUM_NC` when it is not wired.

**GPIO1.** Optional: polling `GPIO_TIO_HV_STATUS` works without it. To
use it, call `tof_irq()` from its interrupt and `tof_irq_pending()` in the
loop; see [API usage](usage.md#the-data-ready-hook). ST's configuration
writes `GPIO_HV_MUX_CTRL` bit 4 clear, which its driver's notes call active
high (ULD 3.4 `VL53L1X_api.c:111`): trigger on the rising edge.

## TF-Luna

UM 6.1, Table 6 (p.8):

| Pin | Name | Goes to |
|---|---|---|
| 1 | +5V | 3.7–5.2 V, 150 mA peak (UM 5, Table 5, p.7) |
| 2 | RXD/SDA | the MCU's TX (serial) or SDA (I2C) |
| 3 | TXD/SCL | the MCU's RX (serial) or SCL (I2C) |
| 4 | GND | ground |
| 5 | Configuration | **ground: I2C mode; open or 3.3 V: serial** |
| 6 | Multiplexing output | I2C mode: data ready, high on new data, low after any register read (UM 6.3, p.10) |

Its signals are 3.3 V LVTTL (UM 5, Table 5, p.7). It has no reverse-polarity or
over-voltage protection (UM 5, p.7).

**Pin 5 decides the interface at power-up**, and no command changes it:
call `tfluna_init_uart()` for a module with pin 5 open, `tfluna_init_i2c()`
for one with it grounded. The serial link is 115200 8N1 out of the box
(UM 6.2, p.8); the I2C address 0x10, any of 0x08–0x77 once changed, at up to
400 kHz (UM 6.3, p.9).

**On I2C, read in step with pin 6.** In continuous mode a read that lands
while the module updates its registers can be wrong (UM 6.3, p.10): wire pin
6 to an interrupt and read on its edge, or use trigger mode
(`tfluna_set_rate(h, 0)` then `tfluna_trigger(h)`), which the manual
recommends when several devices share the bus.

**Settings are not saved** unless `tfluna_save()` is called (UM 6.2, p.9);
rate, trigger mode and output on/off are lost at power-off otherwise.
