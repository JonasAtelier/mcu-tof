/* First: on ESP-IDF it brings in sdkconfig.h, which the #if below reads. */
#include "tof_priv.h"

#if CONFIG_TOF_TFLUNA

#include <stdlib.h>
#include <string.h>

#include "tfluna.h"
#include "tfluna_proto.h"
#include "tof_log.h"

#define I2C_TIMEOUT_MS      100     /* one bus transfer */
#define DEFAULT_TIMEOUT_MS  100     /* ten frames at the default 100 Hz */

/*
 * The most bytes one search of the stream looks through. Fourteen data
 * frames: a reply that comes in among them at the default 100 Hz, or the
 * next frame after a start in the middle of one, is well inside it.
 */
#define SCAN_BUDGET         128

#define TRY(expr) do { int _e = (expr); if (_e) return _e; } while (0)

struct tfluna_t {
    struct tof_dev  tof;            /* the category view; priv points back */
    struct tof_bus  bus;
    bool            uart;
    bool            millimetres;    /* UART frames carry mm, not cm */
    bool            triggered;      /* a trigger went out: keep what comes */
};

/*
 * A walk along the UART stream. The window holds the bytes not yet ruled
 * out as the start of a frame, so a header hiding inside a rejected one is
 * still found and no byte is dropped unexamined.
 */
struct scan {
    struct tof_bus *bus;
    uint8_t  window[TFLUNA_REPLY_MAX];
    size_t   have;          /* bytes in window */
    size_t   budget;        /* bytes still allowed to arrive */
    size_t   received;      /* bytes that did arrive */
    bool     drop_stale;    /* the first receive starts on fresh data */
    unsigned valid;         /* frames whose checksum matched */
};

static void scan_start(struct scan *scan, struct tof_bus *bus, bool drop_stale)
{
    memset(scan, 0, sizeof *scan);
    scan->bus = bus;
    scan->budget = SCAN_BUDGET;
    scan->drop_stale = drop_stale;
}

static void scan_drop(struct scan *scan)
{
    scan->have--;
    memmove(scan->window, scan->window + 1, scan->have);
}

/*
 * The next frame, data or reply, whose checksum checks out; its length in
 * *len. Out of budget: -ENODATA if some frames were valid, -EBADMSG if none
 * ever was. A stream that stops is the bus's -ETIMEDOUT.
 */
static int next_frame(struct scan *scan, uint8_t frame[TFLUNA_REPLY_MAX],
                      size_t *len)
{
    size_t want, need;

    for (;;) {
        if (scan->have && !tfluna_header(scan->window[0])) {
            scan_drop(scan);
            continue;
        }
        want = scan->have < 2 ? 2 : tfluna_frame_len(scan->window);
        if (want == 0) {
            scan_drop(scan);
            continue;
        }
        if (scan->have < want) {
            need = want - scan->have;
            if (need > scan->budget)
                return scan->valid ? -ENODATA : -EBADMSG;
            TRY(tof_bus_receive(scan->bus, scan->window + scan->have, need,
                                scan->drop_stale));
            scan->drop_stale = false;
            scan->budget -= need;
            scan->received += need;
            scan->have += need;
            continue;
        }
        if (tfluna_frame_valid(scan->window, want)) {
            memcpy(frame, scan->window, want);
            *len = want;
            scan->have = 0;
            scan->valid++;
            return 0;
        }
        scan_drop(scan);
    }
}

/* The next data frame; replies passing by are skipped. */
static int next_data(struct scan *scan, uint8_t frame[TFLUNA_REPLY_MAX])
{
    size_t len;

    for (;;) {
        TRY(next_frame(scan, frame, &len));
        if (frame[0] == TFLUNA_DATA_HEADER)
            return 0;
    }
}

/*
 * Send a command and wait for the reply with the same ID; data frames pass
 * by. The UART transfer drops stale input before it sends, so the search
 * keeps everything after it. -ENODATA if frames come but not that one.
 */
