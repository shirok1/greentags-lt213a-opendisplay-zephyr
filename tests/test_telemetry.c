#include "telemetry.h"
#include <zephyr/drivers/sensor.h>
#include <hal/nrf_adc.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct device fake_sensor;
struct adc adc;
static int64_t now;
static int temp_failure, adc_failure;
static struct sensor_value temperature;
int device_is_ready(const struct device *dev) { (void)dev; return 1; }
int sensor_sample_fetch(const struct device *dev) { (void)dev; return temp_failure; }
int sensor_channel_get(const struct device *dev, int channel, struct sensor_value *value)
{ (void)dev; (void)channel; *value = temperature; return 0; }
int64_t k_uptime_get(void) { return now; }
void k_msleep(int ms)
{
    now += ms;
    if (!adc_failure) { adc.EVENTS_END = 1; }
}
static void sample(int degrees, int fraction, unsigned raw, unsigned encoded_temp, unsigned voltage)
{
    uint8_t msd[16];
    memset(msd, 0xaf, sizeof msd);
    temperature = (struct sensor_value){degrees, fraction};
    adc.RESULT = raw;
    adc.TASKS_STOP = 0;
    assert(!od_read_msd(msd));
    for (unsigned i = 0; i < 13; i++) { assert(msd[i] == 0xaf); }
    assert(msd[13] == encoded_temp && msd[14] == (voltage & 255));
    assert(msd[15] == (0xae | (voltage >> 8)));
    assert(adc.CONFIG == 14 && adc.TASKS_STOP && !adc.ENABLE);
}
int main(void)
{
    sample(25, 250000, 853, 130, 300);
    sample(-10, -750000, 512, 59, 180);
    sample(-50, 0, 0, 0, 0);
    sample(100, 0, 1023, 255, 360);
    uint8_t msd[16], previous[16];
    memset(msd, 0xff, sizeof msd);
    memcpy(previous, msd, sizeof msd);
    adc_failure = 1;
    int64_t started = now;
    assert(od_read_msd(msd) == -ETIMEDOUT && now - started == 50);
    assert(!memcmp(previous, msd, sizeof msd) && !adc.ENABLE && adc.TASKS_STOP);
    temp_failure = 1;
    adc.TASKS_START = 0;
    assert(od_read_msd(msd) == -EIO && !adc.TASKS_START);
    assert(!memcmp(previous, msd, sizeof msd));
    puts("Telemetry encoding, snapshot preservation and ADC timeout/shutdown passed");
}
