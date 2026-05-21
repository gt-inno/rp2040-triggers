#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "trigger.h"

typedef struct {
    bool has_record;
    uint32_t sequence;
    uint32_t valid_records;
    uint32_t system_clock_khz;
    uint32_t flash_offset;
    uint32_t storage_bytes;
} settings_info_t;

bool settings_load(
    trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT],
    uint32_t *system_clock_khz
);
bool settings_save(
    const trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT],
    uint32_t system_clock_khz
);
bool settings_clear(void);
bool settings_get_info(settings_info_t *info);

#endif
