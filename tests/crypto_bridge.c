#include "crypto.h"
#include <errno.h>

typedef int (*aes_block_fn)(const uint8_t *, const uint8_t *, uint8_t *);
static aes_block_fn encrypt_block;
static int calls_before_failure = -1;
void set_aes_block(aes_block_fn fn) { encrypt_block = fn; }
void fail_aes_after(int calls) { calls_before_failure = calls; }
int od_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    if (calls_before_failure == 0) { return -EIO; }
    if (calls_before_failure > 0) { calls_before_failure--; }
    return encrypt_block ? encrypt_block(key, in, out) : -EIO;
}
