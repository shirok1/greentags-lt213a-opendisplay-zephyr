#include "protocol.h"
#include <string.h>
#include <errno.h>
#include "config_store.h"
#include "uzlib.h"

static uint32_t le(const uint8_t *p, unsigned n)
{ uint32_t v = 0; for (unsigned i = 0; i < n; i++) { v |= (uint32_t)p[i] << (8 * i); } return v; }
static uint32_t be(const uint8_t *p, unsigned n)
{ uint32_t v = 0; for (unsigned i = 0; i < n; i++) { v = (v << 8) | p[i]; } return v; }

static int error_code(const struct od_io *io, uint8_t cmd, uint8_t code)
{ const uint8_t r[] = {255, cmd, code, 0}; return io->send(io->ctx, r, sizeof(r)); }

static int reply(const struct od_io *io, uint8_t cmd, bool ok)
{
    const uint8_t response[] = { ok ? 0x00 : 0xff, cmd };
    return io->send(io->ctx, response, sizeof(response));
}

void od_reset(struct od_protocol *p)
{
    p->active = false;
    p->received = 0;
    p->expected = EPD_FRAME_BYTES;
    p->compressed = p->zdone = p->pipe = p->pipe_failed = p->partial = false;
    p->new_etag = p->pipe_frames = p->next_seq = 0;
}

void od_abort(struct od_protocol *p, const struct od_io *io)
{
    if (p->active) { io->abort(io->ctx); }
    if (p->active) { p->displayed_etag = 0; }
    od_reset(p);
}

static int stream(struct od_protocol *p, const struct od_io *io,
                  const uint8_t *data, size_t len, bool final)
{
    if (!p->compressed) {
        if (len > p->expected - p->received) { return -EFBIG; }
        int err = len ? io->write(io->ctx, data, len) : 0;
        if (!err) { p->received += len; }
        return err;
    }
    if (od_zlib_stream_push(data, len, final) == OD_ZLIB_STATUS_ERROR) { return -EBADMSG; }
    for (;;) {
        uint8_t out[32]; size_t count;
        od_zlib_status_t status = od_zlib_stream_poll(out, sizeof(out), &count);
        if (status == OD_ZLIB_STATUS_ERROR || count > p->expected - p->received) { return -EBADMSG; }
        int err = count ? io->write(io->ctx, out, count) : 0;
        if (err) { return err; }
        p->received += count;
        if (status == OD_ZLIB_STATUS_DONE) { p->zdone = true; return 0; }
        if (status == OD_ZLIB_STATUS_NEEDS_INPUT) { return final ? -EBADMSG : 0; }
    }
}

static int sack(struct od_protocol *p, const struct od_io *io, uint8_t error)
{
    uint8_t r[8] = {error ? 255 : 0, 0x81};
    unsigned at = 2;
    if (error) { r[at++] = error; }
    r[at++] = p->next_seq - 1;
    unsigned previous = p->pipe_frames ? p->pipe_frames - 1 : 0;
    uint32_t mask = previous >= 32 ? UINT32_MAX : (1u << previous) - 1;
    for (unsigned i = 0; i < 4; i++) { r[at++] = mask >> (i * 8); }
    return io->send(io->ctx, r, at);
}

static int start_region(struct od_protocol *p, const struct od_io *io,
                        uint32_t old, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (!old || old != p->displayed_etag) { return 1; }
    if (!w || !h || x >= EPD_WIDTH || y >= EPD_HEIGHT || w > EPD_WIDTH - x || h > EPD_HEIGHT - y) { return 3; }
    if ((x | w) & 7) { return 4; }
    if (!io->region) { return 7; }
    if (io->region(io->ctx, x, y, w, h)) { return 6; }
    p->partial = true; p->active = true; p->expected = (w / 8) * h * 2;
    return 0;
}

