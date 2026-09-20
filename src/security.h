#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct od_security {
    uint8_t key[16], id[8];
    /* pending owns handshake scratch; authenticated owns the session state.
     * Key/id remain separate: the final proof still needs the challenge. */
    union {
        struct { uint8_t challenge[16]; uint32_t started; } handshake;
        struct {
            uint64_t rx_counter, tx_counter;
            uint32_t replay, started;
            bool rx_seen;
        } session;
    } phase;
    uint32_t rate_time;
    uint8_t attempts;
    bool authenticated, pending;
};
void od_security_reset(struct od_security *s);
bool od_security_enabled(void);
bool od_security_expire(struct od_security *s, uint32_t now);
int od_security_auth(struct od_security *s, const uint8_t *payload, size_t len,
    uint32_t now, const uint8_t device_id[4], int (*random)(void *, size_t), uint8_t response[23]);
int od_security_decrypt(struct od_security *s, uint8_t *frame, size_t len, uint32_t now);
int od_security_encrypt(struct od_security *s, const uint8_t *plain, size_t len, uint8_t *out, size_t capacity);
