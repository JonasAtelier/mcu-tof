/* First: on ESP-IDF it brings in sdkconfig.h, which the #if below reads. */
#include "tof_priv.h"

#if CONFIG_TOF_VL53L1X

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tof_log.h"
#include "vl53l1x.h"
#include "vl53l1x_regs.h"

#define TIMEOUT_MS          100     /* one bus transfer */
#define BOOT_TIMEOUT_MS     20      /* the datasheet's boot is 1.2 ms max
                                     * (DS 2.6.1); esp-vl53l1x-tof's wait */
#define XSHUT_MS            2       /* low, then high again before booting */
#define CALIBRATE_TIMEOUT_MS 1000   /* the throwaway measurement at init */
#define DEFAULT_BUDGET_MS   50
#define MAX_PERIOD_MS       UINT16_MAX  /* as vl53l1x_config_t holds it */

#define TRY(expr) do { int _e = (expr); if (_e) return _e; } while (0)

struct vl53l1x_t {
    struct tof_dev   tof;           /* the category view; priv points back */
    struct tof_bus   bus;
    tof_i2c_bus      i2c;           /* the app's, for a move of address */
    uint32_t         scl_speed_hz;
    uint8_t          addr;
    uint8_t          int_polarity;  /* what GPIO_TIO_HV_STATUS bit 0 reads
                                     * when a measurement is waiting */
    vl53l1x_mode_t   mode;
    uint16_t         budget_ms;
    uint16_t         period_ms;
};

/* A measurement as the chip gives it: rates in its own unit, x8 for kcps. */
struct raw_result {
    uint8_t  status;                /* ST's code, 0-13 or 255 */
    uint16_t distance_mm;
    uint16_t signal;
    uint16_t ambient;
};

/* ------------------------------------------------------------- bus ---- */

/*
 * A 16-bit index, high byte first, then the data, also high byte first
 * (DS 4, Figures 13-16; 4.2, Table 12). A write is the index and the data
 * in one transfer; a read sends the index and reads after a repeated start.
 */
static int reg_write(vl53l1x_handle_t d, uint16_t reg, const uint8_t *vals,
                     size_t n)
{
    uint8_t tx[1 + 4];

    tx[0] = (uint8_t)reg;
    memcpy(tx + 1, vals, n);
    return d->bus.ops->transfer(&d->bus, (uint8_t)(reg >> 8), tx, 1 + n,
                                NULL, 0);
}

static int reg_read(vl53l1x_handle_t d, uint16_t reg, uint8_t *buf, size_t n)
{
    uint8_t lo = (uint8_t)reg;

    return d->bus.ops->transfer(&d->bus, (uint8_t)(reg >> 8), &lo, 1, buf,
                                n);
}

static int write8(vl53l1x_handle_t d, uint16_t reg, uint8_t v)
{
    return reg_write(d, reg, &v, 1);
}

static int write16(vl53l1x_handle_t d, uint16_t reg, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };

    return reg_write(d, reg, b, sizeof b);
}

static int write32(vl53l1x_handle_t d, uint16_t reg, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8), (uint8_t)v };

    return reg_write(d, reg, b, sizeof b);
}

static int read8(vl53l1x_handle_t d, uint16_t reg, uint8_t *v)
{
    return reg_read(d, reg, v, 1);
}

