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
#include "trigger.h"

#define CMD_BUF_LEN 192

int main(void) {
    stdio_init_all();
    sleep_ms(1500);

    trigger_init();

    printf("\nRP2040 4-Channel Trigger/Delay/Pulse Firmware ready\n");
    printf("Type help for commands.\n");
    command_print_status();

    char cmd[CMD_BUF_LEN];
    uint idx = 0;

    while (true) {
        int c = getchar_timeout_us(1000);

        if (c == PICO_ERROR_TIMEOUT) {
            tight_loop_contents();
            continue;
        }

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            cmd[idx] = '\0';
            command_process(cmd);
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
