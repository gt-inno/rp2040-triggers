#ifndef SYSTEM_CLOCK_H
#define SYSTEM_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define SYSTEM_CLOCK_MIN_KHZ 48000u
#define SYSTEM_CLOCK_MAX_KHZ 200000u

uint32_t system_clock_default_khz(void);
uint32_t system_clock_get_khz(void);
bool system_clock_khz_is_valid(uint32_t freq_khz);
bool system_clock_apply_khz(uint32_t freq_khz);

#endif
