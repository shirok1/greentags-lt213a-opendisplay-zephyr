"""Exercise the production sampler's encoding, atomic snapshot and ADC shutdown."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
headers = {
    "zephyr/drivers/sensor.h": """
#include <stdint.h>
struct device { int unused; };
struct sensor_value { int32_t val1, val2; };
extern struct device fake_sensor;
#define DEVICE_DT_GET_ONE(x) (&fake_sensor)
#define SENSOR_CHAN_DIE_TEMP 0
int device_is_ready(const struct device *dev);
int sensor_sample_fetch(const struct device *dev);
int sensor_channel_get(const struct device *dev, int channel, struct sensor_value *value);
""",
    "zephyr/kernel.h": """
#include <stdbool.h>
#include <stdint.h>
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
int64_t k_uptime_get(void);
void k_msleep(int ms);
""",
    "hal/nrf_adc.h": """
#include <stdint.h>
struct adc { uint32_t ENABLE, CONFIG, EVENTS_END, TASKS_START, TASKS_STOP, RESULT; };
extern struct adc adc;
#define NRF_ADC (&adc)
#define ADC_CONFIG_RES_10bit 2
#define ADC_CONFIG_RES_Pos 0
#define ADC_CONFIG_INPSEL_SupplyOneThirdPrescaling 3
#define ADC_CONFIG_INPSEL_Pos 2
""",
}
with tempfile.TemporaryDirectory(prefix="lt213a-telemetry-") as directory:
    temporary = Path(directory)
    for name, content in headers.items():
        path = temporary / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    binary = temporary / "test-telemetry"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-Isrc", f"-I{temporary}", "src/telemetry.c",
        "tests/test_telemetry.c", "-o", str(binary)], cwd=root, check=True)
    subprocess.run([str(binary)], check=True)
