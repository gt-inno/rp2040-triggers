#include "system_clock.h"

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#ifndef SYS_CLK_KHZ
#define SYS_CLK_KHZ 125000u
#endif

#define SYSTEM_CLOCK_OVERCLOCK_KHZ 133000u

uint32_t system_clock_default_khz(void) {
    return SYS_CLK_KHZ;
}

uint32_t system_clock_get_khz(void) {
    return clock_get_hz(clk_sys) / 1000u;
}

bool system_clock_khz_is_valid(uint32_t freq_khz) {
    uint vco;
    uint postdiv1;
    uint postdiv2;

    if (freq_khz < SYSTEM_CLOCK_MIN_KHZ || freq_khz > SYSTEM_CLOCK_MAX_KHZ) {
        return false;
    }

    return check_sys_clock_khz(freq_khz, &vco, &postdiv1, &postdiv2);
}

bool system_clock_apply_khz(uint32_t freq_khz) {
    if (!system_clock_khz_is_valid(freq_khz)) {
        return false;
    }

    if (freq_khz > SYSTEM_CLOCK_OVERCLOCK_KHZ) {
        vreg_set_voltage(VREG_VOLTAGE_1_15);
        sleep_us(1000);
    }

    if (!set_sys_clock_khz(freq_khz, false)) {
        return false;
    }

    if (freq_khz <= SYSTEM_CLOCK_OVERCLOCK_KHZ) {
        vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
    }

    return true;
}
