#include "protocol.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "fake_flash.h"

static struct od_protocol p;
static uint8_t panel[EPD_FRAME_BYTES * 2], response[64][20], sizes[64];
static size_t panel_expected = EPD_FRAME_BYTES;
static size_t written;
static int responses, begins, finishes, aborts, reboots, fail_send, fail_begin, fail_write, fail_finish;
static const uint8_t msd[16] = {0x46, 0x24};

static int send(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (fail_send) { return -ENOTCONN; }
    assert(responses < 64 && len <= 20);
    memcpy(response[responses], data, len);
    sizes[responses++] = len;
    return 0;
}
static int begin(void *ctx) { (void)ctx; begins++; written = 0; panel_expected = EPD_FRAME_BYTES; return fail_begin; }
static int write_data(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (fail_write) { return -EIO; }
    assert(written + len <= sizeof(panel));
    memcpy(panel + written, data, len);
    written += len;
    return 0;
}
static int finish(void *ctx, uint8_t mode) { (void)ctx; (void)mode; finishes++; assert(written == panel_expected); return fail_finish; }
static int region(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{ (void)ctx; assert(x + w <= 104 && y + h <= 212); written = 0; panel_expected = (w / 8) * h * 2; return 0; }
static void abort_panel(void *ctx) { (void)ctx; aborts++; }
static void reboot(void *ctx) { (void)ctx; reboots++; }
static int samples, fail_sample;
static int sample_msd(void *ctx) { (void)ctx; samples++; return fail_sample; }
static const struct od_io io = {send, begin, write_data, finish, abort_panel, reboot, NULL, msd, sample_msd, region};

static void command(uint8_t cmd, const uint8_t *payload, size_t size, bool ok)
{
    uint8_t data[OD_MAX_FRAME] = {0, cmd};
    assert(size <= OD_MAX_FRAME - 2);
    if (size) { memcpy(data + 2, payload, size); }
    responses = 0;
    assert(od_handle(&p, &io, data, size + 2) == 0);
    assert(responses > 0);
    assert(response[0][0] == (ok ? 0 : 255) && response[0][1] == cmd);
}

static void upload(size_t chunk)
{
    command(0x70, NULL, 0, true);
    for (size_t pos = 0; pos < EPD_FRAME_BYTES;) {
        uint8_t bytes[230];
        size_t count = EPD_FRAME_BYTES - pos;
        if (count > chunk) { count = chunk; }
        for (size_t i = 0; i < count; i++) { bytes[i] = (uint8_t)(pos + i); }
        command(0x71, bytes, count, true);
        pos += count;
    }
}

static size_t load(const char *name, uint8_t *data, size_t max)
{ FILE *f = fopen(name, "rb"); assert(f); size_t n = fread(data, 1, max, f); assert(feof(f)); fclose(f); return n; }

static void compressed_tests(void)
{
    const char *names[] = {"build/dynamic.zlib", "build/fixed.zlib", "build/stored.zlib"};
    const size_t chunks[] = {1, 2, 17, 230};
    uint8_t zipped[4096], raw[2757], header[] = {0xc4, 0x0a, 0, 0}, end[] = {0};
    assert(load("build/fixture.raw", raw, sizeof(raw)) == 2756);
    for (size_t file = 0; file < 3; file++) {
        size_t len = load(names[file], zipped, sizeof(zipped));
        for (size_t block = 0; block < 4; block++) {
            command(0x70, header, sizeof(header), true);
            for (size_t pos = 0; pos < len;) {
                size_t count = len - pos < chunks[block] ? len - pos : chunks[block];
                command(0x71, zipped + pos, count, true); pos += count;
            }
            command(0x72, end, 1, true);
            assert(!memcmp(panel, raw, 2756));
        }
        command(0x70, header, sizeof(header), true);
        for (size_t pos = 0; pos < len - 1;) {
            size_t count = len - 1 - pos < 230 ? len - 1 - pos : 230;
            command(0x71, zipped + pos, count, true); pos += count;
        }
        command(0x72, end, 1, false); /* missing Adler trailer byte */
    }
    size_t len = load(names[0], zipped, sizeof(zipped));
    zipped[len - 1] ^= 1;
    command(0x70, header, sizeof(header), true);
    for (size_t pos = 0; pos < len;) {
        size_t count = len - pos < 230 ? len - pos : 230;
        command(0x71, zipped + pos, count, pos + count != len); pos += count;
    }
    assert(!p.active);
}

static void pipe_tests(void)
{
    uint8_t start[] = {1, 0, 32, 32, 244, 0, 0xc4, 0x0a, 0, 0};
    command(0x80, start, sizeof(start), true);
    assert(response[0][3] == 1 && response[0][4] == 1 && response[0][7] == 1);
    uint32_t count = 0;
    for (size_t pos = 0; pos < EPD_FRAME_BYTES;) {
        uint8_t frame[8]; size_t take = EPD_FRAME_BYTES - pos < 7 ? EPD_FRAME_BYTES - pos : 7;
        frame[0] = count;
        for (size_t i = 0; i < take; i++) { frame[i + 1] = (uint8_t)(pos + i); }
        command(0x81, frame, take + 1, true);
        assert(response[0][2] == (uint8_t)count);
        uint32_t mask = response[0][3] | (uint32_t)response[0][4] << 8 | (uint32_t)response[0][5] << 16 | (uint32_t)response[0][6] << 24;
        assert(mask == (count >= 32 ? UINT32_MAX : (1u << count) - 1));
        size_t already = written;
        if (pos + take < EPD_FRAME_BYTES) {
            assert(responses == 1 && p.active);
            command(0x81, frame, take + 1, true); /* retransmit: no duplicate write */
            assert(written == already);
        } else {
            assert(responses == 3 && response[1][1] == 0x82 && response[2][1] == 0x73);
            assert(!p.active && p.displayed_etag == 0);
        }
        pos += take; count++;
    }
    assert(count > 256);
    /* A late retry after auto-END is harmless, even after the 8-bit wrap. */
    uint8_t late_data[] = {0, 0x81, (uint8_t)(count - 1), 0xff};
    int auto_finishes = finishes;
    responses = 0;
    assert(!od_handle(&p, &io, late_data, sizeof(late_data)));
    assert(!responses && finishes == auto_finishes && !p.active);
    uint8_t end[] = {0, 1, 2, 3, 4}, packet[] = {0, 0x82, 0, 1, 2, 3, 4};
    int finished = finishes;
    responses = 0; assert(!od_handle(&p, &io, packet, sizeof(packet)));
    assert(responses == 1 && response[0][0] == 255 && finishes == finished);
    /* Establish an etag through explicit direct-write END for partial tests. */
    upload(230);
    command(0x72, end, sizeof(end), true);
    assert(p.displayed_etag == 0x01020304);
    uint8_t partial[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 8, 0, 2, 0, 16, 0, 16};
    command(0x76, partial, sizeof(partial), true);
    uint8_t pixels[64]; for (size_t i = 0; i < sizeof(pixels); i++) { pixels[i] = i; }
    command(0x71, pixels, sizeof(pixels), true);
    end[0] = 1; command(0x72, end, 1, true);
    assert(p.displayed_etag == 0x05060708 && !memcmp(pixels, panel, sizeof(pixels)));
    command(0x76, partial, sizeof(partial), false); /* stale etag */
    assert(response[0][2] == 1);
    command(0x80, start, sizeof(start), true);
    uint8_t gap[] = {1, 0}; command(0x81, gap, sizeof(gap), false);
    assert(response[0][2] == 4 && !p.active && p.pipe_failed);
    uint8_t ignored[] = {0, 0x81, 0, 0}; responses = 0;
    assert(!od_handle(&p, &io, ignored, sizeof(ignored)) && !responses);
    command(0x80, start, sizeof(start), true);
    packet[2] = 0; responses = 0; assert(!od_handle(&p, &io, packet, 3));
    assert(responses == 2 && response[0][0] == 0 && response[0][1] == 0x81);
    assert(response[0][2] == 255 && response[0][3] == 0);
    assert(response[1][0] == 255 && response[1][1] == 0x82);

    /* Exercise compression inside PIPE, including SACK before END ACK. */
    uint8_t zipped[4096], raw[2757];
    size_t zipped_len = load("build/dynamic.zlib", zipped, sizeof(zipped));
    assert(load("build/fixture.raw", raw, sizeof(raw)) == EPD_FRAME_BYTES);
    start[1] = 1;
    command(0x80, start, sizeof(start), true);
    for (size_t pos = 0, seq = 0; pos < zipped_len; seq++) {
        uint8_t frame[18]; size_t take = zipped_len - pos < 17 ? zipped_len - pos : 17;
        frame[0] = seq; memcpy(frame + 1, zipped + pos, take);
        command(0x81, frame, take + 1, true); pos += take;
    }
    responses = 0; assert(!od_handle(&p, &io, packet, sizeof(packet)));
    assert(responses == 3 && response[1][0] == 0 && response[2][1] == 0x73);
    assert(!memcmp(panel, raw, EPD_FRAME_BYTES));

    /* PIPE partial extension uses little-endian fields; END may be empty. */
    uint8_t region_start[] = {1, 2, 1, 1, 244, 0, 64, 0, 0, 0,
        4, 3, 2, 1, 8, 0, 2, 0, 16, 0, 16, 0};
    command(0x80, region_start, sizeof(region_start), true);
    uint8_t region_frame[65] = {0}; memcpy(region_frame + 1, pixels, 64);
    command(0x81, region_frame, sizeof(region_frame), true);
    responses = 0; assert(!od_handle(&p, &io, packet, 2));
    assert(responses == 3 && response[1][0] == 0 && response[2][1] == 0x73);
    assert(!memcmp(panel, pixels, 64));

    /* Stray PIPE DATA must not cancel an unrelated direct-write session. */
    command(0x70, NULL, 0, true);
    int prior_aborts = aborts;
    responses = 0;
    assert(!od_handle(&p, &io, late_data, sizeof(late_data)));
    assert(!responses && p.active && !p.pipe && aborts == prior_aborts);
    od_abort(&p, &io);

    /* Truncated compressed END still flushes the SACK before its NACK. */
    start[1] = 1;
    command(0x80, start, sizeof(start), true);
    uint8_t truncated[] = {0, 0x18, 0x95};
    command(0x81, truncated, sizeof(truncated), true);
    responses = 0;
    assert(!od_handle(&p, &io, packet, 3));
    assert(responses == 2 && response[0][1] == 0x81 && response[0][2] == 0);
    assert(response[1][0] == 255 && response[1][1] == 0x82 && !p.active);
}

int main(void)
{
    fake_init();
    uint8_t zero[242] = {0}, fast = 3;
    command(0x71, zero, 1, false);
    command(0x72, zero, 1, false);
    assert(!finishes);
    for (size_t chunk = 18; chunk <= 230; chunk += 212) {
        upload(chunk);
        command(0x72, zero, 1, true);
        assert(responses == 2 && response[1][1] == 0x73 && !p.active);
        for (size_t i = 0; i < EPD_FRAME_BYTES; i++) { assert(panel[i] == (uint8_t)i); }
    }
    assert(finishes == 2);
    command(0x70, zero, 4, false); /* compressed rejected */
    command(0x70, NULL, 0, true);
    command(0x71, zero, 231, false); /* max protocol chunk */
    assert(!p.active);
    command(0x70, NULL, 0, true);
    command(0x71, zero, 0, false);
    command(0x70, NULL, 0, true);
    command(0x71, zero, 18, true);
    command(0x72, zero, 1, false); /* truncated image */
    upload(230);
    command(0x71, zero, 1, false); /* one-byte overflow */
    command(0x72, zero, 1, false);
    upload(230);
    command(0x72, &fast, 1, false);
    assert(finishes == 2);
    command(0x70, NULL, 0, true);
    int before = aborts;
    command(0x70, NULL, 0, true); /* restart */
    assert(aborts == before + 1 && p.received == 0);
    od_abort(&p, &io); /* disconnect/transfer watchdog path */
    command(0x71, zero, 1, false);
    command(0x72, zero, 1, false);
    fail_begin = -ETIMEDOUT;
    command(0x70, NULL, 0, false);
    fail_begin = 0;
    command(0x70, NULL, 0, true);
    fail_write = 1;
    command(0x71, zero, 18, false);
    fail_write = 0;
    assert(!p.active);
    upload(230);
    fail_finish = -ETIMEDOUT;
    command(0x72, zero, 5, true);
    assert(response[1][1] == 0x74 && !p.active);
    fail_finish = 0;
    upload(230);
    int previous_finishes = finishes;
    fail_send = 1;
    uint8_t end[] = {0, 0x72, 0};
    assert(od_handle(&p, &io, end, sizeof(end)) == -ENOTCONN);
    assert(finishes == previous_finishes && !p.active);
    fail_send = 0;
    command(0x40, NULL, 0, true);
    uint8_t config[160];
    size_t total = 0;
    for (int i = 0; i < responses; i++) {
        assert(response[i][2] == i && response[i][3] == 0);
        size_t header = i ? 4 : 6;
        memcpy(config + total, response[i] + header, sizes[i] - header);
        total += sizes[i] - header;
    }
    assert(total == 133 && response[0][4] == total && config[2] == 1);
    FILE *out = fopen("build/test-config.bin", "wb");
    assert(out && fwrite(config, 1, total, out) == total);
    fclose(out);
    command(0x43, NULL, 0, true);
    assert(response[0][2] == 0 && response[0][3] == 5 && response[0][4] > 0);
    assert(sizes[0] == 6 + response[0][4] && response[0][sizes[0] - 1] == 0);
    out = fopen("build/test-version.bin", "wb");
    assert(out && fwrite(response[0], 1, sizes[0], out) == sizes[0]);
    fclose(out);
    command(0x44, NULL, 0, true);
    assert(sizes[0] == 18 && !memcmp(response[0] + 2, msd, 16));
    assert(samples == 1);
    command(0x44, zero, 1, false);
    assert(samples == 1); /* Malformed requests must not sample. */
    fail_sample = 1;
    command(0x44, NULL, 0, false);
    assert(samples == 2 && sizes[0] == 2);
    fail_sample = 0;
    command(0x50, NULL, 0, true);
    assert(response[0][2] == 3);
    command(0x41, zero, 1, false);
    command(0x76, zero, 1, false);
    assert(response[0][2] == 6);
    command(0x80, zero, 10, false);
    command(0x0f, zero, 1, false);
    assert(!reboots);
    uint8_t reset[] = {0, 0x0f};
    assert(!od_handle(&p, &io, reset, sizeof(reset)) && reboots == 1);
    compressed_tests(); pipe_tests();
    uint8_t extended[600], config_first[202];
    size_t extended_len = load("build/secure-config.bin", extended, sizeof(extended));
    config_first[0] = extended_len; config_first[1] = extended_len >> 8;
    memcpy(config_first + 2, extended, 200);
    command(0x41, config_first, sizeof(config_first), true);
    assert(od_config_writing());
    for (size_t pos = 200; pos < extended_len;) {
        size_t n = extended_len - pos < 200 ? extended_len - pos : 200;
        command(0x42, extended + pos, n, true); pos += n;
    }
    assert(!od_config_writing());
    size_t stored; assert(!memcmp(od_config_get(&stored), extended, extended_len) && stored == extended_len);
    command(0x45, NULL, 0, true);
    /* Deterministic malformed-packet exercise under ASan + UBSan. */
    uint32_t seed = 1;
    for (int trial = 0; trial < 20000; trial++) {
        uint8_t bytes[OD_MAX_FRAME + 1];
        for (size_t i = 0; i < sizeof(bytes); i++) {
            seed = seed * 1664525u + 1013904223u;
            bytes[i] = seed >> 24;
        }
        if (trial % 2) { bytes[0] = 0; }
        responses = 0;
        od_handle(&p, &io, bytes, seed % (sizeof(bytes) + 1));
        assert(p.received <= EPD_FRAME_BYTES);
    }
    puts("Protocol tests passed (including 20,000 malformed packets)");
    return 0;
}
