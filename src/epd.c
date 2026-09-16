/* GDEW0213T5 register sequence from Good Display's 2019-10-16 demo.
 * Four-wire, mode-0, MSB-first SPI; BS=0. No readback on bidirectional SDA.
 */
#include "epd.h"
#include "config_store.h"
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/gpio/gpio.h>
#include <hal/nrf_gpio.h>
#include <zephyr/kernel.h>
#include <errno.h>

struct pin { uint8_t number; bool inverted; };
#define PIN(name) static const struct pin name = { \
    DT_GPIO_PIN(DT_PATH(zephyr_user), name##_gpios), \
    (DT_GPIO_FLAGS(DT_PATH(zephyr_user), name##_gpios) & GPIO_ACTIVE_LOW) != 0 }
PIN(mosi); PIN(sck); PIN(cs); PIN(dc); PIN(reset); PIN(busy); PIN(bs);
static void gpio_pin_set_dt(const struct pin *pin, bool active)
{ nrf_gpio_pin_write(pin->number, active != pin->inverted); }
static int gpio_pin_get_dt(const struct pin *pin)
{ return (nrf_gpio_pin_read(pin->number) != 0) != pin->inverted; }

/* GPIO operations do not mask interrupts: BLE radio deadlines take priority. */
static void shift_out(uint8_t value)
{
    for (unsigned int i = 0; i < 8; i++) {
        gpio_pin_set_dt(&sck, 0);
        gpio_pin_set_dt(&mosi, (value & 0x80) != 0);
        k_busy_wait(1);
        gpio_pin_set_dt(&sck, 1);
        k_busy_wait(1);
        value <<= 1;
    }
    gpio_pin_set_dt(&sck, 0);
}
static void byte_out(uint8_t value, bool data)
{
    gpio_pin_set_dt(&dc, data);
    gpio_pin_set_dt(&cs, 1);
    shift_out(value);
    gpio_pin_set_dt(&cs, 0);
}

static void cmd(uint8_t value) { byte_out(value, false); }
static void data(uint8_t value) { byte_out(value, true); }
/* On this LT213A, the vendor's 14-frame phase left both transitions incomplete.
 * Default to the tested 100-frame phase; config can select 1..255 frames.
 * Keep the timing identical in all five LUTs. Calibrate against the actual panel. */
/* BUSY high permits the next operation (panel spec, update flow).
 * Keep a small board settling margin; tune here if a panel needs longer. */
#define READY_SETTLE_MS 10
static bool partial;
static bool awake;
static size_t plane_bytes, plane_written;

static void panel_reset(void)
{
    /* Deep sleep may leave BUSY undriven; only reconnect its input when awake. */
    nrf_gpio_cfg_input(busy.number, NRF_GPIO_PIN_NOPULL);
    awake = true;
    gpio_pin_set_dt(&reset, 1);
    k_msleep(10);
    gpio_pin_set_dt(&reset, 0);
    k_msleep(10);
}

static int wait_ready(void)
{
    int64_t deadline = k_uptime_get() + 30000;
    for (;;) {
        cmd(0x71);
        int active = gpio_pin_get_dt(&busy); /* logical 1 = physical LOW */
        if (active < 0) { return active; }
        if (!active) { k_msleep(READY_SETTLE_MS); return 0; }
        if (k_uptime_get() >= deadline) { return -ETIMEDOUT; }
        k_msleep(10);
    }
}

int epd_init(void)
{
    const struct pin *outputs[] = { &cs, &dc, &reset, &bs, &sck, &mosi };
    for (size_t i = 0; i < ARRAY_SIZE(outputs); i++) {
        gpio_pin_set_dt(outputs[i], 0);
        nrf_gpio_cfg_output(outputs[i]->number);
    }
    panel_reset();
    /* Establish a known idle state even if no image is ever uploaded.
     * A stuck/missing panel must not prevent the BLE interface from starting. */
    (void)epd_off();
    return 0;
}

static int panel_begin(bool region)
{
    partial = region;
    gpio_pin_set_dt(&bs, 0);
    panel_reset();
    if (region) { cmd(0x01); data(0x03); data(0x02); data(0x21); data(0x21); }
    cmd(0x06); data(0x17); data(0x17); data(0x17);
    cmd(0x04);
    k_msleep(10); /* allow BUSY to assert before polling */
    int err = wait_ready();
    if (err) { epd_off(); return err; }
    cmd(0x00); data(region ? 0xbf : 0x1f); data(0x0d);
    if (region) { cmd(0x30); data(0x3c); }
    cmd(0x61); data(0x68); data(0x00); data(0xd4);
    if (region) { return 0; }
    cmd(0x50); data(0x97);
    cmd(0x10);
    gpio_pin_set_dt(&dc, 1);
    gpio_pin_set_dt(&cs, 1);
    for (size_t i = 0; i < EPD_FRAME_BYTES; i++) { shift_out(0xff); }
    gpio_pin_set_dt(&cs, 0);
    cmd(0x13);
    return 0;
}

int epd_begin(void) { return panel_begin(false); }

int epd_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (!w || !h || ((x | w) & 7) || x + w > EPD_WIDTH || y + h > EPD_HEIGHT) { return -EINVAL; }
    int err = panel_begin(true);
    if (err) { return err; }
    /* Good Display GDEW0213T5 Arduino P20201021: register LUT,
     * VCOM / W->W / B->W / W->B / B->B, zero-filled after phase one. */
    cmd(0x82); data(0x08);
    cmd(0x50); data(0x47);
    const uint8_t transitions[] = {0x00, 0x00, 0x20, 0x10, 0x00};
    const uint8_t frames = od_config_partial_frames();
    for (unsigned lut = 0; lut < 5; lut++) {
        cmd(0x20 + lut);
        unsigned count = lut == 0 ? 44 : 42;
        for (unsigned i = 0; i < count; i++) {
            data(i == 0 ? transitions[lut] : i == 2 ? frames : (i == 1 || i == 5) ? 1 : 0);
        }
    }
    cmd(0x91); cmd(0x90);
    data(x); data(x + w - 1);
    data(y >> 8); data(y); data((y + h - 1) >> 8); data(y + h - 1);
    data(0x28);
    cmd(0x10);
    plane_bytes = (w / 8) * h; plane_written = 0; partial = true;
    return 0;
}

int epd_write(const uint8_t *bytes, size_t len)
{
    if (!len) { return 0; }
    gpio_pin_set_dt(&dc, 1);
    gpio_pin_set_dt(&cs, 1);
    for (size_t i = 0; i < len; i++) {
        if (partial && plane_written == plane_bytes) {
            gpio_pin_set_dt(&cs, 0);
            cmd(0x13);
            gpio_pin_set_dt(&dc, 1);
            gpio_pin_set_dt(&cs, 1);
        }
        /* T5 register LUT expects the inverse of OpenDisplay wire pixels. */
        shift_out(partial ? (uint8_t)~bytes[i] : bytes[i]);
        if (partial) { plane_written++; }
    }
    gpio_pin_set_dt(&cs, 0);
    return 0;
}

int epd_finish(uint8_t mode)
{
    if (partial && mode == 0) {
        cmd(0x00); data(0x1f); data(0x0d);
        cmd(0x50); data(0x97);
    }
    cmd(0x12);
    k_msleep(100);
    int err = wait_ready();
    if (partial) { cmd(0x92); }
    int off_err = epd_off();
    return err ? err : off_err;
}

int epd_off(void)
{
    partial = false;
    if (!awake) { return 0; }
    /* Always attempt panel power-off, including on a stuck BUSY line. */
    cmd(0x50); data(0xf7);
    cmd(0x02);
    k_msleep(10);
    int err = wait_ready();
    if (!err) {
        if (IS_ENABLED(CONFIG_LT213A_EPD_DEEP_SLEEP)) {
            cmd(0x07); data(0xa5);
            nrf_gpio_cfg_default(busy.number);
        }
        awake = false;
    }
    gpio_pin_set_dt(&cs, 0);
    gpio_pin_set_dt(&dc, 0);
    gpio_pin_set_dt(&mosi, 0);
    gpio_pin_set_dt(&sck, 0);
    return err;
}
