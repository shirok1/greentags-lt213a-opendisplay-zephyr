#include "config_store.h"
#include <string.h>
#include <errno.h>
static uint8_t flash_bytes[2][1024];
static int cut_after = -1;
static int fake_erase(unsigned slot)
{
    if (cut_after == 0) { return -EIO; }
    if (cut_after > 0) { cut_after--; }
    memset(flash_bytes[slot], 255, 1024); return 0;
}
static int fake_write(unsigned slot, size_t pos, const void *data, size_t len)
{
    if (cut_after == 0) { return -EIO; }
    if (cut_after > 0) { cut_after--; }
    if (pos + len > 1024 || (pos & 3) || (len & 3)) { return -EINVAL; }
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        if ((flash_bytes[slot][pos + i] & p[i]) != p[i]) { return -EIO; }
        flash_bytes[slot][pos + i] &= p[i];
    }
    return 0;
}
static const uint8_t *fake_read(unsigned slot) { return flash_bytes[slot]; }
static const struct od_flash_ops fake_flash = {fake_erase, fake_write, fake_read};
static void fake_init(void) { memset(flash_bytes, 255, sizeof(flash_bytes)); od_config_init(&fake_flash); }