static int command(struct tfluna_t *d, struct scan *scan, uint8_t id,
                   const uint8_t *payload, size_t n,
                   uint8_t reply[TFLUNA_REPLY_MAX])
{
    uint8_t frame[TFLUNA_REPLY_MAX];
    size_t len = tfluna_command(frame, id, payload, n);

    TRY(d->bus.ops->transfer(&d->bus, frame[0], frame + 1, len - 1, NULL,
                             0));
    scan_start(scan, &d->bus, false);
    for (;;) {
        TRY(next_frame(scan, reply, &len));
        if (reply[0] == TFLUNA_CMD_HEADER && reply[TFLUNA_CMD_ID] == id)
            return 0;
    }
}

/* ------------------------------------------------------- measuring -- */

static int read_uart(struct tfluna_t *d, tfluna_result_t *out)
{
    uint8_t frame[TFLUNA_REPLY_MAX];
    struct scan scan;
    uint16_t dist;

    /* Fresh data, unless a trigger just asked for the next frame. */
    scan_start(&scan, &d->bus, !d->triggered);
    d->triggered = false;
    TRY(next_data(&scan, frame));
    dist = tfluna_le16(frame + TFLUNA_DATA_DIST);
    out->distance_mm = d->millimetres ? dist : (uint16_t)(dist * 10);
    out->amp = tfluna_le16(frame + TFLUNA_DATA_AMP);
    out->temp_c = tfluna_uart_temp_c(tfluna_le16(frame + TFLUNA_DATA_TEMP));
    return 0;
}

/*
 * DIST, AMP and TEMP in one read. In continuous mode the manual asks for
 * reads in step with pin 6, or one may land on an update (6.3, p.10).
 */
static int read_i2c(struct tfluna_t *d, tfluna_result_t *out)
{
    uint8_t b[TFLUNA_REG_MEASURE_LEN];

    TRY(d->bus.ops->transfer(&d->bus, TFLUNA_REG_DIST, NULL, 0, b,
                             sizeof b));
    out->distance_mm = (uint16_t)(tfluna_le16(b) * 10);
    out->amp = tfluna_le16(b + TFLUNA_REG_AMP);
    out->temp_c = tfluna_i2c_temp_c(tfluna_le16(b + TFLUNA_REG_TEMP));
    return 0;
}

static int read_result(struct tfluna_t *d, tfluna_result_t *out)
{
    return d->uart ? read_uart(d, out) : read_i2c(d, out);
}

static int reg_write(struct tfluna_t *d, uint8_t reg, const uint8_t *v,
                     size_t n)
{
    return d->bus.ops->transfer(&d->bus, reg, v, n, NULL, 0);
}

static int reg_write8(struct tfluna_t *d, uint8_t reg, uint8_t v)
{
    return reg_write(d, reg, &v, 1);
}

/* The module answers SAMPLE_FREQ with the rate it now runs at (Appendix II
 * item 3, p.23). */
static int set_rate(struct tfluna_t *d, uint16_t hz)
{
    uint8_t le[2] = { (uint8_t)hz, (uint8_t)(hz >> 8) };
    uint8_t reply[TFLUNA_REPLY_MAX];
    struct scan scan;

    if (hz > TFLUNA_RATE_MAX_HZ)
        return -EINVAL;
    if (!d->uart) {
        if (hz == 0)
            return reg_write8(d, TFLUNA_REG_MODE, TFLUNA_MODE_TRIGGER);
        TRY(reg_write8(d, TFLUNA_REG_MODE, TFLUNA_MODE_CONTINUOUS));
        return reg_write(d, TFLUNA_REG_FPS, le, sizeof le);
    }
    TRY(command(d, &scan, TFLUNA_ID_SAMPLE_FREQ, le, sizeof le, reply));
    if (reply[TFLUNA_CMD_LEN] != TFLUNA_CMD_MIN_LEN + 2 ||
        tfluna_le16(reply + TFLUNA_CMD_PAYLOAD) != hz)
        return -EIO;
    return 0;
}