static uint16_t be16(const uint8_t *b)
{
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static int read16(vl53l1x_handle_t d, uint16_t reg, uint16_t *v)
{
    uint8_t b[2];

    TRY(reg_read(d, reg, b, sizeof b));
    *v = be16(b);
    return 0;
}

/* ------------------------------------------------------ the settings -- */

static int set_mode(vl53l1x_handle_t d, vl53l1x_mode_t mode)
{
    const struct vl53l1x_mode_tune *t;

    if (mode != VL53L1X_SHORT && mode != VL53L1X_LONG)
        return -EINVAL;
    t = &vl53l1x_mode_tune[mode];

    TRY(write8(d, VL53L1X_PHASECAL_TIMEOUT, t->phasecal));
    TRY(write8(d, VL53L1X_VCSEL_PERIOD_A, t->vcsel_a));
    TRY(write8(d, VL53L1X_VCSEL_PERIOD_B, t->vcsel_b));
    TRY(write8(d, VL53L1X_VALID_PHASE_HIGH, t->valid_phase_high));
    TRY(write16(d, VL53L1X_SD_CONFIG_WOI_SD0, t->woi));
    TRY(write16(d, VL53L1X_SD_CONFIG_PHASE_SD0, t->phase));
    d->mode = mode;
    return 0;
}

static const struct vl53l1x_budget *find_budget(vl53l1x_mode_t mode,
                                                uint32_t ms)
{
    const struct vl53l1x_budget *tab = vl53l1x_budget_long;
    size_t n = sizeof vl53l1x_budget_long / sizeof *vl53l1x_budget_long;

    if (mode == VL53L1X_SHORT) {
        tab = vl53l1x_budget_short;
        n = sizeof vl53l1x_budget_short / sizeof *vl53l1x_budget_short;
    }
    for (size_t i = 0; i < n; i++)
        if (tab[i].ms == ms)
            return &tab[i];
    return NULL;
}

static int set_budget(vl53l1x_handle_t d, uint32_t ms)
{
    const struct vl53l1x_budget *b = find_budget(d->mode, ms);

    if (b == NULL)
        return -EINVAL;
    TRY(write16(d, VL53L1X_TIMEOUT_MACROP_A_HI, b->macrop_a));
    TRY(write16(d, VL53L1X_TIMEOUT_MACROP_B_HI, b->macrop_b));
    d->budget_ms = (uint16_t)ms;
    return 0;
}

/*
 * The period is counted in the chip's own oscillator ticks, and that
 * oscillator is trimmed per part -- so read its calibration and scale, in
 * double and cut to 32 bits, as ST's driver does (api.c:505-507).
 */
static int set_period(vl53l1x_handle_t d, uint32_t ms)
{
    uint16_t pll;

    TRY(read16(d, VL53L1X_OSC_CALIBRATE_VAL, &pll));
    pll &= VL53L1X_OSC_CALIBRATE_MASK;
    TRY(write32(d, VL53L1X_INTERMEASUREMENT,
                (uint32_t)((uint32_t)pll * ms * VL53L1X_PERIOD_FACTOR)));
    d->period_ms = (uint16_t)ms;
    return 0;
}

/* --------------------------------------------------------- measuring -- */

static int data_ready(vl53l1x_handle_t d, bool *ready)
{
    uint8_t status;

    TRY(read8(d, VL53L1X_GPIO_TIO_HV_STATUS, &status));
    *ready = (status & VL53L1X_TIO_READY) == d->int_polarity;
    return 0;
}

/* ST's renumbering of the raw status (api.c:620-622). */
static uint8_t map_status(uint8_t raw)
{
    raw &= VL53L1X_RESULT_STATUS_MASK;
    return raw < sizeof vl53l1x_status_map ? vl53l1x_status_map[raw]
                                           : VL53L1X_STATUS_NONE;
}

/*
 * The whole result block in one read, as ST's GetResult() takes it
 * (api.c:632), then the interrupt cleared: until it is, the sensor will not
 * offer the next one. esp-vl53l1x-tof read the four fields one by one.
 */
static int read_result(vl53l1x_handle_t d, struct raw_result *r)
{
    uint8_t b[VL53L1X_RESULT_LEN];

    TRY(reg_read(d, VL53L1X_RANGE_STATUS, b, sizeof b));
    r->status = map_status(b[0]);
    r->distance_mm = be16(b + VL53L1X_RESULT_DISTANCE);
    r->signal = be16(b + VL53L1X_RESULT_SIGNAL);
    r->ambient = be16(b + VL53L1X_RESULT_AMBIENT);
    return write8(d, VL53L1X_INTERRUPT_CLEAR, VL53L1X_CLEAR_INTERRUPT);
}

/*
 * Wait on the clock, not on a count of polls. esp-vl53l1x-tof counted 2 ms
 * per poll and waited vTaskDelay(pdMS_TO_TICKS(2)) between them, which at
 * IDF's default 100 Hz tick is no wait at all: its timeouts ran out in a
 * few bus transfers.
 */
static int wait_ready(vl53l1x_handle_t d, uint32_t timeout_ms)
{
    int64_t start = tof_time_us();
    bool ready;

    for (;;) {
        TRY(data_ready(d, &ready));
        if (ready)
            return 0;
        if (tof_elapsed_ms(start) >= timeout_ms)
            return -ETIMEDOUT;
        tof_yield();
    }
}

/* -------------------------------------------------- the tof_dev ops -- */

static vl53l1x_handle_t of(struct tof_dev *tof)
{
    return tof->priv;
}

static int op_start(struct tof_dev *tof)
{
    return write8(of(tof), VL53L1X_MODE_START, VL53L1X_START_RANGING);
}

static int op_stop(struct tof_dev *tof)
{
    return write8(of(tof), VL53L1X_MODE_START, VL53L1X_STOP_RANGING);
}

static int op_data_ready(struct tof_dev *tof, bool *ready)
{
    return data_ready(of(tof), ready);
}

/* x8 for kcps, stopped at 65535 instead of wrapping as ST's does. */
static uint16_t kcps(uint16_t raw)
{
    uint32_t v = (uint32_t)raw * VL53L1X_RATE_TO_KCPS;

    return v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
}

static enum tof_status tof_status_of(uint8_t status)
{
    switch (status) {
    case VL53L1X_VALID:         return TOF_VALID;
    case VL53L1X_SIGMA_FAIL:    return TOF_NOISY;
    case VL53L1X_SIGNAL_FAIL:   return TOF_WEAK_SIGNAL;
    case VL53L1X_OUT_OF_BOUNDS: return TOF_OUT_OF_RANGE;
    case VL53L1X_HARDWARE_FAIL: return TOF_HW_FAIL;
    case VL53L1X_WRAPAROUND:    return TOF_WRAPAROUND;
    default:                    return TOF_OTHER;
    }
}

static int op_read(struct tof_dev *tof, struct tof_sample *s)
{
    vl53l1x_handle_t d = of(tof);
    struct raw_result r;
    bool ready;

    TRY(data_ready(d, &ready));
    if (!ready)
        return -EAGAIN;
    TRY(read_result(d, &r));
    s->distance_mm = r.distance_mm;
    s->signal = kcps(r.signal);
    s->ambient = kcps(r.ambient);
    s->temp_c = NAN;
    s->raw_status = r.status;
    s->status = tof_status_of(r.status);
    return 0;
}

/* The period stays at least the budget, as init has it. */
static int op_set_period_ms(struct tof_dev *tof, uint32_t ms)
{
    vl53l1x_handle_t d = of(tof);

    if (ms < d->budget_ms || ms > MAX_PERIOD_MS)
        return -EINVAL;
    return set_period(d, ms);
}

static int op_set_budget_ms(struct tof_dev *tof, uint32_t ms)
{
    vl53l1x_handle_t d = of(tof);

    if (ms > d->period_ms)
        return -EINVAL;
    return set_budget(d, ms);
}

static const struct tof_ops vl53l1x_ops = {
    .start = op_start,
    .stop = op_stop,
    .data_ready = op_data_ready,
    .read = op_read,
    .set_period_ms = op_set_period_ms,
    .set_budget_ms = op_set_budget_ms,
};

/* ------------------------------------------------------------- init ---- */

static void release(vl53l1x_handle_t d)
{
    if (d->bus.ops)
        tof_bus_detach(&d->bus);
    free(d);
}

/*
 * The firmware sets its booted state once it has booted; nothing written
 * before that sticks. A bus error is returned as itself; only a chip that
 * answers but never boots is -ENODEV.
 */
static int wait_boot(vl53l1x_handle_t d)
{
    int64_t start = tof_time_us();
    uint8_t state;

    for (;;) {
        TRY(read8(d, VL53L1X_SYSTEM_STATUS, &state));
        if (state & VL53L1X_BOOTED)
            return 0;
        if (tof_elapsed_ms(start) >= BOOT_TIMEOUT_MS)
            return -ENODEV;
        tof_yield();
    }
}

/* One byte at a time from 0x2D, as ST's driver writes it (api.c:230-232). */
static int load_config(vl53l1x_handle_t d)
{
    for (size_t i = 0; i < sizeof vl53l1x_default_config; i++)
        TRY(write8(d, (uint16_t)(VL53L1X_CONFIG_FIRST + i),
                   vl53l1x_default_config[i]));
    return 0;
}

/*
 * ST's driver runs one throwaway measurement so the chip's VHV (the SPAD
 * bias calibration) settles, then tells it to reuse that result instead of
 * recalibrating from cold on every start (api.c:233-241). It does this
 * right after the configuration; esp-vl53l1x-tof, and so this driver, after
 * mode, budget and period, which leaves the chip in the same state.
 */
static int calibrate(vl53l1x_handle_t d)
{
    TRY(write8(d, VL53L1X_MODE_START, VL53L1X_START_RANGING));
    TRY(wait_ready(d, CALIBRATE_TIMEOUT_MS));
    TRY(write8(d, VL53L1X_INTERRUPT_CLEAR, VL53L1X_CLEAR_INTERRUPT));
    TRY(write8(d, VL53L1X_MODE_START, VL53L1X_STOP_RANGING));
    TRY(write8(d, VL53L1X_VHV_LOOP_BOUND, VL53L1X_VHV_TWO_BOUNDS));
    return write8(d, VL53L1X_VHV_START, VL53L1X_VHV_FROM_LAST);
}

/* Everything after the bus is attached: the chip, booted and set up. */
static int setup(vl53l1x_handle_t d, vl53l1x_mode_t mode, uint16_t budget_ms,
                 uint16_t period_ms)
{
    uint16_t model;
    uint8_t mux;
    uint16_t budget = budget_ms ? budget_ms : DEFAULT_BUDGET_MS;
    uint16_t period = period_ms ? period_ms : budget;

    if (period < budget)
        return -EINVAL;

    TRY(wait_boot(d));

    /* The one identity check the datasheet gives (Table 11). */
    TRY(read16(d, VL53L1X_MODEL_ID_REG, &model));
    if (model != VL53L1X_MODEL_ID) {
        tof_err(&d->tof, "model ID 0x%04X, not 0x%04X", (unsigned)model,
                (unsigned)VL53L1X_MODEL_ID);
        return -ENODEV;
    }

    TRY(load_config(d));

    /* Interrupt polarity is fixed by the configuration above, so read it
     * once rather than on every poll (api.c:270-271). */
    TRY(read8(d, VL53L1X_GPIO_HV_MUX_CTRL, &mux));
    d->int_polarity = !(mux & VL53L1X_MUX_POLARITY);

    TRY(set_mode(d, mode ? mode : VL53L1X_LONG));
    TRY(set_budget(d, budget));
    TRY(set_period(d, period));
    TRY(calibrate(d));

    d->tof.ops = &vl53l1x_ops;
    d->tof.priv = d;
    return 0;
}

/*
 * Presence first, before anything is allocated: nothing ACKing is -ENXIO.
 * Then the handle, its bus, and setup().
 */
static int open_dev(tof_i2c_bus bus, uint32_t scl_hz, vl53l1x_mode_t mode,
                    uint16_t budget_ms, uint16_t period_ms,
                    vl53l1x_handle_t *out)
{
    vl53l1x_handle_t d;
    int err;

    TRY(tof_i2c_probe(bus, VL53L1X_ADDR_DEFAULT, TIMEOUT_MS));

    d = calloc(1, sizeof *d);
    if (d == NULL)
        return -ENOMEM;
    d->tof.name = "vl53l1x";
    d->i2c = bus;
    d->addr = VL53L1X_ADDR_DEFAULT;
    d->scl_speed_hz = scl_hz;

    err = tof_i2c_attach(&d->bus, bus, d->addr, scl_hz, TIMEOUT_MS);
    if (!err)
        err = setup(d, mode, budget_ms, period_ms);
    if (err) {
        release(d);
        return err;
    }
    *out = d;
    return 0;
}

#ifdef ESP_PLATFORM
/* Hold the sensor in reset, then release it, so init works the same on a
 * warm restart as on a cold one. */
static int xshut_pulse(gpio_num_t pin)
{
    gpio_config_t io = { .mode = GPIO_MODE_OUTPUT };

    if (pin == GPIO_NUM_NC)
        return 0;
    io.pin_bit_mask = 1ULL << pin;
    TRY(tof_errno(gpio_config(&io)));
    TRY(tof_errno(gpio_set_level(pin, 0)));
    tof_sleep_ms(XSHUT_MS);
    TRY(tof_errno(gpio_set_level(pin, 1)));
    tof_sleep_ms(XSHUT_MS);
    return 0;
}

static int init(i2c_master_bus_handle_t bus, const vl53l1x_config_t *cfg,
                vl53l1x_handle_t *out)
{
    static const vl53l1x_config_t defaults = { .xshut_gpio = GPIO_NUM_NC };

    if (bus == NULL || out == NULL)
        return -EINVAL;
    if (cfg == NULL)
        cfg = &defaults;
    TRY(xshut_pulse(cfg->xshut_gpio));
    return open_dev(bus, cfg->scl_speed_hz, cfg->mode, cfg->budget_ms,
                    cfg->period_ms, out);
}
#elif defined(HAL_I2C_MODULE_ENABLED)
static void xshut_pulse(GPIO_TypeDef *port, uint16_t pin)
{
    if (port == NULL)
        return;
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
    tof_sleep_ms(XSHUT_MS);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
    tof_sleep_ms(XSHUT_MS);
}

static int init(I2C_HandleTypeDef *bus, const vl53l1x_config_t *cfg,
                vl53l1x_handle_t *out)
{
    static const vl53l1x_config_t defaults = { 0 };

    if (bus == NULL || out == NULL)
        return -EINVAL;
    if (cfg == NULL)
        cfg = &defaults;
    xshut_pulse(cfg->xshut_port, cfg->xshut_pin);
    return open_dev(bus, 0, cfg->mode, cfg->budget_ms, cfg->period_ms, out);
}
#endif

/*
 * The new address takes from the next transfer on (api.c:217-223; ST's
 * takes the 8-bit form and shifts it, this the 7-bit one). The new bus
 * binding comes first: if it fails nothing has been written to the chip,
 * and both stay where they were.
 */
static int set_address(vl53l1x_handle_t d, uint8_t addr)
{
    struct tof_bus next = { 0 };
    int err;

    if (addr > 0x7F)
        return -EINVAL;
    if (addr == d->addr)
        return 0;

    TRY(tof_i2c_attach(&next, d->i2c, addr, d->scl_speed_hz, TIMEOUT_MS));
    err = write8(d, VL53L1X_I2C_SLAVE_ADDR, addr);
    if (err) {
        tof_bus_detach(&next);
        return err;
    }
    tof_bus_detach(&d->bus);
    d->bus = next;
    d->addr = addr;
    return 0;
}

/* The handle API's result: ST's code, and the rates x8 into 16 bits,
 * wrapping above 8191 as ST's driver does (api.c:637-639) and
 * esp-vl53l1x-tof did. */
static int read_handle(vl53l1x_handle_t d, vl53l1x_result_t *out)
{
    struct raw_result r;

    TRY(read_result(d, &r));
    out->distance_mm = r.distance_mm;
    out->status = r.status;
    out->signal_kcps = (uint16_t)(r.signal * VL53L1X_RATE_TO_KCPS);
    out->ambient_kcps = (uint16_t)(r.ambient * VL53L1X_RATE_TO_KCPS);
    return 0;
}

/* ---------------------------------------------------- the handle API -- */

#ifdef ESP_PLATFORM
esp_err_t vl53l1x_init(i2c_master_bus_handle_t bus, const vl53l1x_config_t *cfg,
                       vl53l1x_handle_t *out)
{
    return TOF_HANDLE_RET(init(bus, cfg, out));
}
#elif defined(HAL_I2C_MODULE_ENABLED)
int vl53l1x_init(I2C_HandleTypeDef *bus, const vl53l1x_config_t *cfg,
                 vl53l1x_handle_t *out)
{
    return init(bus, cfg, out);
}
#endif

tof_handle_ret vl53l1x_start(vl53l1x_handle_t dev)
{
    if (dev == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    return TOF_HANDLE_RET(op_start(&dev->tof));
}

tof_handle_ret vl53l1x_stop(vl53l1x_handle_t dev)
{
    if (dev == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    return TOF_HANDLE_RET(op_stop(&dev->tof));
}

tof_handle_ret vl53l1x_data_ready(vl53l1x_handle_t dev, bool *ready)
{
    if (dev == NULL || ready == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    return TOF_HANDLE_RET(data_ready(dev, ready));
}

/* Whatever the chip holds, ready or not, as esp-vl53l1x-tof read it. */
tof_handle_ret vl53l1x_read(vl53l1x_handle_t dev, vl53l1x_result_t *out)
{
    if (dev == NULL || out == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    return TOF_HANDLE_RET(read_handle(dev, out));
}

tof_handle_ret vl53l1x_read_blocking(vl53l1x_handle_t dev,
                                     vl53l1x_result_t *out,
                                     uint32_t timeout_ms)
{
    int err;

    if (dev == NULL || out == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    err = wait_ready(dev, timeout_ms);
    if (!err)
        err = read_handle(dev, out);
    return TOF_HANDLE_RET(err);
}

tof_handle_ret vl53l1x_set_address(vl53l1x_handle_t dev, uint8_t addr)
{
    if (dev == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    return TOF_HANDLE_RET(set_address(dev, addr));
}

/*
 * ST's driver names 0, 1, 2 and 7 (api.h:278-279); 4 and 5 are
 * esp-vl53l1x-tof's names. ST's codes 3, 6 and 9-13 have no name in ST's
 * driver; UM2510, which has the table, was not consulted.
 */
const char *vl53l1x_status_str(uint8_t status)
{
    switch (status) {
    case VL53L1X_VALID:         return "valid";
    case VL53L1X_SIGMA_FAIL:    return "too noisy to trust";
    case VL53L1X_SIGNAL_FAIL:   return "too little light back";
    case VL53L1X_OUT_OF_BOUNDS: return "out of bounds";
    case VL53L1X_HARDWARE_FAIL: return "hardware failure";
    case VL53L1X_WRAPAROUND:    return "wraparound - target beyond range";
    case VL53L1X_STATUS_NONE:   return "no measurement";
    default:                    return "not valid (a code ST does not name)";
    }
}

tof_handle_ret vl53l1x_del(vl53l1x_handle_t dev)
{
    if (dev == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    release(dev);
    return TOF_HANDLE_RET(0);
}

struct tof_dev *vl53l1x_tof(vl53l1x_handle_t dev)
{
    return dev ? &dev->tof : NULL;
}

#endif /* CONFIG_TOF_VL53L1X */
