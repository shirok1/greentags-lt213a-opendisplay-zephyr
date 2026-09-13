#pragma once
#include <stdint.h>

/* Commit temperature/VDD together; keep the previous MSD on sampling failure. */
int od_read_msd(uint8_t msd[16]);
