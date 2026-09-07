#include "fake_flash.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    fake_init(); size_t base_len; const uint8_t *base = od_config_get(&base_len);
    uint8_t original[200]; memcpy(original, base, base_len);
    assert(!od_config_validate(base, base_len));
    uint8_t replacement[600]; FILE *f = fopen("build/secure-config.bin", "rb"); assert(f);
    size_t len = fread(replacement, 1, sizeof(replacement), f); fclose(f);
    assert(!od_config_validate(replacement, len));
    assert(!od_config_start(base_len) && !od_config_append(original, base_len));
    uint8_t baseline[2][1024]; memcpy(baseline, flash_bytes, sizeof(baseline));
    for (int point = 0; point < 220; point++) {
        memcpy(flash_bytes, baseline, sizeof(baseline)); od_config_init(&fake_flash);
        cut_after = point;
        if (!od_config_start(len)) {
            /* Deliberately unaligned chunks exercise flash word preservation. */
            for (size_t pos = 0; pos < len;) {
                size_t n = len - pos < 7 ? len - pos : 7;
                int err = od_config_append(replacement + pos, n); pos += n;
                if (err) { break; }
            }
        }
        cut_after = -1; od_config_init(&fake_flash);
        size_t size; const uint8_t *result = od_config_get(&size);
        assert((size == base_len && !memcmp(result, original, size)) ||
               (size == len && !memcmp(result, replacement, size)));
    }
    assert(!od_config_start(len) && !od_config_append(replacement, len));
    assert(!od_config_start(10) && od_config_append(replacement, 11));
    assert(!od_config_writing());
    od_config_init(&fake_flash);
    assert(od_config_security() && od_config_security()[0] == 1);
    assert(!od_config_clear()); od_config_init(&fake_flash);
    size_t size; assert(!memcmp(od_config_get(&size), original, base_len) && size == base_len);
    replacement[len - 1] ^= 1;
    assert(!od_config_start(len) && od_config_append(replacement, len));
    od_config_init(&fake_flash); assert(!od_config_security());
    puts("Flash transactions passed: CRC rejection, aligned/unaligned chunks, reboot and 220 power cuts");
}
