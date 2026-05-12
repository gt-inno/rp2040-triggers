#ifndef TRIGGER_H
#define TRIGGER_H

#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"

#define TRIGGER_CHANNEL_COUNT 4

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
    bool idle_high;
    bool active_high;
} trigger_channel_status_t;

void trigger_init(void);
bool trigger_is_armed(void);
void trigger_arm(void);
void trigger_disarm(void);
bool trigger_fire(uint ch);
bool trigger_drive_idle(uint ch);
bool trigger_drive_active(uint ch);
bool trigger_drive_level(uint ch, bool high);

bool trigger_get_status(uint ch, trigger_channel_status_t *status);
bool trigger_set_input_gpio(uint ch, uint gpio);
bool trigger_set_output_gpio(uint ch, uint gpio);
bool trigger_set_edge(uint ch, trigger_edge_mode_t mode);
bool trigger_set_input_pull(uint ch, trigger_input_pull_t pull);
bool trigger_set_delay_us(uint ch, uint32_t delay_us);
bool trigger_set_width_us(uint ch, uint32_t width_us);
bool trigger_set_enabled(uint ch, bool enabled);
bool trigger_set_idle_high(uint ch, bool high);
bool trigger_set_active_high(uint ch, bool high);

const char *trigger_edge_name(trigger_edge_mode_t mode);
const char *trigger_pull_name(trigger_input_pull_t pull);
const char *trigger_level_name(bool high);

#endif
