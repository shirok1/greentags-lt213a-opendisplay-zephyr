"""Compile the real EPD driver against a virtual GPIO/clock, in both power modes."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
headers = {
    "zephyr/devicetree.h": """
#define DT_PATH(x) 0
#define DT_GPIO_PIN(node, prop) PIN_##prop
#define DT_GPIO_FLAGS(node, prop) FLAGS_##prop
#define PIN_mosi_gpios 30
#define PIN_sck_gpios 0
#define PIN_cs_gpios 1
#define PIN_dc_gpios 2
#define PIN_reset_gpios 3
#define PIN_busy_gpios 4
#define PIN_bs_gpios 5
#define FLAGS_mosi_gpios 0
#define FLAGS_sck_gpios 0
#define FLAGS_cs_gpios 1
#define FLAGS_dc_gpios 0
#define FLAGS_reset_gpios 1
#define FLAGS_busy_gpios 1
#define FLAGS_bs_gpios 0
""",
    "zephyr/dt-bindings/gpio/gpio.h": "#define GPIO_ACTIVE_LOW 1\n",
    "zephyr/kernel.h": """
#include <stdbool.h>
#include <stdint.h>
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define IS_ENABLED(x) (x)
int64_t k_uptime_get(void);
void k_msleep(int ms);
void k_busy_wait(unsigned us);
""",
    "hal/nrf_gpio.h": """
#include <stdbool.h>
#define NRF_GPIO_PIN_NOPULL 0
void nrf_gpio_pin_write(unsigned pin, unsigned value);
void nrf_gpio_cfg_output(unsigned pin);
void nrf_gpio_cfg_input(unsigned pin, unsigned pull);
void nrf_gpio_cfg_default(unsigned pin);
unsigned nrf_gpio_pin_read(unsigned pin);
""",
}
with tempfile.TemporaryDirectory(prefix="lt213a-epd-") as directory:
    temporary = Path(directory)
    for name, content in headers.items():
        path = temporary / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    for enabled in (0, 1):
        binary = temporary / f"test-epd-{enabled}"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", f"-DCONFIG_LT213A_EPD_DEEP_SLEEP={enabled}",
            "-Isrc", f"-I{temporary}", "src/epd.c", "tests/test_epd.c", "-o", str(binary)],
            cwd=root, check=True)
        subprocess.run([str(binary)], check=True)
