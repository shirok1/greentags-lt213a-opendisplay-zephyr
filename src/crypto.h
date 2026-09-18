#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* AES-128 encryption; input/output may alias. Return zero on success. */
int od_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
bool od_cmac(const uint8_t key[16], const uint8_t *in, size_t len, uint8_t out[16]);
/* OpenDisplay fixed profile: nonce 13, AAD 2, tag 12 bytes.
 * len is the payload length (0..214), excluding the tag. data has capacity
 * for len + 12 bytes; encryption appends the tag, decryption verifies it.
 * key/nonce/aad must not overlap data. On authentication/AES failure the
 * payload is wiped. An invalid length is rejected without touching data.
 */
bool od_ccm(const uint8_t key[16], const uint8_t nonce[13], const uint8_t aad[2],
            uint8_t *data, size_t len, bool decrypt);
