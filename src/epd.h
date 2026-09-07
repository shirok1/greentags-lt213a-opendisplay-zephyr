#pragma once
#include <stddef.h>
#include <stdint.h>
#define EPD_WIDTH 104
#define EPD_HEIGHT 212
#define EPD_FRAME_BYTES ((EPD_WIDTH / 8) * EPD_HEIGHT)
int epd_init(void);
int epd_begin(void);
int epd_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
int epd_write(const uint8_t *data, size_t len);
int epd_finish(uint8_t mode);
int epd_off(void);
