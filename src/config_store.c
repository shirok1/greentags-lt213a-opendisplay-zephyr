#include "config_store.h"
#include <string.h>
#include <errno.h>
#include "config.inc"

/* One 1-KiB erase page per slot. Commit word is written LAST. A reset at any
 * earlier point leaves the previous generation readable. No heap or RAM blob. */
#define COMMIT 0x434f444fu
struct header { uint32_t generation; uint32_t length; uint32_t checksum; uint32_t commit; };
static const struct od_flash_ops *flash;
static int active_slot = -1, staging = -1;
static size_t expected, received;
static uint32_t generation;

static uint16_t crc(const uint8_t *data, size_t len, bool outer)
{
    uint16_t c = 0xffff;
    for (size_t i = 0; i < len; i++) {
        c ^= (uint16_t)((outer && i < 2) ? 0 : data[i]) << 8;
        for (int b = 0; b < 8; b++) { c = (c << 1) ^ ((c & 0x8000) ? 0x1021 : 0); }
    }
    return c;
}

int od_config_validate(const uint8_t *data, size_t len)
{
    if (len < 5 || len > OD_CONFIG_MAX || data[2] != 1 ||
        ((data[0] | data[1] << 8) != 0 && (size_t)(data[0] | data[1] << 8) != len) ||
        crc(data, len - 2, true) != (data[len - 2] | data[len - 1] << 8)) { return -EINVAL; }
    unsigned seen = 0;
    for (size_t pos = 3; pos < len - 2;) {
        if (pos + 2 > len - 2 || data[pos] != 0) { return -EINVAL; }
        uint8_t type = data[pos + 1];
        size_t size; unsigned bit;
        switch (type) {
        case 1: size = 22; bit = 1; break;
        case 2: size = 22; bit = 2; break;
        case 4: size = 30; bit = 4; break;
        case 32: size = 46; bit = 8; break;
        case 39: size = 64; bit = 16; break;
        case 44: size = 288; bit = 32; break;
        default: return -ENOTSUP; /* no corresponding physical peripheral */
        }
        if ((seen & bit) || pos + 2 + size > len - 2) { return -EINVAL; }
        seen |= bit;
        const uint8_t *p = data + pos + 2;
        /* This firmware is for fixed wiring, not an arbitrary board programmer. */
        if (type == 1 && (p[0] != 255 || p[1] != 255 || p[2] != 1 || p[3] || p[4] != 255)) { return -ENOTSUP; }
        if (type == 4 && (p[6] != 0 || p[8] != 255 || p[9] != 255 || p[18] || p[19] || p[20] != 255 || p[21] != 255)) { return -ENOTSUP; }
        if (type == 32 && (p[0] || p[1] != 1 || p[2] != 19 || p[3] || p[4] != 104 || p[5] ||
            p[6] != 212 || p[7] || p[14] > 3 || p[15] != 3 || p[16] != 4 || p[17] != 2 ||
            p[18] != 1 || p[19] != 30 || p[20] > 1 || p[21] || (p[22] & ~0x1b) || p[23] || p[24] != 255)) { return -ENOTSUP; }
        if (type == 39) {
            uint8_t key = 0;
            for (int i = 1; i <= 16; i++) { key |= p[i]; }
            if (p[0] > 1 || (p[0] && !key) || p[19]) { return -ENOTSUP; }
        }
        if (type == 44) {
            for (unsigned i = 0; i < 9; i++) { if (!memchr(p + i * 32, 0, 32)) { return -EINVAL; } }
        }
        pos += 2 + size;
    }
    return (seen & 15) == 15 ? 0 : -EINVAL;
}

void od_config_init(const struct od_flash_ops *ops)
{
    flash = ops; active_slot = -1; staging = -1; generation = 0;
    for (unsigned i = 0; i < 2; i++) {
        struct header h;
        const uint8_t *slot = flash->read(i);
        memcpy(&h, slot, sizeof(h));
        if (h.commit != COMMIT || h.length > OD_CONFIG_MAX ||
            crc(slot + sizeof(h), h.length, false) != h.checksum ||
            (h.length && od_config_validate(slot + sizeof(h), h.length))) { continue; }
        if (active_slot < 0 || (int32_t)(h.generation - generation) > 0) {
            active_slot = i; generation = h.generation;
        }
    }
}

const uint8_t *od_config_get(size_t *len)
{
    if (active_slot >= 0) {
        const uint8_t *p = flash->read(active_slot);
        struct header h; memcpy(&h, p, sizeof(h));
        if (h.length) { *len = h.length; return p + sizeof(h); }
    }
    *len = sizeof(od_config); return od_config;
}

