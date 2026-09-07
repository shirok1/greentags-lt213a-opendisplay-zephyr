#include "security.h"
#include "fake_flash.h"
static struct od_security security;
static unsigned random_generation;
static int random_bytes(void *p, size_t len)
{ uint8_t *out = p; for (size_t i = 0; i < len; i++) { out[i] = (uint8_t)(0x10 + i + random_generation); } random_generation++; return 0; }
int setup(const uint8_t *config, size_t len)
{
    fake_init(); memset(&security, 0, sizeof(security)); od_security_reset(&security);
    random_generation = 0;
    int err = od_config_start(len); return err ? err : od_config_append(config, len);
}
void disconnect_session(void) { od_security_reset(&security); }
int auth(const uint8_t *p, size_t len, uint32_t now, uint8_t *out)
{ const uint8_t id[] = {0xa1, 0xb2, 0xc3, 0xd4}; return od_security_auth(&security, p, len, now, id, random_bytes, out); }
int decrypt(uint8_t *p, size_t len, uint32_t now) { return od_security_decrypt(&security, p, len, now); }
int encrypt(const uint8_t *p, size_t len, uint8_t *out, size_t capacity)
{ return od_security_encrypt(&security, p, len, out, capacity); }
int expire(uint32_t now) { return od_security_expire(&security, now); }
int authenticated(void) { return security.authenticated; }
