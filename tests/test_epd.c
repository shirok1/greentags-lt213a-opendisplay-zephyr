/* Exercise the actual bit-banged driver with a virtual panel/GPIO clock. */
#include "epd.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static unsigned pins[32], resets, bits, byte;
static bool input_connected, stuck;
static int64_t now;
static struct { unsigned value, data; } wire[12000];
static unsigned count;
void nrf_gpio_pin_write(unsigned pin, unsigned value)
{
    if (pin == 3 && pins[pin] && !value) { resets++; }
    if (pin == 0 && !pins[pin] && value && !pins[1]) {
        byte = (byte << 1) | pins[30]; bits++;
    }
    if (pin == 1 && !pins[pin] && value && bits) {
        assert(bits == 8 && count < 12000);
        wire[count].value = byte; wire[count++].data = pins[2];
        bits = byte = 0;
    }
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
    assert(!epd_begin() && resets == 2 && input_connected);
    assert(has_command(previous, 0x04) && has_command(previous, 0x13));
    uint8_t pixels[] = {0xaa, 0x55};
    assert(!epd_write(pixels, sizeof(pixels)));
    assert(wire[count - 2].data && wire[count - 2].value == 0xaa);
    previous = count;
    assert(!epd_finish(0) && has_command(previous, 0x12));
    assert(has_command(previous, 0x02));
    if (CONFIG_LT213A_EPD_DEEP_SLEEP) {
        assert(!wire[count - 2].data && wire[count - 2].value == 7);
        assert(wire[count - 1].data && wire[count - 1].value == 0xa5);
    }
    check_idle();
    /* A timeout cannot mark the panel asleep or issue deep sleep prematurely. */
    count = 0; stuck = true;
    assert(epd_begin() < 0 && !has_command(0, 7) && input_connected);
    stuck = false; count = 0;
    assert(!epd_off() && has_command(0, 2));
    check_idle();
    puts("Panel power lifecycle passed: boot, repeated off, reset wake, refresh, timeout recovery");
}
