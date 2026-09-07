#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define OD_CONFIG_MAX 768
struct od_flash_ops {
    int (*erase)(unsigned slot);
    int (*write)(unsigned slot, size_t offset, const void *data, size_t len);
    const uint8_t *(*read)(unsigned slot);
};
void od_config_init(const struct od_flash_ops *ops);
const uint8_t *od_config_get(size_t *len);
const uint8_t *od_config_security(void);
bool od_config_writing(void);
void od_config_cancel(void);
int od_config_start(size_t total);
int od_config_append(const uint8_t *data, size_t len);
int od_config_clear(void);
int od_config_validate(const uint8_t *data, size_t len);
