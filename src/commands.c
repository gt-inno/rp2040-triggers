#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trigger.h"

static int parse_channel(const char *text) {
    if (!text) {
        return -1;
    }

    int n = atoi(text);
    if (n < 1 || n > TRIGGER_CHANNEL_COUNT) {
        return -1;
    }

    return n - 1;
}

static uint32_t parse_u32(const char *text) {
    if (!text) {
        return 0;
    }

    return (uint32_t)strtoul(text, NULL, 10);
}

static void print_help(void) {
    printf("\n");
    printf("RP2040 4-Channel Trigger/Delay/Pulse Firmware\n");
    printf("------------------------------------------------\n");
    printf("Commands:\n");
    printf("  help\n");
    printf("  status\n");
    printf("  arm\n");
    printf("  disarm\n");
    printf("  fire <ch>\n");
    printf("  drive <ch> idle|active|low|high\n");
    printf("  set <ch> input_gpio <0-29>\n");
    printf("  set <ch> output_gpio <0-29>\n");
    printf("  set <ch> edge rising|falling|both\n");
    printf("  set <ch> pull none|up|down\n");
    printf("  set <ch> delay_us <value>\n");
    printf("  set <ch> width_us <value>\n");
    printf("  set <ch> enabled 0|1\n");
    printf("  set <ch> idle low|high\n");
    printf("  set <ch> active low|high\n");
    printf("\n");
    printf("Default pins:\n");
    printf("  CH1 input GP2  -> output GP10\n");
    printf("  CH2 input GP3  -> output GP11\n");
    printf("  CH3 input GP4  -> output GP12\n");
    printf("  CH4 input GP5  -> output GP13\n");
    printf("\n");
    printf("Example:\n");
    printf("  set 1 edge rising\n");
    printf("  set 1 delay_us 50\n");
    printf("  set 1 width_us 100\n");
    printf("  arm\n");
    printf("\n");
}

void command_print_status(void) {
    printf("\n");
    printf("Armed: %s\n", trigger_is_armed() ? "yes" : "no");

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        trigger_channel_status_t status;
        if (!trigger_get_status(ch, &status)) {
            continue;
        }

        printf(
            "CH%u: enabled=%u input=GP%u output=GP%u edge=%s delay_us=%lu width_us=%lu idle=%s active=%s pending=%u events=%lu last_event_us=%llu input_level=%u output_level=%u pull=%s\n",
            ch + 1,
            status.enabled ? 1 : 0,
            status.input_gpio,
            status.output_gpio,
            trigger_edge_name(status.edge_mode),
            (unsigned long)status.delay_us,
            (unsigned long)status.width_us,
            trigger_level_name(status.idle_high),
            trigger_level_name(status.active_high),
            status.pending ? 1 : 0,
            (unsigned long)status.event_count,
            (unsigned long long)status.last_event_us,
            status.input_level,
            status.output_level,
            trigger_pull_name(status.input_pull)
        );
    }

    printf("\n");
}

static bool parse_edge_mode(const char *value, trigger_edge_mode_t *mode) {
    if (strcmp(value, "rising") == 0) {
        *mode = TRIGGER_EDGE_RISING;
        return true;
    }
    if (strcmp(value, "falling") == 0) {
        *mode = TRIGGER_EDGE_FALLING;
        return true;
    }
    if (strcmp(value, "both") == 0) {
        *mode = TRIGGER_EDGE_BOTH;
        return true;
    }
    return false;
}

static bool parse_level(const char *value, bool *high) {
    if (strcmp(value, "high") == 0) {
        *high = true;
        return true;
    }
    if (strcmp(value, "low") == 0) {
        *high = false;
        return true;
    }
    return false;
}

static bool parse_pull(const char *value, trigger_input_pull_t *pull) {
    if (strcmp(value, "none") == 0) {
        *pull = TRIGGER_PULL_NONE;
        return true;
    }
    if (strcmp(value, "up") == 0) {
        *pull = TRIGGER_PULL_UP;
        return true;
    }
    if (strcmp(value, "down") == 0) {
        *pull = TRIGGER_PULL_DOWN;
        return true;
    }
    return false;
}

