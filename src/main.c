/*
 * RP2040 OEM Pico SDK 4-Channel Trigger/Delay/Pulse Firmware
 * -----------------------------------------------------------
 * Target: Raspberry Pi Pico / RP2040
 * SDK: Raspberry Pi Pico SDK 1.5.1+ compatible
 * Arduino: NOT required
 * PIO: NOT required
 *
 * USB serial commands are handled in commands.c.
 * Trigger timing and GPIO behavior are handled in trigger.c.
 */

#include <stdio.h>

#include "commands.h"
#include "pico/stdlib.h"
#include "settings.h"
#include "system_clock.h"
#include "trigger.h"

#define CMD_BUF_LEN 192

int main(void) {
    stdio_init_all();
    sleep_ms(1500);

    trigger_channel_config_t saved_configs[TRIGGER_CHANNEL_COUNT];
    uint32_t saved_clock_khz = system_clock_default_khz();
    bool loaded_saved_settings = settings_load(saved_configs, &saved_clock_khz);
    if (loaded_saved_settings) {
        loaded_saved_settings = system_clock_apply_khz(saved_clock_khz) &&
                                trigger_apply_all_configs(saved_configs);
    }

    trigger_init();

    printf("\nRP2040 4-Channel Trigger/Delay/Pulse Firmware ready\n");
    printf("Settings: %s\n", loaded_saved_settings ? "loaded from flash" : "using defaults");
    printf("Type help for commands.\n");
    command_print_status();

    char cmd[CMD_BUF_LEN];
    uint idx = 0;

    while (true) {
        int c = getchar_timeout_us(1000);

        if (c == PICO_ERROR_TIMEOUT) {
            command_poll();
            tight_loop_contents();
            continue;
        }

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            cmd[idx] = '\0';
            command_process(cmd);
            command_poll();
            idx = 0;
            continue;
        }

        if (idx < CMD_BUF_LEN - 1) {
            cmd[idx++] = (char)c;
        } else {
            idx = 0;
            printf("[ERR] command too long\n");
        }
    }

    return 0;
}
