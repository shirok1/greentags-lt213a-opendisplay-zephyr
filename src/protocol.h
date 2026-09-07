#pragma once
#include "epd.h"
#include <stdbool.h>
#define OD_MAX_FRAME 244

struct od_protocol {
    size_t received;
    size_t expected;
    uint32_t displayed_etag, new_etag, pipe_frames;
    uint16_t pipe_frame;
    uint8_t next_seq;
    bool compressed, zdone, pipe, pipe_failed, partial;
    bool active;
};

struct od_io {
    int (*send)(void *ctx, const uint8_t *data, size_t len);
    int (*begin)(void *ctx);
    int (*write)(void *ctx, const uint8_t *data, size_t len);
    int (*finish)(void *ctx, uint8_t mode);
    void (*abort)(void *ctx);
    void (*reboot)(void *ctx);
    void *ctx;
    const uint8_t *msd;
    int (*region)(void *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
};

void od_reset(struct od_protocol *p);
void od_abort(struct od_protocol *p, const struct od_io *io);
int od_handle(struct od_protocol *p, const struct od_io *io,
              const uint8_t *data, size_t len);
