#include "telemetry.h"
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <hal/nrf_adc.h>
#include <errno.h>

int od_read_msd(uint8_t msd[16])
{
    /* The clock-calibration worker also uses TEMP: keep its driver's mutex and
     * HF clock ownership. The driver stops TEMP after fetch. Its wait is not
     * bounded, so the main-loop hardware watchdog remains the recovery path. */
    const struct device *sensor = DEVICE_DT_GET_ONE(nordic_nrf_temp);
    struct sensor_value value;
    if (!device_is_ready(sensor) || sensor_sample_fetch(sensor) ||
        sensor_channel_get(sensor, SENSOR_CHAN_DIE_TEMP, &value)) { return -EIO; }
    int temperature = value.val1 * 2 + value.val2 / 500000 + 80;

    /* No ADC driver/thread/buffer: this worker is the only ADC owner. 10-bit,
     * VDD/3 input and internal 1.2 V reference give a 3.6 V full scale. */
    NRF_ADC->ENABLE = 1;
    NRF_ADC->CONFIG = (ADC_CONFIG_RES_10bit << ADC_CONFIG_RES_Pos) |
        (ADC_CONFIG_INPSEL_SupplyOneThirdPrescaling << ADC_CONFIG_INPSEL_Pos);
    NRF_ADC->EVENTS_END = 0;
    NRF_ADC->TASKS_START = 1;
    int64_t deadline = k_uptime_get() + 50;
    while (!NRF_ADC->EVENTS_END && k_uptime_get() < deadline) { k_msleep(1); }
    bool ready = NRF_ADC->EVENTS_END;
    unsigned voltage = (NRF_ADC->RESULT * 360u + 511) / 1023;
    NRF_ADC->TASKS_STOP = 1;
    NRF_ADC->ENABLE = 0;
    if (!ready) { return -ETIMEDOUT; }

    msd[13] = CLAMP(temperature, 0, 255);
    msd[14] = voltage;
    msd[15] = (msd[15] & ~1u) | ((voltage >> 8) & 1u);
    return 0;
}
