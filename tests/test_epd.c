/* Exercise the actual bit-banged driver with a virtual panel/GPIO clock. */
#include "epd.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static unsigned pins[32], resets, bits, byte, selections;
static bool input_connected, stuck;
static int64_t now;
static struct { unsigned value, data; } wire[12000];
static unsigned count;
void nrf_gpio_pin_write(unsigned pin, unsigned value)
{
    if (pin == 3 && pins[pin] && !value) { resets++; }
    if (pin == 1 && pins[pin] && !value) { selections++; }
    if (pin == 0 && !pins[pin] && value && !pins[1]) {
        byte = (byte << 1) | pins[30]; bits++;
        if (bits == 8) {
            assert(count < 12000);
            wire[count].value = byte; wire[count++].data = pins[2];
            bits = byte = 0;
        }
    }
    if (pin == 1 && !pins[pin] && value) { assert(bits == 0); }
    if (pin == 2 && pins[pin] != value) { assert(bits == 0); }
    pins[pin] = value;
}
void nrf_gpio_cfg_output(unsigned pin) { (void)pin; }
void nrf_gpio_cfg_input(unsigned pin, unsigned pull)
{ assert(pin == 4 && pull == 0); input_connected = true; }
void nrf_gpio_cfg_default(unsigned pin)
{ assert(pin == 4); input_connected = false; }
unsigned nrf_gpio_pin_read(unsigned pin)
{ assert(pin == 4 && input_connected); return !stuck; }
int64_t k_uptime_get(void) { return now; }
void k_msleep(int ms) { now += ms; }
void k_busy_wait(unsigned us) { (void)us; }

static bool has_command(unsigned start, unsigned command)
{
    for (unsigned i = start; i < count; i++) {
        if (!wire[i].data && wire[i].value == command) { return true; }
    }
    return false;
}
static void check_register(unsigned command, const uint8_t *expected, unsigned length)
{
    for (unsigned i = 0; i < count; i++) {
        if (wire[i].data || wire[i].value != command) { continue; }
        for (unsigned j = 0; j < length; j++) {
            assert(i + j + 1 < count && wire[i + j + 1].data);
            assert(wire[i + j + 1].value == expected[j]);
        }
        return;
    }
    assert(!"Missing register");
}
static void check_idle(void)
{
    assert(pins[1] == 1 && pins[3] == 1 && pins[0] == 0 && pins[30] == 0 && pins[2] == 0 && pins[5] == 0);
    assert(input_connected == !CONFIG_LT213A_EPD_DEEP_SLEEP);
}
int main(void)
{
    assert(!epd_init());
    assert(resets == 1 && has_command(0, 0x02));
    assert(has_command(0, 0x07) == !!CONFIG_LT213A_EPD_DEEP_SLEEP);
    check_idle();
    unsigned previous = count;
    assert(!epd_off() && count == previous); /* no SPI access to sleeping panel */
    int64_t begin_at = now;
    unsigned select_at = selections;
    assert(!epd_begin() && resets == 2 && input_connected);
    assert(now - begin_at < 100); /* Ready panel: no 200 ms fixed tail. */
    assert(selections - select_at < 64); /* Old plane is one SPI burst. */
    assert(has_command(previous, 0x04) && has_command(previous, 0x13));
    uint8_t pixels[] = {0xaa, 0x55};
    assert(!epd_write(pixels, sizeof(pixels)));
    assert(pins[1] == 1 && pins[0] == 0);
    assert(wire[count - 2].data && wire[count - 2].value == 0xaa);
    previous = count;
    assert(!epd_finish(0) && has_command(previous, 0x12));
    assert(has_command(previous, 0x02));
    if (CONFIG_LT213A_EPD_DEEP_SLEEP) {
        assert(!wire[count - 2].data && wire[count - 2].value == 7);
        assert(wire[count - 1].data && wire[count - 1].value == 0xa5);
    }
    check_idle();
    /* Non-square T5 region: verify vendor settings and both inverted planes. */
    count = 0;
    assert(!epd_region(8, 3, 16, 2));
    check_register(0x01, (uint8_t[]){3, 2, 0x21, 0x21}, 4);
    check_register(0x00, (uint8_t[]){0xbf, 0x0d}, 2);
    check_register(0x30, (uint8_t[]){0x3c}, 1);
    check_register(0x50, (uint8_t[]){0x47}, 1);
    check_register(0x90, (uint8_t[]){8, 23, 0, 3, 0, 4, 0x28}, 7);
    const uint8_t transitions[] = {0, 0, 0x20, 0x10, 0};
    for (unsigned i = 0; i < 5; i++) {
        uint8_t lut[44] = {transitions[i], 1, 100, 0, 0, 1};
        check_register(0x20 + i, lut, i ? 42 : 44);
    }
    uint8_t planes[] = {0x00, 0xff, 0xaa, 0x55, 0xff, 0x00, 0x55, 0xaa};
    assert(!epd_write(planes, 3) && !epd_write(planes + 3, 5));
    check_register(0x10, (uint8_t[]){0xff, 0, 0x55, 0xaa}, 4);
    check_register(0x13, (uint8_t[]){0, 0xff, 0xaa, 0x55}, 4);
    assert(!epd_finish(2));
    check_idle();
    /* A timeout cannot mark the panel asleep or issue deep sleep prematurely. */
    count = 0; stuck = true;
    assert(epd_begin() < 0 && !has_command(0, 7) && input_connected);
    stuck = false; count = 0;
    assert(!epd_off() && has_command(0, 2));
    check_idle();
    puts("Panel power lifecycle passed: boot, repeated off, reset wake, refresh, timeout recovery");
}
