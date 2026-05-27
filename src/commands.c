#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/time.h"
#include "settings.h"
#include "system_clock.h"
#include "trigger.h"

#define SETTINGS_AUTOSAVE_DELAY_US 500000u
#define SETTINGS_AUTOSAVE_RETRY_US 1000000u

static bool settings_dirty = false;
static uint64_t settings_save_due_us = 0;

static void mark_settings_dirty(void) {
    settings_dirty = true;
    settings_save_due_us = time_us_64() + SETTINGS_AUTOSAVE_DELAY_US;
}

static bool save_current_settings(void) {
    trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT];

    if (trigger_is_armed()) {
        return false;
    }

    return trigger_get_all_configs(configs) &&
           settings_save(configs, system_clock_get_khz());
}

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
    printf("  freq [khz]\n");
    printf("  save\n");
    printf("  load\n");
    printf("  factory_reset\n");
    printf("  config_status\n");
    printf("  clear_edges <ch>\n");
    printf("  monitor_start <ch>\n");
    printf("  monitor_status\n");
    printf("  monitor_stop\n");
    printf("  diag_config <ch> <input_gpio> <output_gpio> <fire_after_edges> <pulse_width_edges> <idle_gap_us>\n");
    printf("  diag_arm <ch>\n");
    printf("  diag_sweep_config <ch> <input_gpio> <output_gpio> <start_edge> <stop_edge> <step> <pulse_width_edges> <idle_gap_us>\n");
    printf("  diag_sweep_arm <ch>\n");
    printf("  diag_stop\n");
    printf("  diag_status\n");
    printf("  set <ch> input_gpio <0-29>\n");
    printf("  set <ch> output_gpio <0-29>\n");
    printf("  set <ch> mode time|edge_count\n");
    printf("  set <ch> edge rising|falling|both\n");
    printf("  set <ch> pull none|up|down\n");
    printf("  set <ch> delay_us <value>\n");
    printf("  set <ch> width_us <value>\n");
    printf("  set <ch> edge_count <1-65535>\n");
    printf("  set <ch> pulse_width_edges <1-65535>\n");
    printf("  set <ch> auto_clear_edges 0|1\n");
    printf("  set <ch> auto_clear_delay_ns <value>\n");
    printf("  set <ch> step_reduce_enabled 0|1\n");
    printf("  set <ch> step_reduce_every <trigger_count>\n");
    printf("  set <ch> step_reduce_edge_delta <0-65535>\n");
    printf("  set <ch> step_reduce_delay_ns <value>\n");
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
    printf("Clock: sys_khz=%lu\n", (unsigned long)system_clock_get_khz());

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        trigger_channel_status_t status;
        if (!trigger_get_status(ch, &status)) {
            continue;
        }

        printf(
            "CH%u: enabled=%u input=GP%u output=GP%u mode=%s edge=%s delay_us=%lu width_us=%lu edge_count=%lu pulse_width_edges=%lu auto_clear_edges=%u auto_clear_delay_ns=%lu step_reduce_enabled=%u step_reduce_every=%lu step_reduce_edge_delta=%lu step_reduce_delay_ns=%lu step_reduce_count=%lu step_current_edge_count=%lu step_current_delay_ns=%lu edge_seen=%lu idle=%s active=%s pending=%u events=%lu last_event_us=%llu input_level=%u output_level=%u pull=%s\n",
            ch + 1,
            status.enabled ? 1 : 0,
            status.input_gpio,
            status.output_gpio,
            trigger_mode_name(status.trigger_mode),
            trigger_edge_name(status.edge_mode),
            (unsigned long)status.delay_us,
            (unsigned long)status.width_us,
            (unsigned long)status.edge_count_target,
            (unsigned long)status.pulse_width_edges,
            status.auto_clear_edges ? 1 : 0,
            (unsigned long)status.auto_clear_delay_ns,
            status.step_reduce_enabled ? 1 : 0,
            (unsigned long)status.step_reduce_every,
            (unsigned long)status.step_reduce_edge_delta,
            (unsigned long)status.step_reduce_delay_ns,
            (unsigned long)status.step_reduce_count,
            (unsigned long)status.step_current_edge_count,
            (unsigned long)status.step_current_delay_ns,
            (unsigned long)status.edge_count_seen,
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

static void print_monitor_status(const trigger_monitor_status_t *status) {
    if (!status) {
        return;
    }

    printf(
        "MON: active=%u ch=%u input=GP%u edges=%llu elapsed_us=%llu rate_hz=%llu period_ns=%lu\n",
        status->active ? 1 : 0,
        status->channel < TRIGGER_CHANNEL_COUNT ? status->channel + 1 : 0,
        status->input_gpio,
        (unsigned long long)status->edges,
        (unsigned long long)status->elapsed_us,
        (unsigned long long)status->rate_hz,
        (unsigned long)status->period_ns
    );
}

static void print_diag_status(const trigger_diag_status_t *status) {
    if (!status || !status->valid) {
        printf("DIAG_STATUS active=0 sweep=0 complete=0 ch=0 input=GP0 output=GP0 run_index=0 edges=0 trigger_edge=0 pulse_width_edges=0 idle_gap_us=0 fired=0 first_edge=0\n");
        return;
    }

    printf(
        "DIAG_STATUS active=%u sweep=%u complete=%u ch=%u input=GP%u output=GP%u run_index=%lu edges=%llu trigger_edge=%lu pulse_width_edges=%lu idle_gap_us=%lu fired=%u first_edge=%u avg_swclk_period_ns=%lu min_period_ns=%lu max_period_ns=%lu duration_us=%llu\n",
        status->active ? 1 : 0,
        status->sweep ? 1 : 0,
        status->sweep_complete ? 1 : 0,
        status->channel < TRIGGER_CHANNEL_COUNT ? status->channel + 1 : 0,
        status->input_gpio,
        status->output_gpio,
        (unsigned long)status->run_index,
        (unsigned long long)status->total_edges,
        (unsigned long)status->trigger_edge,
        (unsigned long)status->pulse_width_edges,
        (unsigned long)status->idle_gap_us,
        status->fired ? 1 : 0,
        status->first_edge_seen ? 1 : 0,
        (unsigned long)status->avg_swclk_period_ns,
        (unsigned long)status->min_period_ns,
        (unsigned long)status->max_period_ns,
        (unsigned long long)status->transaction_duration_us
    );
}

static void print_diag_result(const trigger_diag_status_t *status) {
    if (!status || !status->valid) {
        return;
    }

    uint ch = status->channel < TRIGGER_CHANNEL_COUNT ? status->channel + 1 : 0;

    if (status->first_edge_seen) {
        printf(
            "DIAG_EVENT ch=%u type=first_edge edge=0 us=%llu\n",
            ch,
            (unsigned long long)status->first_us
        );
    }

    if (status->fired) {
        printf(
            "DIAG_EVENT ch=%u type=trigger_on edge=%lu us=%llu\n",
            ch,
            (unsigned long)status->trigger_edge,
            (unsigned long long)status->trigger_on_us
        );

        if (status->trigger_off_us != 0) {
            printf(
                "DIAG_EVENT ch=%u type=trigger_off edge=%llu us=%llu\n",
                ch,
                (unsigned long long)(
                    (uint64_t)status->trigger_edge + status->pulse_width_edges
                ),
                (unsigned long long)status->trigger_off_us
            );
        }
    }

    if (status->first_edge_seen) {
        printf(
            "DIAG_EVENT ch=%u type=last_edge edge=%llu us=%llu\n",
            ch,
            (unsigned long long)status->total_edges,
            (unsigned long long)status->last_edge_us
        );
    }

    printf(
        "DIAG ch=%u run_index=%lu total_edges=%llu trigger_edge=%lu fire_after_edges=%lu pulse_width_edges=%lu first_us=%llu trigger_on_us=%llu trigger_off_us=%llu last_edge_us=%llu idle_gap_us=%lu duration_us=%llu avg_swclk_period_ns=%lu min_period_ns=%lu max_period_ns=%lu fired=%u\n",
        ch,
        (unsigned long)status->run_index,
        (unsigned long long)status->total_edges,
        (unsigned long)status->trigger_edge,
        (unsigned long)status->fire_after_edges,
        (unsigned long)status->pulse_width_edges,
        (unsigned long long)status->first_us,
        (unsigned long long)status->trigger_on_us,
        (unsigned long long)status->trigger_off_us,
        (unsigned long long)status->last_edge_us,
        (unsigned long)status->idle_gap_us,
        (unsigned long long)status->transaction_duration_us,
        (unsigned long)status->avg_swclk_period_ns,
        (unsigned long)status->min_period_ns,
        (unsigned long)status->max_period_ns,
        status->fired ? 1 : 0
    );
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

static bool parse_trigger_mode(const char *value, trigger_mode_t *mode) {
    if (strcmp(value, "time") == 0) {
        *mode = TRIGGER_MODE_TIME;
        return true;
    }
    if (strcmp(value, "edge_count") == 0 || strcmp(value, "count") == 0) {
        *mode = TRIGGER_MODE_EDGE_COUNT;
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

        mark_settings_dirty();
        printf("[OK] CH%d input=GP%u\n", ch + 1, gpio);
        return;
    }

    if (strcmp(key, "output_gpio") == 0 || strcmp(key, "output") == 0) {
        uint gpio = parse_u32(value);
        if (!trigger_set_output_gpio((uint)ch, gpio)) {
            printf("[ERR] output_gpio must be 0-29, unused, and not the onboard LED pin\n");
            return;
        }

        mark_settings_dirty();
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
        mark_settings_dirty();
        printf("[OK] CH%d edge=%s\n", ch + 1, trigger_edge_name(mode));
        return;
    }

    if (strcmp(key, "mode") == 0 || strcmp(key, "trigger_mode") == 0) {
        trigger_mode_t mode;
        if (!parse_trigger_mode(value, &mode)) {
            printf("[ERR] mode must be time or edge_count\n");
            return;
        }

        if (!trigger_set_mode((uint)ch, mode)) {
            printf("[ERR] edge_count mode requires an unused PWM B-channel input pin such as GP3, GP5, or GP7\n");
            return;
        }

        mark_settings_dirty();
        printf("[OK] CH%d mode=%s\n", ch + 1, trigger_mode_name(mode));
        return;
    }

    if (strcmp(key, "pull") == 0 || strcmp(key, "input_pull") == 0) {
        trigger_input_pull_t pull;
        if (!parse_pull(value, &pull)) {
            printf("[ERR] pull must be none, up, or down\n");
            return;
        }

        trigger_set_input_pull((uint)ch, pull);
        mark_settings_dirty();
        printf("[OK] CH%d pull=%s\n", ch + 1, trigger_pull_name(pull));
        return;
    }

    if (strcmp(key, "delay_us") == 0) {
        uint32_t delay_us = parse_u32(value);
        trigger_set_delay_us((uint)ch, delay_us);
        mark_settings_dirty();
        printf("[OK] CH%d delay_us=%lu\n", ch + 1, (unsigned long)delay_us);
        return;
    }

    if (strcmp(key, "width_us") == 0) {
        uint32_t width_us = parse_u32(value);
        if (!trigger_set_width_us((uint)ch, width_us)) {
            printf("[ERR] width_us must be > 0\n");
            return;
        }

        mark_settings_dirty();
        printf("[OK] CH%d width_us=%lu\n", ch + 1, (unsigned long)width_us);
        return;
    }

    if (strcmp(key, "edge_count") == 0 || strcmp(key, "edge_count_target") == 0) {
        uint32_t edge_count = parse_u32(value);
        if (!trigger_set_edge_count_target((uint)ch, edge_count)) {
            printf("[ERR] edge_count must be 1-65535\n");
            return;
        }

        mark_settings_dirty();
        printf("[OK] CH%d edge_count=%lu\n", ch + 1, (unsigned long)edge_count);
        return;
    }

    if (strcmp(key, "pulse_width_edges") == 0 || strcmp(key, "width_edges") == 0) {
        uint32_t edge_count = parse_u32(value);
        if (!trigger_set_pulse_width_edges((uint)ch, edge_count)) {
            printf("[ERR] pulse_width_edges must be 1-65535\n");
            return;
        }

        mark_settings_dirty();
        printf("[OK] CH%d pulse_width_edges=%lu\n", ch + 1, (unsigned long)edge_count);
        return;
    }

    if (strcmp(key, "auto_clear_edges") == 0 || strcmp(key, "auto_clear") == 0) {
        bool enabled = atoi(value) != 0;
        trigger_set_auto_clear_edges((uint)ch, enabled);
        mark_settings_dirty();
        printf("[OK] CH%d auto_clear_edges=%u\n", ch + 1, enabled ? 1 : 0);
        return;
    }

    if (strcmp(key, "auto_clear_delay_ns") == 0 || strcmp(key, "clear_delay_ns") == 0) {
        uint32_t delay_ns = parse_u32(value);
        if (!trigger_set_auto_clear_delay_ns((uint)ch, delay_ns)) {
            printf("[ERR] auto_clear_delay_ns must be 0-4294967295\n");
            return;
        }

        mark_settings_dirty();
        printf("[OK] CH%d auto_clear_delay_ns=%lu\n", ch + 1, (unsigned long)delay_ns);
        return;
    }

    if (strcmp(key, "step_reduce_enabled") == 0 || strcmp(key, "step_reduce") == 0) {
        bool enabled = atoi(value) != 0;
        trigger_set_step_reduce_enabled((uint)ch, enabled);
        mark_settings_dirty();
        printf("[OK] CH%d step_reduce_enabled=%u\n", ch + 1, enabled ? 1 : 0);
        return;
    }

    if (strcmp(key, "step_reduce_every") == 0 || strcmp(key, "step_every") == 0) {
        uint32_t every = parse_u32(value);
        if (!trigger_set_step_reduce_every((uint)ch, every)) {
            printf("[ERR] step_reduce_every must be greater than 0\n");
            return;
        }

        mark_settings_dirty();
        printf("[OK] CH%d step_reduce_every=%lu\n", ch + 1, (unsigned long)every);
        return;
    }

    if (strcmp(key, "step_reduce_edge_delta") == 0 ||
        strcmp(key, "step_edge_delta") == 0) {
        uint32_t delta = parse_u32(value);
        if (!trigger_set_step_reduce_edge_delta((uint)ch, delta)) {
            printf("[ERR] step_reduce_edge_delta must be 0-65535\n");
            return;
        }

        mark_settings_dirty();
        printf("[OK] CH%d step_reduce_edge_delta=%lu\n", ch + 1, (unsigned long)delta);
        return;
    }

    if (strcmp(key, "step_reduce_delay_ns") == 0 ||
        strcmp(key, "step_delay_ns") == 0) {
        uint32_t delay_ns = parse_u32(value);
        if (!trigger_set_step_reduce_delay_ns((uint)ch, delay_ns)) {
            printf("[ERR] step_reduce_delay_ns must be 0-4294967295\n");
            return;
        }

        mark_settings_dirty();
        printf("[OK] CH%d step_reduce_delay_ns=%lu\n", ch + 1, (unsigned long)delay_ns);
        return;
    }

    if (strcmp(key, "enabled") == 0) {
        bool enabled = atoi(value) != 0;
        trigger_set_enabled((uint)ch, enabled);
        mark_settings_dirty();
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
        mark_settings_dirty();
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
        mark_settings_dirty();
        printf("[OK] CH%d active=%s\n", ch + 1, trigger_level_name(high));
        return;
    }

    printf("[ERR] unknown set key\n");
}

void command_process(char *line) {
    char *argv[10] = {0};
    int argc = 0;

    char *token = strtok(line, " \t\r\n");
    while (token && argc < 10) {
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

    if (strcmp(argv[0], "freq") == 0) {
        uint32_t freq_khz;

        if (argc < 2) {
            printf("Clock: sys_khz=%lu\n", (unsigned long)system_clock_get_khz());
            return;
        }

        if (trigger_is_armed()) {
            printf("[ERR] disarm before changing system clock\n");
            return;
        }

        freq_khz = parse_u32(argv[1]);
        if (!system_clock_apply_khz(freq_khz)) {
            printf(
                "[ERR] freq must be an attainable clock from %lu to %lu kHz\n",
                (unsigned long)SYSTEM_CLOCK_MIN_KHZ,
                (unsigned long)SYSTEM_CLOCK_MAX_KHZ
            );
            return;
        }

        mark_settings_dirty();
        printf("[OK] freq=%lu kHz\n", (unsigned long)system_clock_get_khz());
        return;
    }

    if (strcmp(argv[0], "save") == 0) {
        if (trigger_is_armed()) {
            printf("[ERR] disarm before saving settings\n");
            return;
        }

        if (!save_current_settings()) {
            printf("[ERR] settings save failed\n");
            return;
        }

        settings_dirty = false;
        printf("[OK] settings saved\n");
        return;
    }

    if (strcmp(argv[0], "load") == 0) {
        trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT];
        uint32_t system_clock_khz;

        if (!settings_load(configs, &system_clock_khz)) {
            printf("[ERR] no valid saved settings\n");
            return;
        }

        if (!system_clock_apply_khz(system_clock_khz) ||
            !trigger_apply_all_configs(configs)) {
            printf("[ERR] saved settings are invalid\n");
            return;
        }

        settings_dirty = false;
        printf("[OK] settings loaded\n");
        return;
    }

    if (strcmp(argv[0], "factory_reset") == 0) {
        trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT];

        trigger_config_defaults(configs);
        if (!system_clock_apply_khz(system_clock_default_khz()) ||
            !trigger_apply_all_configs(configs)) {
            printf("[ERR] failed to apply defaults\n");
            return;
        }

        if (!settings_clear()) {
            printf("[ERR] failed to erase saved settings\n");
            return;
        }

        settings_dirty = false;
        printf("[OK] factory defaults restored\n");
        return;
    }

    if (strcmp(argv[0], "config_status") == 0) {
        settings_info_t info;

        if (!settings_get_info(&info)) {
            printf("[ERR] settings status failed\n");
            return;
        }

        printf(
            "Settings: saved=%s dirty=%s sequence=%lu valid_records=%lu system_clock_khz=%lu flash_offset=0x%08lx storage_bytes=%lu\n",
            info.has_record ? "yes" : "no",
            settings_dirty ? "yes" : "no",
            (unsigned long)info.sequence,
            (unsigned long)info.valid_records,
            (unsigned long)info.system_clock_khz,
            (unsigned long)info.flash_offset,
            (unsigned long)info.storage_bytes
        );
        return;
    }

    if (strcmp(argv[0], "clear_edges") == 0) {
        if (argc < 2) {
            printf("[ERR] missing channel\n");
            return;
        }

        int ch = parse_channel(argv[1]);
        if (ch < 0) {
            printf("[ERR] invalid channel\n");
            return;
        }

        if (!trigger_clear_edge_seen((uint)ch)) {
            printf("[ERR] failed to clear edge counter\n");
            return;
        }

        printf("[OK] cleared edges CH%d\n", ch + 1);
        return;
    }

    if (strcmp(argv[0], "monitor_start") == 0) {
        if (argc < 2) {
            printf("[ERR] missing channel\n");
            return;
        }

        int ch = parse_channel(argv[1]);
        if (ch < 0) {
            printf("[ERR] invalid channel\n");
            return;
        }

        if (!trigger_monitor_start((uint)ch)) {
            printf("[ERR] monitor needs disarmed unit, idle monitor, and PWM B-channel input such as GP3, GP5, or GP7\n");
            return;
        }

        printf("[OK] monitor started CH%d\n", ch + 1);
        trigger_monitor_status_t status;
        trigger_monitor_get_status(&status);
        print_monitor_status(&status);
        return;
    }

    if (strcmp(argv[0], "monitor_status") == 0) {
        trigger_monitor_status_t status;
        trigger_monitor_get_status(&status);
        print_monitor_status(&status);
        return;
    }

    if (strcmp(argv[0], "monitor_stop") == 0) {
        trigger_monitor_status_t status;
        if (!trigger_monitor_stop(&status)) {
            printf("[ERR] monitor is not running\n");
            print_monitor_status(&status);
            return;
        }

        printf("[OK] monitor stopped\n");
        print_monitor_status(&status);
        return;
    }

    if (strcmp(argv[0], "diag_config") == 0) {
        if (argc < 7) {
            printf("[ERR] usage: diag_config <ch> <input_gpio> <output_gpio> <fire_after_edges> <pulse_width_edges> <idle_gap_us>\n");
            return;
        }

        int ch = parse_channel(argv[1]);
        if (ch < 0) {
            printf("[ERR] invalid channel\n");
            return;
        }

        uint input_gpio = parse_u32(argv[2]);
        uint output_gpio = parse_u32(argv[3]);
        uint32_t fire_after_edges = parse_u32(argv[4]);
        uint32_t pulse_width_edges = parse_u32(argv[5]);
        uint32_t idle_gap_us = parse_u32(argv[6]);

        if (!trigger_diag_config(
                (uint)ch,
                input_gpio,
                output_gpio,
                fire_after_edges,
                pulse_width_edges,
                idle_gap_us
            )) {
            printf("[ERR] diag_config needs disarmed unit, PWM B input such as GP3/GP5/GP7, distinct output GPIO, fire_after_edges 1-65535, pulse_width_edges 1-65535, and idle_gap_us > 0\n");
            return;
        }

        printf(
            "[OK] diag_config CH%d input=GP%u output=GP%u fire_after_edges=%lu pulse_width_edges=%lu idle_gap_us=%lu\n",
            ch + 1,
            input_gpio,
            output_gpio,
            (unsigned long)fire_after_edges,
            (unsigned long)pulse_width_edges,
            (unsigned long)idle_gap_us
        );
        return;
    }

    if (strcmp(argv[0], "diag_arm") == 0) {
        if (argc < 2) {
            printf("[ERR] missing channel\n");
            return;
        }

        int ch = parse_channel(argv[1]);
        if (ch < 0) {
            printf("[ERR] invalid channel\n");
            return;
        }

        if (!trigger_diag_arm((uint)ch)) {
            printf("[ERR] diag_arm needs disarmed unit, idle monitor/diagnostic, and matching diag_config channel\n");
            return;
        }

        printf("[OK] diag armed CH%d\n", ch + 1);
        return;
    }

    if (strcmp(argv[0], "diag_sweep_config") == 0) {
        if (argc < 9) {
            printf("[ERR] usage: diag_sweep_config <ch> <input_gpio> <output_gpio> <start_edge> <stop_edge> <step> <pulse_width_edges> <idle_gap_us>\n");
            return;
        }

        int ch = parse_channel(argv[1]);
        if (ch < 0) {
            printf("[ERR] invalid channel\n");
            return;
        }

        uint input_gpio = parse_u32(argv[2]);
        uint output_gpio = parse_u32(argv[3]);
        uint32_t start_edge = parse_u32(argv[4]);
        uint32_t stop_edge = parse_u32(argv[5]);
        uint32_t step = parse_u32(argv[6]);
        uint32_t pulse_width_edges = parse_u32(argv[7]);
        uint32_t idle_gap_us = parse_u32(argv[8]);

        if (!trigger_diag_sweep_config(
                (uint)ch,
                input_gpio,
                output_gpio,
                start_edge,
                stop_edge,
                step,
                pulse_width_edges,
                idle_gap_us
            )) {
            printf("[ERR] diag_sweep_config needs disarmed unit, PWM B input such as GP3/GP5/GP7, distinct output GPIO, start/stop 1-65535 with stop>=start, step > 0, pulse_width_edges 1-65535, and idle_gap_us > 0\n");
            return;
        }

        printf(
            "[OK] diag_sweep_config CH%d input=GP%u output=GP%u start_edge=%lu stop_edge=%lu step=%lu pulse_width_edges=%lu idle_gap_us=%lu\n",
            ch + 1,
            input_gpio,
            output_gpio,
            (unsigned long)start_edge,
            (unsigned long)stop_edge,
            (unsigned long)step,
            (unsigned long)pulse_width_edges,
            (unsigned long)idle_gap_us
        );
        return;
    }

    if (strcmp(argv[0], "diag_sweep_arm") == 0) {
        if (argc < 2) {
            printf("[ERR] missing channel\n");
            return;
        }

        int ch = parse_channel(argv[1]);
        if (ch < 0) {
            printf("[ERR] invalid channel\n");
            return;
        }

        if (!trigger_diag_sweep_arm((uint)ch)) {
            printf("[ERR] diag_sweep_arm needs disarmed unit, idle monitor/diagnostic, sweep config, and an unfinished sweep target\n");
            return;
        }

        printf("[OK] diag sweep armed CH%d\n", ch + 1);
        return;
    }

    if (strcmp(argv[0], "diag_stop") == 0) {
        trigger_diag_status_t status;
        if (!trigger_diag_stop(&status)) {
            printf("[ERR] diagnostic is not running\n");
            print_diag_status(&status);
            return;
        }

        printf("[OK] diagnostic stopped\n");
        print_diag_result(&status);
        return;
    }

    if (strcmp(argv[0], "diag_status") == 0) {
        trigger_diag_status_t status;
        trigger_diag_get_status(&status);
        print_diag_status(&status);
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

void command_poll(void) {
    trigger_diag_status_t diag_result;
    if (trigger_diag_consume_result(&diag_result)) {
        print_diag_result(&diag_result);
    }

    if (!settings_dirty || trigger_is_armed()) {
        return;
    }

    trigger_diag_status_t diag_status;
    if (trigger_diag_get_status(&diag_status)) {
        return;
    }

    uint64_t now_us = time_us_64();
    if ((int64_t)(now_us - settings_save_due_us) < 0) {
        return;
    }

    if (save_current_settings()) {
        settings_dirty = false;
        printf("[OK] settings saved\n");
        return;
    }

    settings_save_due_us = now_us + SETTINGS_AUTOSAVE_RETRY_US;
    printf("[ERR] settings autosave failed\n");
}
