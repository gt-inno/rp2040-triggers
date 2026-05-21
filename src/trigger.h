#ifndef TRIGGER_H
#define TRIGGER_H

#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"

#define TRIGGER_CHANNEL_COUNT 4
#define TRIGGER_EDGE_COUNT_DEFAULT 16742u
#define TRIGGER_PULSE_WIDTH_EDGES_DEFAULT 100u
#define TRIGGER_EDGE_COUNT_MAX 65535u
#define TRIGGER_DIAG_FIRE_AFTER_EDGES_DEFAULT 4220u
#define TRIGGER_DIAG_PULSE_WIDTH_EDGES_DEFAULT 120u
#define TRIGGER_DIAG_IDLE_GAP_US_DEFAULT 50u
#define TRIGGER_DIAG_SWEEP_START_EDGE_DEFAULT 4100u
#define TRIGGER_DIAG_SWEEP_STOP_EDGE_DEFAULT 4300u
#define TRIGGER_DIAG_SWEEP_STEP_DEFAULT 5u
#define TRIGGER_DIAG_SWEEP_IDLE_GAP_US_DEFAULT 5000u

typedef enum {
    TRIGGER_MODE_TIME = 0,
    TRIGGER_MODE_EDGE_COUNT = 1
} trigger_mode_t;

typedef enum {
    TRIGGER_EDGE_RISING = 0,
    TRIGGER_EDGE_FALLING = 1,
    TRIGGER_EDGE_BOTH = 2
} trigger_edge_mode_t;

typedef enum {
    TRIGGER_PULL_NONE = 0,
    TRIGGER_PULL_UP = 1,
    TRIGGER_PULL_DOWN = 2
} trigger_input_pull_t;

typedef struct {
    uint input_gpio;
    uint output_gpio;
    bool enabled;
    trigger_edge_mode_t edge_mode;
    trigger_input_pull_t input_pull;
    uint32_t delay_us;
    uint32_t width_us;
    trigger_mode_t trigger_mode;
    uint32_t edge_count_target;
    uint32_t pulse_width_edges;
    bool idle_high;
    bool active_high;
} trigger_channel_config_t;

typedef struct {
    uint input_gpio;
    uint output_gpio;
    uint input_level;
    uint output_level;
    bool enabled;
    bool pending;
    uint32_t event_count;
    uint64_t last_event_us;
    trigger_edge_mode_t edge_mode;
    trigger_input_pull_t input_pull;
    uint32_t delay_us;
    uint32_t width_us;
    trigger_mode_t trigger_mode;
    uint32_t edge_count_target;
    uint32_t pulse_width_edges;
    uint32_t edge_count_seen;
    bool idle_high;
    bool active_high;
} trigger_channel_status_t;

typedef struct {
    bool active;
    uint channel;
    uint input_gpio;
    uint64_t edges;
    uint64_t elapsed_us;
    uint64_t rate_hz;
    uint32_t period_ns;
} trigger_monitor_status_t;

typedef struct {
    bool valid;
    bool active;
    bool sweep;
    bool sweep_complete;
    bool fired;
    bool first_edge_seen;
    uint channel;
    uint input_gpio;
    uint output_gpio;
    uint32_t run_index;
    uint32_t trigger_edge;
    uint32_t fire_after_edges;
    uint32_t pulse_width_edges;
    uint32_t idle_gap_us;
    uint64_t total_edges;
    uint64_t first_us;
    uint64_t trigger_on_us;
    uint64_t trigger_off_us;
    uint64_t last_edge_us;
    uint64_t transaction_duration_us;
    uint32_t avg_swclk_period_ns;
    uint32_t min_period_ns;
    uint32_t max_period_ns;
} trigger_diag_status_t;

void trigger_init(void);
bool trigger_is_armed(void);
void trigger_arm(void);
void trigger_disarm(void);
bool trigger_fire(uint ch);
bool trigger_drive_idle(uint ch);
bool trigger_drive_active(uint ch);
bool trigger_drive_level(uint ch, bool high);

void trigger_config_defaults(trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT]);
bool trigger_config_validate_all(const trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT]);
bool trigger_get_config(uint ch, trigger_channel_config_t *config);
bool trigger_get_all_configs(trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT]);
bool trigger_apply_all_configs(const trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT]);

bool trigger_get_status(uint ch, trigger_channel_status_t *status);
bool trigger_set_input_gpio(uint ch, uint gpio);
bool trigger_set_output_gpio(uint ch, uint gpio);
bool trigger_set_edge(uint ch, trigger_edge_mode_t mode);
bool trigger_set_input_pull(uint ch, trigger_input_pull_t pull);
bool trigger_set_delay_us(uint ch, uint32_t delay_us);
bool trigger_set_width_us(uint ch, uint32_t width_us);
bool trigger_set_mode(uint ch, trigger_mode_t mode);
bool trigger_set_edge_count_target(uint ch, uint32_t edge_count);
bool trigger_set_pulse_width_edges(uint ch, uint32_t edge_count);
bool trigger_clear_edge_seen(uint ch);
bool trigger_set_enabled(uint ch, bool enabled);
bool trigger_set_idle_high(uint ch, bool high);
bool trigger_set_active_high(uint ch, bool high);

bool trigger_monitor_start(uint ch);
bool trigger_monitor_stop(trigger_monitor_status_t *status);
bool trigger_monitor_get_status(trigger_monitor_status_t *status);

bool trigger_diag_config(
    uint ch,
    uint input_gpio,
    uint output_gpio,
    uint32_t fire_after_edges,
    uint32_t pulse_width_edges,
    uint32_t idle_gap_us
);
bool trigger_diag_arm(uint ch);
bool trigger_diag_stop(trigger_diag_status_t *status);
bool trigger_diag_get_status(trigger_diag_status_t *status);
bool trigger_diag_consume_result(trigger_diag_status_t *status);
bool trigger_diag_sweep_config(
    uint ch,
    uint input_gpio,
    uint output_gpio,
    uint32_t start_edge,
    uint32_t stop_edge,
    uint32_t step,
    uint32_t pulse_width_edges,
    uint32_t idle_gap_us
);
bool trigger_diag_sweep_arm(uint ch);

const char *trigger_mode_name(trigger_mode_t mode);
const char *trigger_edge_name(trigger_edge_mode_t mode);
const char *trigger_pull_name(trigger_input_pull_t pull);
const char *trigger_level_name(bool high);

#endif
