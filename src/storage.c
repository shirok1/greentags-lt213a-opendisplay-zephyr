#include "config_store.h"
#include <zephyr/drivers/flash.h>
#include <zephyr/devicetree.h>

#define BASE DT_REG_ADDR(DT_NODELABEL(storage_partition))
static const struct device *const flash_device = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));
static int erase_slot(unsigned slot) { return flash_erase(flash_device, BASE + slot * 1024, 1024); }
static int write_slot(unsigned slot, size_t offset, const void *data, size_t len)
{ return flash_write(flash_device, BASE + slot * 1024 + offset, data, len); }
static const uint8_t *read_slot(unsigned slot) { return (const uint8_t *)(BASE + slot * 1024); }
int storage_init(void)
{
    static const struct od_flash_ops ops = {erase_slot, write_slot, read_slot};
    if (!device_is_ready(flash_device)) { return -ENODEV; }
    od_config_init(&ops); return 0;
}