static void process_set_command(int ch, const char *key, const char *value) {
    if (!key || !value) {
        printf("[ERR] missing set arguments\n");
        return;
    }

    if (strcmp(key, "input_gpio") == 0 || strcmp(key, "input") == 0) {
        uint gpio = parse_u32(value);
        if (!trigger_set_input_gpio((uint)ch, gpio)) {
            printf("[ERR] input_gpio must be 0-29, unused, and not the onboard LED pin\n");
            return;
        }

        printf("[OK] CH%d input=GP%u\n", ch + 1, gpio);
        return;
    }

    if (strcmp(key, "output_gpio") == 0 || strcmp(key, "output") == 0) {
        uint gpio = parse_u32(value);
        if (!trigger_set_output_gpio((uint)ch, gpio)) {
            printf("[ERR] output_gpio must be 0-29, unused, and not the onboard LED pin\n");
            return;
        }

        printf("[OK] CH%d output=GP%u\n", ch + 1, gpio);
        return;
    }

    if (strcmp(key, "edge") == 0) {
        trigger_edge_mode_t mode;
        if (!parse_edge_mode(value, &mode)) {
            printf("[ERR] edge must be rising, falling, or both\n");
            return;
        }

        trigger_set_edge((uint)ch, mode);
        printf("[OK] CH%d edge=%s\n", ch + 1, trigger_edge_name(mode));
        return;
    }

    if (strcmp(key, "pull") == 0 || strcmp(key, "input_pull") == 0) {
        trigger_input_pull_t pull;
        if (!parse_pull(value, &pull)) {
            printf("[ERR] pull must be none, up, or down\n");
            return;
        }

        trigger_set_input_pull((uint)ch, pull);
        printf("[OK] CH%d pull=%s\n", ch + 1, trigger_pull_name(pull));
        return;
    }

    if (strcmp(key, "delay_us") == 0) {
        uint32_t delay_us = parse_u32(value);
        trigger_set_delay_us((uint)ch, delay_us);
        printf("[OK] CH%d delay_us=%lu\n", ch + 1, (unsigned long)delay_us);
        return;
    }

    if (strcmp(key, "width_us") == 0) {
        uint32_t width_us = parse_u32(value);
        if (!trigger_set_width_us((uint)ch, width_us)) {
            printf("[ERR] width_us must be > 0\n");
            return;
        }

        printf("[OK] CH%d width_us=%lu\n", ch + 1, (unsigned long)width_us);
        return;
    }

    if (strcmp(key, "enabled") == 0) {
        bool enabled = atoi(value) != 0;
        trigger_set_enabled((uint)ch, enabled);
        printf("[OK] CH%d enabled=%u\n", ch + 1, enabled ? 1 : 0);
        return;
    }

    if (strcmp(key, "idle") == 0 || strcmp(key, "out_idle") == 0) {
        bool high;
        if (!parse_level(value, &high)) {
            printf("[ERR] idle must be low or high\n");
            return;
        }

        trigger_set_idle_high((uint)ch, high);
        printf("[OK] CH%d idle=%s\n", ch + 1, trigger_level_name(high));
        return;
    }

    if (strcmp(key, "active") == 0 || strcmp(key, "out_active") == 0) {
        bool high;
        if (!parse_level(value, &high)) {
            printf("[ERR] active must be low or high\n");
            return;
        }

        trigger_set_active_high((uint)ch, high);
        printf("[OK] CH%d active=%s\n", ch + 1, trigger_level_name(high));
        return;
    }

    printf("[ERR] unknown set key\n");
}

void command_process(char *line) {
    char *argv[5] = {0};
    int argc = 0;

    char *token = strtok(line, " \t\r\n");
    while (token && argc < 5) {
        argv[argc++] = token;
        token = strtok(NULL, " \t\r\n");
    }

    if (argc == 0) {
        return;
    }

    if (strcmp(argv[0], "help") == 0) {
        print_help();
        return;
    }

    if (strcmp(argv[0], "status") == 0) {
        command_print_status();
        return;
    }

    if (strcmp(argv[0], "arm") == 0) {
        trigger_arm();
        printf("[OK] armed\n");
        return;
    }

    if (strcmp(argv[0], "disarm") == 0) {
        trigger_disarm();
        printf("[OK] disarmed\n");
        return;
    }

    if (strcmp(argv[0], "fire") == 0) {
        if (argc < 2) {
            printf("[ERR] missing channel\n");
            return;
        }

        int ch = parse_channel(argv[1]);
        if (ch < 0) {
            printf("[ERR] invalid channel\n");
            return;
        }

        trigger_fire((uint)ch);
        printf("[OK] fired CH%d\n", ch + 1);
        return;
    }

    if (strcmp(argv[0], "drive") == 0) {
        if (argc < 3) {
            printf("[ERR] usage: drive <ch> idle|active|low|high\n");
            return;
        }

        int ch = parse_channel(argv[1]);
        if (ch < 0) {
            printf("[ERR] invalid channel\n");
            return;
        }

        if (strcmp(argv[2], "idle") == 0) {
            trigger_drive_idle((uint)ch);
        } else if (strcmp(argv[2], "active") == 0) {
            trigger_drive_active((uint)ch);
        } else if (strcmp(argv[2], "low") == 0) {
            trigger_drive_level((uint)ch, false);
        } else if (strcmp(argv[2], "high") == 0) {
            trigger_drive_level((uint)ch, true);
        } else {
            printf("[ERR] drive level must be idle, active, low, or high\n");
            return;
        }

        printf("[OK] drove CH%d %s\n", ch + 1, argv[2]);
        return;
    }

    if (strcmp(argv[0], "set") == 0) {
        if (argc < 4) {
            printf("[ERR] usage: set <ch> <key> <value>\n");
            return;
        }

        int ch = parse_channel(argv[1]);
        if (ch < 0) {
            printf("[ERR] invalid channel\n");
            return;
        }

        process_set_command(ch, argv[2], argv[3]);
        return;
    }

    printf("[ERR] unknown command. Type help\n");
}