/* The answer to SAMPLE_TRIG is a data frame (Appendix II item 4, p.23):
 * the next read keeps what has come since instead of dropping it. */
static int trigger(struct tfluna_t *d)
{
    uint8_t frame[TFLUNA_CMD_MIN_LEN];
    size_t len;

    if (!d->uart)
        return reg_write8(d, TFLUNA_REG_TRIG_ONE_SHOT, TFLUNA_TRIGGER_ONCE);
    len = tfluna_command(frame, TFLUNA_ID_SAMPLE_TRIG, NULL, 0);
    TRY(d->bus.ops->transfer(&d->bus, frame[0], frame + 1, len - 1, NULL,
                             0));
    d->triggered = true;
    return 0;
}

/* SAVE answers with a status, 0 for success (Appendix II item 11, p.26). */
static int save(struct tfluna_t *d)
{
    uint8_t reply[TFLUNA_REPLY_MAX];
    struct scan scan;

    if (!d->uart)
        return reg_write8(d, TFLUNA_REG_SAVE, TFLUNA_SAVE_NOW);
    TRY(command(d, &scan, TFLUNA_ID_SAVE, NULL, 0, reply));
    if (reply[TFLUNA_CMD_LEN] != TFLUNA_CMD_MIN_LEN + 1 ||
        reply[TFLUNA_CMD_PAYLOAD] != 0)
        return -EIO;
    return 0;
}

/* OUTPUT_EN on the UART (Appendix II item 7, p.24), ENABLE on I2C
 * (Appendix III 0x25, p.32-33). Neither is saved. */
static int enable(struct tfluna_t *d, bool on)
{
    uint8_t v = on ? 1 : 0;
    uint8_t reply[TFLUNA_REPLY_MAX];
    struct scan scan;

    if (!d->uart)
        return reg_write8(d, TFLUNA_REG_ENABLE, v);
    TRY(command(d, &scan, TFLUNA_ID_OUTPUT_EN, &v, 1, reply));
    if (reply[TFLUNA_CMD_LEN] != TFLUNA_CMD_MIN_LEN + 1 ||
        reply[TFLUNA_CMD_PAYLOAD] != v)
        return -EIO;
    return 0;
}

/* -------------------------------------------------- the tof_dev ops -- */

static struct tfluna_t *of(struct tof_dev *tof)
{
    return tof->priv;
}

static int op_start(struct tof_dev *tof)
{
    return enable(of(tof), true);
}

static int op_stop(struct tof_dev *tof)
{
    return enable(of(tof), false);
}

static int op_read(struct tof_dev *tof, struct tof_sample *s)
{
    tfluna_result_t r;

    TRY(read_result(of(tof), &r));
    s->distance_mm = r.distance_mm;
    s->signal = r.amp;
    s->ambient = 0;
    s->temp_c = r.temp_c;
    s->raw_status = 0;      /* the module has no status code of its own */
    s->status = tfluna_status(r.amp);
    return 0;
}

static const struct tof_ops tfluna_ops = {
    .start = op_start,
    .stop = op_stop,
    .read = op_read,
};

/* ------------------------------------------------------------- init ---- */

static void release(struct tfluna_t *d)
{
    if (d->bus.ops)
        tof_bus_detach(&d->bus);
    free(d);
}

static struct tfluna_t *alloc(bool uart, bool millimetres)
{
    struct tfluna_t *d = calloc(1, sizeof *d);

    if (d == NULL)
        return NULL;
    d->tof.name = "tfluna";
    d->tof.ops = &tfluna_ops;
    d->tof.priv = d;
    d->uart = uart;
    d->millimetres = millimetres;
    return d;
}

