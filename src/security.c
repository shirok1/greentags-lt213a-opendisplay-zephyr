#include "security.h"
#include "config_store.h"
#include "crypto.h"
#include <string.h>
#include <errno.h>

static void wipe(void *data, size_t len)
{ volatile uint8_t *p = data; while (len--) { *p++ = 0; } }
static bool equal(const uint8_t *a, const uint8_t *b, size_t len)
{ uint8_t diff = 0; while (len--) { diff |= *a++ ^ *b++; } return !diff; }
static uint64_t read64(const uint8_t *p)
{ uint64_t v = 0; for (int i = 0; i < 8; i++) { v = (v << 8) | p[i]; } return v; }
static void put64(uint8_t *p, uint64_t v)
{ for (int i = 7; i >= 0; i--) { p[i] = v; v >>= 8; } }
static bool cmac(const uint8_t *key, const uint8_t *input, size_t len, uint8_t *output)
{
    return od_cmac(key, input, len, output);
}

void od_security_reset(struct od_security *s)
{
    /* Rate limiter survives reconnects and reauthentication. */
    uint32_t time = s->rate_time; uint8_t attempts = s->attempts;
    wipe(s, sizeof(*s)); s->rate_time = time; s->attempts = attempts;
}
bool od_security_enabled(void)
{ const uint8_t *c = od_config_security(); return c && c[0] == 1; }
bool od_security_expire(struct od_security *s, uint32_t now)
{
    const uint8_t *c = od_config_security();
    uint32_t timeout = c ? (c[17] | c[18] << 8) * 1000u : 0;
    if (s->authenticated && timeout && now - s->phase.session.started >= timeout) { od_security_reset(s); return true; }
    return false;
}

/* Keep crypto scratch frames out of main/dispatch when LTO is enabled. */
__attribute__((noinline))
int od_security_auth(struct od_security *s, const uint8_t *p, size_t len,
    uint32_t now, const uint8_t device_id[4], int (*random)(void *, size_t), uint8_t out[23])
{
    out[0] = 0; out[1] = 0x50; out[2] = 255;
    const uint8_t *config = od_config_security();
    if (!od_security_enabled()) { out[2] = 3; return 3; }
    if (now - s->rate_time >= 60000) { s->attempts = 0; s->rate_time = now; }
    if (s->attempts >= 10) { out[2] = 4; return 3; }
    if (len == 1 && p[0] == 0) {
        s->attempts++;
        od_security_reset(s);
        if (random(s->phase.handshake.challenge, 16)) { return 3; }
        s->pending = true; s->phase.handshake.started = now;
        out[2] = 0; memcpy(out + 3, s->phase.handshake.challenge, 16); memcpy(out + 19, device_id, 4); return 23;
    }
    if (len != 32 || !s->pending || now - s->phase.handshake.started >= 30000) { s->pending = false; return 3; }
    s->pending = false; /* each challenge is single-use, including failures */
    uint8_t input[64], proof[16];
    memcpy(input, s->phase.handshake.challenge, 16); memcpy(input + 16, p, 16); memcpy(input + 32, device_id, 4);
    if (!cmac(config + 1, input, 36, proof) || !equal(p + 16, proof, 16)) {
        od_security_reset(s); out[2] = 1; return 3;
    }
    /* Canonical OpenDisplay CMAC KDF followed by AES-ECB. */
    memcpy(input, "OpenDisplay session", 19); input[19] = 0;
    memcpy(input + 20, device_id, 4); memcpy(input + 24, p, 16); memcpy(input + 40, s->phase.handshake.challenge, 16);
    input[56] = 0; input[57] = 0x80;
    if (!cmac(config + 1, input, 58, proof)) { od_security_reset(s); return 3; }
    memset(input, 0, 8); input[7] = 1; memcpy(input + 8, proof, 8);
    if (od_aes_block(config + 1, input, s->key)) { od_security_reset(s); return 3; }
    memcpy(input, p, 16); memcpy(input + 16, s->phase.handshake.challenge, 16);
    if (!cmac(s->key, input, 32, proof)) { od_security_reset(s); return 3; }
    memcpy(s->id, proof, 8);
    memcpy(input, s->phase.handshake.challenge, 16); memcpy(input + 16, p, 16); memcpy(input + 32, device_id, 4);
    if (!cmac(s->key, input, 36, proof)) { od_security_reset(s); return 3; }
    out[2] = 0; memcpy(out + 3, proof, 16);
    wipe(input, sizeof(input)); wipe(proof, sizeof(proof));
    /* Install session state only after the last use of the server challenge.
     * Disjoint TX/RX nonce domains must be initialized after the union wipe. */
    wipe(&s->phase, sizeof(s->phase));
    s->phase.session.tx_counter = UINT64_C(1) << 63;
    s->phase.session.started = now;
    s->authenticated = true;
    return 19;
}

__attribute__((noinline))
int od_security_decrypt(struct od_security *s, uint8_t *frame, size_t len, uint32_t now)
{
    (void)now; /* Only authentication starts the absolute session lifetime. */
    if (!s->authenticated || len < 31 || len > 244 || !equal(frame + 2, s->id, 8)) { return -EACCES; }
    uint64_t counter = read64(frame + 10);
    if (counter >> 63) { return -EACCES; }
    uint64_t behind = s->phase.session.rx_counter - counter;
    if (s->phase.session.rx_seen && counter <= s->phase.session.rx_counter && (behind >= 32 || (s->phase.session.replay & (1u << behind)))) { return -EALREADY; }
    bool ok = od_ccm(s->key, frame + 5, frame, frame + 18, len - 30, true);
    if (!ok || frame[18] != len - 31) { return -EBADMSG; }
    if (!s->phase.session.rx_seen || counter > s->phase.session.rx_counter) {
        uint64_t ahead = counter - s->phase.session.rx_counter;
        s->phase.session.replay = !s->phase.session.rx_seen || ahead >= 32 ? 1 : (s->phase.session.replay << ahead) | 1;
        s->phase.session.rx_counter = counter;
    } else { s->phase.session.replay |= 1u << behind; }
    s->phase.session.rx_seen = true;
    size_t size = frame[18]; memmove(frame + 2, frame + 19, size); return size + 2;
}

/* Keep CCM scratch out of send_response while BLE notification code runs. */
__attribute__((noinline))
int od_security_encrypt(struct od_security *s, const uint8_t *plain, size_t len, uint8_t *out, size_t capacity)
{
    if (!s->authenticated || len < 2 || len > 215 || capacity < len + 29 || s->phase.session.tx_counter == UINT64_MAX) { return -EINVAL; }
    memcpy(out, plain, 2); memcpy(out + 2, s->id, 8); put64(out + 10, s->phase.session.tx_counter++);
    out[18] = len - 2; memcpy(out + 19, plain + 2, len - 2);
    bool ok = od_ccm(s->key, out + 5, out, out + 18, len - 1, false);
    return ok ? (int)len + 29 : -EIO;
}
