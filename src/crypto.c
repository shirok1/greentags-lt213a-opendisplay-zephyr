/* SPDX-License-Identifier: GPL-3.0-only
 * AES-CMAC (RFC 4493) and the fixed OpenDisplay AES-CCM profile.
 * AES-block operations are provided by the device or host-test adapter.
 */
#include "crypto.h"
#include <string.h>
static void wipe(void *v, size_t n) { volatile uint8_t *p = v; while (n--) *p++ = 0; }
static void double_block(uint8_t b[16])
{
    uint8_t carry = b[0] >> 7;
    for (unsigned i = 0; i < 15; i++) b[i] = (uint8_t)((b[i] << 1) | (b[i + 1] >> 7));
    b[15] = (uint8_t)((b[15] << 1) ^ (0x87 & (0u - carry)));
}
bool od_cmac(const uint8_t key[16], const uint8_t *in, size_t len, uint8_t out[16])
{
    uint8_t subkey[16] = {0}, block[16] = {0};
    bool ok = false;
    if (od_aes_block(key, subkey, subkey)) goto done;
    double_block(subkey);
    while (len > 16) {
        for (unsigned i = 0; i < 16; i++) block[i] ^= in[i];
        if (od_aes_block(key, block, block)) goto done;
        in += 16; len -= 16;
    }
    if (len < 16) { double_block(subkey); block[len] ^= 0x80; }
    for (size_t i = 0; i < len; i++) block[i] ^= in[i];
    for (unsigned i = 0; i < 16; i++) block[i] ^= subkey[i];
    ok = !od_aes_block(key, block, out);
done:
    wipe(subkey, sizeof(subkey)); wipe(block, sizeof(block)); return ok;
}
static bool ccm_tag(const uint8_t key[16], const uint8_t nonce[13], const uint8_t aad[2],
                    const uint8_t *data, size_t len, uint8_t tag[16])
{
    uint8_t block[16]; bool ok = false;
    block[0] = 0x69; /* Adata=1, M=12, L=2 */
    memcpy(block + 1, nonce, 13); block[14] = (uint8_t)(len >> 8); block[15] = (uint8_t)len;
    if (od_aes_block(key, block, tag)) goto done;
    tag[1] ^= 2; tag[2] ^= aad[0]; tag[3] ^= aad[1];
    if (od_aes_block(key, tag, tag)) goto done;
    for (size_t pos = 0; pos < len; pos += 16) {
        size_t count = len - pos < 16 ? len - pos : 16;
        for (size_t i = 0; i < count; i++) tag[i] ^= data[pos + i];
        if (od_aes_block(key, tag, tag)) goto done;
    }
    block[0] = 1; block[14] = 0; block[15] = 0;
    if (od_aes_block(key, block, block)) goto done;
    for (unsigned i = 0; i < 12; i++) tag[i] ^= block[i];
    ok = true;
done:
    wipe(block, sizeof(block)); return ok;
}
static bool ccm_xor(const uint8_t key[16], const uint8_t nonce[13], uint8_t *data, size_t len)
{
    uint8_t counter[16], stream[16]; bool ok = false;
    counter[0] = 1; memcpy(counter + 1, nonce, 13);
    for (size_t pos = 0; pos < len; pos += 16) {
        unsigned n = (unsigned)(pos / 16 + 1);
        counter[14] = (uint8_t)(n >> 8); counter[15] = (uint8_t)n;
        if (od_aes_block(key, counter, stream)) goto done;
        size_t count = len - pos < 16 ? len - pos : 16;
        for (size_t i = 0; i < count; i++) data[pos + i] ^= stream[i];
    }
    ok = true;
done:
    wipe(counter, sizeof(counter)); wipe(stream, sizeof(stream)); return ok;
}
bool od_ccm(const uint8_t key[16], const uint8_t nonce[13], const uint8_t aad[2],
            uint8_t *data, size_t len, bool decrypt)
{
    uint8_t tag[16]; bool ok = false;
    if (len > 214) return false;
    if (decrypt && !ccm_xor(key, nonce, data, len)) goto done;
    if (!ccm_tag(key, nonce, aad, data, len, tag)) goto done;
    if (decrypt) {
        uint8_t diff = 0;
        for (unsigned i = 0; i < 12; i++) diff |= tag[i] ^ data[len + i];
        ok = diff == 0;
    } else {
        if (!ccm_xor(key, nonce, data, len)) goto done;
        memcpy(data + len, tag, 12); ok = true;
    }
done:
    if (!ok) wipe(data, len);
    wipe(tag, sizeof(tag)); return ok;
}