#ifdef ESP_PLATFORM
typedef uart_port_t tfluna_uart;
#elif defined(HAL_UART_MODULE_ENABLED)
typedef UART_HandleTypeDef *tfluna_uart;
#endif

#if defined(ESP_PLATFORM) || defined(HAL_UART_MODULE_ENABLED)
/*
 * Ask for the version (Appendix II item 1, p.22): it answers in either mode,
 * streaming or triggered, and only a TF-Luna answers it so. Decided on a
 * bus on the stack, before anything is allocated.
 */
static int probe_uart(struct tfluna_t *probe)
{
    uint8_t reply[TFLUNA_REPLY_MAX];
    struct scan scan;
    int ret;

    ret = command(probe, &scan, TFLUNA_ID_GET_VERSION, NULL, 0, reply);
    if (ret == -ETIMEDOUT && !scan.received) {
        tof_dbg(&probe->tof, "nothing on the UART (%d)", ret);
        return -ENXIO;
    }
    if (ret == -EBADMSG) {
        tof_err(&probe->tof, "bytes arrive, but no frame's checksum "
                "matches: check the baud rate");
        return ret;
    }
    if (ret == -ENODATA) {
        tof_err(&probe->tof, "frames arrive, but no answer to the version "
                "request");
        return -ENODEV;
    }
    if (ret) {
        tof_err(&probe->tof, "reading the UART failed (%d)", ret);
        return ret;
    }
    if (reply[TFLUNA_CMD_LEN] != TFLUNA_CMD_MIN_LEN + 3)
        return -ENODEV;
    /* "112, 50, 9 ... is 9.50.112" (Appendix II item 1, p.22). */
    tof_info(&probe->tof, "UART, firmware %u.%u.%u",
             (unsigned)reply[TFLUNA_CMD_PAYLOAD + 2],
             (unsigned)reply[TFLUNA_CMD_PAYLOAD + 1],
             (unsigned)reply[TFLUNA_CMD_PAYLOAD]);
    return 0;
}

static int init_uart(tfluna_uart port, const tfluna_config_t *cfg,
                     tfluna_handle_t *out)
{
    static const tfluna_config_t defaults = { 0 };
    struct tfluna_t probe = { .tof.name = "tfluna", .uart = true };
    struct tfluna_t *d;
    int ret;

    if (out == NULL)
        return -EINVAL;
#ifndef ESP_PLATFORM
    if (port == NULL)
        return -EINVAL;
#endif
    if (cfg == NULL)
        cfg = &defaults;

    TRY(tof_uart_attach(&probe.bus, port,
                        cfg->timeout_ms ? cfg->timeout_ms
                                        : DEFAULT_TIMEOUT_MS));
    ret = probe_uart(&probe);
    if (ret) {
        tof_bus_detach(&probe.bus);
        return ret;
    }

    d = alloc(true, cfg->millimetres);
    if (d == NULL) {
        tof_bus_detach(&probe.bus);
        return -ENOMEM;
    }
    d->bus = probe.bus;
    *out = d;
    return 0;
}
#endif

#if defined(ESP_PLATFORM) || defined(HAL_I2C_MODULE_ENABLED)
/*
 * Presence first, before anything is allocated: nothing ACKing is -ENXIO.
 * Then the signature, "LUNA" at 0x3C-0x3F (Appendix III, p.33).
 */
