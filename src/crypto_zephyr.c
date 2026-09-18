#include "crypto.h"
#include <zephyr/bluetooth/crypto.h>
int od_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{ return bt_encrypt_be(key, in, out); }