int od_handle(struct od_protocol *p, const struct od_io *io,
              const uint8_t *data, size_t len)
{
    if (len < 2 || len > OD_MAX_FRAME) { return -EINVAL; }
    uint8_t cmd = data[1];
    if (data[0] != 0) { return reply(io, cmd, false); }
    const uint8_t *payload = data + 2;
    size_t size = len - 2;
    switch (cmd) {
    case 0x40: {
        if (size) { return reply(io, cmd, false); }
        size_t config_len;
        const uint8_t *config = od_config_get(&config_len);
        size_t offset = 0;
        uint16_t chunk = 0;
        while (offset < config_len) {
            uint8_t response[20] = {0, cmd, chunk & 0xff, chunk >> 8};
            size_t header = chunk ? 4 : 6;
            if (!chunk) {
                response[4] = config_len & 0xff;
                response[5] = config_len >> 8;
            }
            size_t count = config_len - offset;
            if (count > sizeof(response) - header) { count = sizeof(response) - header; }
            memcpy(response + header, config + offset, count);
            int err = io->send(io->ctx, response, header + count);
            if (err) { return err; }
            offset += count;
            chunk++;
        }
        return 0;
    }
    case 0x41: {
        od_abort(p, io);
        od_config_cancel();
        if (size == 202) {
            size_t total = le(payload, 2);
            if (total <= 200 || od_config_start(total)) { return reply(io, cmd, false); }
            return reply(io, cmd, !od_config_append(payload + 2, 200));
        }
        if (size < 5 || size > 200 || od_config_start(size)) { return reply(io, cmd, false); }
        return reply(io, cmd, !od_config_append(payload, size));
    }
    case 0x42:
        if (!size || size > 200 || !od_config_writing()) { od_config_cancel(); return reply(io, cmd, false); }
        return reply(io, cmd, !od_config_append(payload, size));
    case 0x45:
        if (size) { return reply(io, cmd, false); }
        od_abort(p, io); return reply(io, cmd, !od_config_clear());
    case 0x43: {
        if (size) { return reply(io, cmd, false); }
        const uint8_t response[] = {0, 0x43, 0, 2, 0, 0}; /* 0.2.0, no SHA */
        return io->send(io->ctx, response, sizeof(response));
    }
    case 0x44: {
        if (size) { return reply(io, cmd, false); }
        uint8_t response[18] = {0, 0x44};
        memcpy(response + 2, io->msd, 16);
        return io->send(io->ctx, response, sizeof(response));
    }
    case 0x50: {
        const uint8_t response[] = {0, 0x50, 3}; /* AUTH_STATUS_NOT_CONFIG */
        return io->send(io->ctx, response, sizeof(response));
    }
    case 0x70:
        od_abort(p, io);
        od_config_cancel();
        if (size && (size < 4 || size > 200 || le(payload, 4) != EPD_FRAME_BYTES)) { return reply(io, cmd, false); }
        if (io->begin(io->ctx)) { return reply(io, cmd, false); }
        p->active = true;
        p->compressed = size != 0;
        if (p->compressed) {
            od_zlib_stream_reset(p->expected);
            if (stream(p, io, payload + 4, size - 4, false)) { od_abort(p, io); return reply(io, cmd, false); }
        }
        return reply(io, cmd, true);
    case 0x71:
        if (!p->active || p->pipe || !size || size > 230) {
            od_abort(p, io);
            return reply(io, cmd, false);
        }
        if (stream(p, io, payload, size, false)) {
            od_abort(p, io);
            return reply(io, cmd, false);
        }
        return reply(io, cmd, true);
    case 0x72:
    case 0x82: {
        bool complete = p->active && p->pipe == (cmd == 0x82) &&
            (((size == 1 || size == 5) && payload[0] <= (p->partial ? 2 : 1)) || (!size && p->partial && p->pipe)) &&
            !stream(p, io, NULL, 0, true) && p->received == p->expected && (!p->compressed || p->zdone);
        if (!complete) { od_abort(p, io); return reply(io, cmd, false); }
        if (p->pipe) { int err = sack(p, io, 0); if (err) { od_abort(p, io); return err; } }
        int err = reply(io, cmd, true);
        if (err) { od_abort(p, io); return err; }
        err = io->finish(io->ctx, size ? payload[0] : 2);
        p->displayed_etag = err ? 0 : size == 5 ? be(payload + 1, 4) : p->new_etag;
        od_reset(p);
        return reply(io, err ? 0x74 : 0x73, true);
    }
    case 0x0f:
        if (size) { return reply(io, cmd, false); }
        od_abort(p, io);
        io->reboot(io->ctx);
        return 0;
    case 0x52:
    case 0x53: {
        const uint8_t response[] = {0xff, cmd, 0, 0};
        return io->send(io->ctx, response, sizeof(response));
    }
    case 0x76: {
        od_abort(p, io); od_config_cancel();
        if (size < 17) { return error_code(io, cmd, 6); }
        if (payload[0] & ~1) { return error_code(io, cmd, 5); }
        uint32_t next = be(payload + 5, 4);
        int code = next ? start_region(p, io, be(payload + 1, 4), be(payload + 9, 2),
            be(payload + 11, 2), be(payload + 13, 2), be(payload + 15, 2)) : 1;
        if (code) { p->displayed_etag = 0; return error_code(io, cmd, code); }
        p->new_etag = next; p->compressed = payload[0] & 1;
        if (p->compressed) { od_zlib_stream_reset(p->expected); }
        if (stream(p, io, payload + 17, size - 17, false)) { od_abort(p, io); return error_code(io, cmd, 6); }
        return reply(io, cmd, true);
    }
    case 0x80: {
        od_abort(p, io); od_config_cancel();
        if (size < 10 || payload[0] != 1 || !payload[2] || !payload[3] || le(payload + 4, 2) < 4) { return error_code(io, cmd, 1); }
        if (payload[1] & ~3) { return error_code(io, cmd, 2); }
        bool partial = payload[1] & 2;
        if (size != (partial ? 22 : 10)) { return error_code(io, cmd, 1); }
        if (partial) {
            uint16_t w = le(payload + 18, 2), h = le(payload + 20, 2);
            if (le(payload + 6, 4) != (uint32_t)(w / 8) * h * 2) { return error_code(io, cmd, 3); }
            int code = start_region(p, io, le(payload + 10, 4), le(payload + 14, 2), le(payload + 16, 2), w, h);
            if (code) { p->displayed_etag = 0; return error_code(io, cmd, code == 1 ? 5 : code == 7 ? 6 : 7); }
        } else {
            if (le(payload + 6, 4) != EPD_FRAME_BYTES) { return error_code(io, cmd, 3); }
            if (io->begin(io->ctx)) { return error_code(io, cmd, 3); }
            p->active = true;
        }
        p->pipe = true; p->compressed = payload[1] & 1;
        if (p->compressed) { od_zlib_stream_reset(p->expected); }
        p->pipe_frame = le(payload + 4, 2);
        if (p->pipe_frame > OD_MAX_FRAME) { p->pipe_frame = OD_MAX_FRAME; }
        /* Negotiated W=N=1 bounds RAM. Selective ACKs still cover retries. */
        const uint8_t r[] = {0, 0x80, 1, 1, 1, p->pipe_frame & 255, p->pipe_frame >> 8, partial ? 3 : 1};
        return io->send(io->ctx, r, sizeof(r));
    }
    case 0x81: {
        if (p->pipe_failed) { return 0; }
        int error = 4;
        if (p->active && p->pipe && size >= 2 && len <= p->pipe_frame) {
            uint8_t behind = p->next_seq - payload[0];
            if (payload[0] != p->next_seq && behind <= 33 && behind && behind <= p->pipe_frames) { return sack(p, io, 0); }
            if (payload[0] == p->next_seq) {
                int err = stream(p, io, payload + 1, size - 1, false);
                if (!err) { p->next_seq++; p->pipe_frames++; return sack(p, io, 0); }
                error = p->compressed ? 2 : 3;
            }
        }
        int err = sack(p, io, error);
        od_abort(p, io); p->pipe_failed = true; return err;
    }
    case 0x83: {
        const uint8_t r[] = {255, 0x83, 255, size && payload[0] != 0 ? 3 : 2};
        return io->send(io->ctx, r, sizeof(r));
    }
    default:
        return reply(io, cmd, false);
    }
}