static int init_i2c(tof_i2c_bus bus, const tfluna_config_t *cfg,
                    tfluna_handle_t *out)
{
    static const tfluna_config_t defaults = { 0 };
    struct tfluna_t *d;
    uint8_t sig[4], ver[3];
    uint8_t addr;
    int err;

    if (bus == NULL || out == NULL)
        return -EINVAL;
    if (cfg == NULL)
        cfg = &defaults;
    if (cfg->addr != 0 && (cfg->addr < TFLUNA_ADDR_MIN ||
                           cfg->addr > TFLUNA_ADDR_MAX))
        return -EINVAL;
    if (cfg->scl_speed_hz > TFLUNA_SCL_MAX_HZ)
        return -EINVAL;
    addr = cfg->addr ? cfg->addr : TFLUNA_ADDR_DEFAULT;

    TRY(tof_i2c_probe(bus, addr, I2C_TIMEOUT_MS));

    d = alloc(false, false);
    if (d == NULL)
        return -ENOMEM;
    err = tof_i2c_attach(&d->bus, bus, addr, cfg->scl_speed_hz,
                         I2C_TIMEOUT_MS);
    if (!err)
        err = d->bus.ops->transfer(&d->bus, TFLUNA_REG_SIGNATURE, NULL, 0,
                                   sig, sizeof sig);
    if (!err && memcmp(sig, "LUNA", sizeof sig)) {
        tof_err(&d->tof, "signature %02X %02X %02X %02X, not \"LUNA\"",
                sig[0], sig[1], sig[2], sig[3]);
        err = -ENODEV;
    }
    if (!err)
        err = d->bus.ops->transfer(&d->bus, TFLUNA_REG_VERSION, NULL, 0,
                                   ver, sizeof ver);
    if (err) {
        release(d);
        return err;
    }
    tof_info(&d->tof, "I2C at 0x%02X, firmware %u.%u.%u", (unsigned)addr,
             (unsigned)ver[2], (unsigned)ver[1], (unsigned)ver[0]);
    *out = d;
    return 0;
}
#endif

/* ---------------------------------------------------- the handle API -- */

#ifdef ESP_PLATFORM
esp_err_t tfluna_init_uart(uart_port_t port, const tfluna_config_t *cfg,
                           tfluna_handle_t *out)
{
    return TOF_HANDLE_RET(init_uart(port, cfg, out));
}

esp_err_t tfluna_init_i2c(i2c_master_bus_handle_t bus,
                          const tfluna_config_t *cfg, tfluna_handle_t *out)
{
    return TOF_HANDLE_RET(init_i2c(bus, cfg, out));
}
#else
#ifdef HAL_UART_MODULE_ENABLED
int tfluna_init_uart(UART_HandleTypeDef *uart, const tfluna_config_t *cfg,
                     tfluna_handle_t *out)
{
    return init_uart(uart, cfg, out);
}
#endif
#ifdef HAL_I2C_MODULE_ENABLED
int tfluna_init_i2c(I2C_HandleTypeDef *bus, const tfluna_config_t *cfg,
                    tfluna_handle_t *out)
{
    return init_i2c(bus, cfg, out);
}
#endif
#endif

tof_handle_ret tfluna_read(tfluna_handle_t dev, tfluna_result_t *out)
{
    tfluna_result_t r;
    int err;

    if (dev == NULL || out == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    err = read_result(dev, &r);
    if (!err)
        *out = r;
    return TOF_HANDLE_RET(err);
}

tof_handle_ret tfluna_set_rate(tfluna_handle_t dev, uint16_t hz)
{
    if (dev == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    return TOF_HANDLE_RET(set_rate(dev, hz));
}

tof_handle_ret tfluna_trigger(tfluna_handle_t dev)
{
    if (dev == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    return TOF_HANDLE_RET(trigger(dev));
}

tof_handle_ret tfluna_save(tfluna_handle_t dev)
{
    if (dev == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    return TOF_HANDLE_RET(save(dev));
}

tof_handle_ret tfluna_del(tfluna_handle_t dev)
{
    if (dev == NULL)
        return TOF_HANDLE_RET(-EINVAL);
    release(dev);
    return TOF_HANDLE_RET(0);
}

bool tfluna_valid(const tfluna_result_t *r)
{
    return r && tfluna_status(r->amp) == TOF_VALID;
}

struct tof_dev *tfluna_tof(tfluna_handle_t dev)
{
    return dev ? &dev->tof : NULL;
}

#endif /* CONFIG_TOF_TFLUNA */