static const uint8_t *config_packet(uint8_t type)
{
    size_t len; const uint8_t *p = od_config_get(&len);
    for (size_t pos = 3; pos < len - 2;) {
        uint8_t kind = p[pos + 1];
        if (kind == type) { return p + pos + 2; }
        size_t size = kind == 4 ? 30 : kind == 32 ? 46 : kind == 39 ? 64 : kind == 44 ? 288 : 22;
        pos += size + 2;
    }
    return NULL;
}

const uint8_t *od_config_security(void) { return config_packet(39); }

uint8_t od_config_partial_frames(void)
{
    const uint8_t *extended = config_packet(44);
    if (!extended) { return OD_PARTIAL_FRAMES_DEFAULT; }
    /* DataExtended.custom_string_3; leave unrelated user strings untouched. */
    const char *value = (const char *)extended + 8 * 32;
    const size_t prefix = sizeof(OD_PARTIAL_FRAMES_KEY) - 1;
    if (strncmp(value, OD_PARTIAL_FRAMES_KEY, prefix)) { return OD_PARTIAL_FRAMES_DEFAULT; }
    value += prefix;
    unsigned frames = 0;
    for (; *value; value++) {
        if (*value < '0' || *value > '9') { return OD_PARTIAL_FRAMES_DEFAULT; }
        frames = frames * 10 + (*value - '0');
        if (frames > 255) { return OD_PARTIAL_FRAMES_DEFAULT; }
    }
    return frames ? frames : OD_PARTIAL_FRAMES_DEFAULT;
}

size_t od_config_name(char name[OD_NAME_MAX + 1], uint32_t chip_id)
{
    name[0] = 'O'; name[1] = 'D';
    const uint8_t *extended = config_packet(44);
    const char *serial = extended ? (const char *)extended + 64 : NULL;
    if (serial && serial[0]) {
        size_t len = strlen(serial); /* Validated 32-byte, NUL-terminated field. */
        if (len > OD_NAME_MAX - 2) {
            len = OD_NAME_MAX - 2;
            /* Do not split a UTF-8 character at the advertising size limit. */
            while (len && ((uint8_t)serial[len] & 0xc0) == 0x80) { len--; }
        }
        memcpy(name + 2, serial, len);
        name[len + 2] = 0;
        return len + 2;
    }
    /* Preserve the upstream nRF fallback: DEVICEID[1]'s low 24 bits. */
    for (int i = 7; i >= 2; i--) {
        name[i] = "0123456789ABCDEF"[chip_id & 0xf];
        chip_id >>= 4;
    }
    name[8] = 0;
    return 8;
}

bool od_config_writing(void) { return staging >= 0; }
void od_config_cancel(void) { staging = -1; expected = received = 0; }
int od_config_start(size_t total)
{
    od_config_cancel();
    if (total > OD_CONFIG_MAX || !flash) { return -EINVAL; }
    int target = active_slot == 0 ? 1 : 0;
    int err = flash->erase(target);
    if (err) { return err; }
    staging = target; expected = total; return 0;
}

int od_config_append(const uint8_t *data, size_t len)
{
    if (staging < 0 || len > expected - received || (len == 0 && expected)) {
        od_config_cancel(); return -EINVAL;
    }
    int err = 0;
    /* Preserve previously written bytes of an unaligned flash word. */
    for (size_t i = 0; i < len && !err;) {
        size_t offset = sizeof(struct header) + received + i;
        size_t base = offset & ~(size_t)3, start = offset & 3;
        uint32_t word; memcpy(&word, flash->read(staging) + base, 4);
        size_t count = len - i < 4 - start ? len - i : 4 - start;
        memcpy((uint8_t *)&word + start, data + i, count);
        err = flash->write(staging, base, &word, 4); i += count;
    }
    if (err) { od_config_cancel(); return err; }
    received += len;
    if (received != expected) { return 0; }
    const uint8_t *blob = flash->read(staging) + sizeof(struct header);
    if (expected && od_config_validate(blob, expected)) { od_config_cancel(); return -EINVAL; }
    struct header h = {generation + 1, expected, crc(blob, expected, false), COMMIT};
    err = flash->write(staging, 0, &h, 12);
    if (!err) { err = flash->write(staging, 12, &h.commit, 4); }
    if (!err) { active_slot = staging; generation = h.generation; }
    od_config_cancel(); return err;
}

int od_config_clear(void)
{
    int err = od_config_start(0);
    return err ? err : od_config_append(NULL, 0);
}
