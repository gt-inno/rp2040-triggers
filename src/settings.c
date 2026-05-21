#include "settings.h"

#include <stddef.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "system_clock.h"

#ifndef PICO_FLASH_SIZE_BYTES
#error PICO_FLASH_SIZE_BYTES must be defined to place persistent settings safely.
#endif

#define SETTINGS_MAGIC 0x43524754u
#define SETTINGS_VERSION 3u
#define SETTINGS_MIN_SUPPORTED_VERSION 1u
#define SETTINGS_FLASH_SECTOR_COUNT 2u
#define SETTINGS_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - (SETTINGS_FLASH_SECTOR_COUNT * FLASH_SECTOR_SIZE))
#define SETTINGS_STORAGE_BYTES (SETTINGS_FLASH_SECTOR_COUNT * FLASH_SECTOR_SIZE)
#define SETTINGS_PAGE_COUNT (SETTINGS_STORAGE_BYTES / FLASH_PAGE_SIZE)
#define SETTINGS_NO_PAGE 0xffffffffu

typedef struct {
    uint32_t input_gpio;
    uint32_t output_gpio;
    uint32_t enabled;
    uint32_t edge_mode;
    uint32_t input_pull;
    uint32_t delay_us;
    uint32_t width_us;
    uint32_t idle_high;
    uint32_t active_high;
} settings_channel_record_v2_t;

typedef struct {
    uint32_t input_gpio;
    uint32_t output_gpio;
    uint32_t enabled;
    uint32_t edge_mode;
    uint32_t input_pull;
    uint32_t delay_us;
    uint32_t width_us;
    uint32_t idle_high;
    uint32_t active_high;
    uint32_t trigger_mode;
    uint32_t edge_count_target;
    uint32_t pulse_width_edges;
} settings_channel_record_t;

#define SETTINGS_HEADER_BYTES (5u * sizeof(uint32_t))
#define SETTINGS_CHANNEL_BYTES (TRIGGER_CHANNEL_COUNT * sizeof(settings_channel_record_t))
#define SETTINGS_CLOCK_BYTES (sizeof(uint32_t))
#define SETTINGS_RESERVED_BYTES (FLASH_PAGE_SIZE - SETTINGS_HEADER_BYTES - SETTINGS_CHANNEL_BYTES - SETTINGS_CLOCK_BYTES)
#define SETTINGS_V2_CHANNEL_BYTES (TRIGGER_CHANNEL_COUNT * sizeof(settings_channel_record_v2_t))
#define SETTINGS_V2_RESERVED_BYTES (FLASH_PAGE_SIZE - SETTINGS_HEADER_BYTES - SETTINGS_V2_CHANNEL_BYTES - SETTINGS_CLOCK_BYTES)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t size;
    uint32_t crc32;
    settings_channel_record_t channels[TRIGGER_CHANNEL_COUNT];
    uint32_t system_clock_khz;
    uint8_t reserved[SETTINGS_RESERVED_BYTES];
} settings_record_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t size;
    uint32_t crc32;
    settings_channel_record_v2_t channels[TRIGGER_CHANNEL_COUNT];
    uint32_t system_clock_khz;
    uint8_t reserved[SETTINGS_V2_RESERVED_BYTES];
} settings_record_v2_t;

typedef struct {
    const settings_record_t *latest;
    uint32_t latest_page;
    uint32_t first_empty_page;
    uint32_t valid_records;
} settings_scan_t;

_Static_assert(sizeof(settings_record_t) == FLASH_PAGE_SIZE, "settings record must be one flash page");
_Static_assert(sizeof(settings_record_v2_t) == FLASH_PAGE_SIZE, "v2 settings record must be one flash page");
_Static_assert(SETTINGS_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0, "settings offset must be sector aligned");

static const settings_record_t *record_at_page(uint32_t page) {
    return (const settings_record_t *)(XIP_BASE + SETTINGS_FLASH_OFFSET + (page * FLASH_PAGE_SIZE));
}

static bool record_is_empty(const settings_record_t *record) {
    const uint32_t *words = (const uint32_t *)record;

    for (size_t i = 0; i < sizeof(settings_record_t) / sizeof(uint32_t); i++) {
        if (words[i] != 0xffffffffu) {
            return false;
        }
    }

    return true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    while (len--) {
        crc ^= *data++;
        for (uint bit = 0; bit < 8; bit++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xedb88320u;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc;
}

static uint32_t record_crc(const settings_record_t *record) {
    uint32_t crc = 0xffffffffu;

    crc = crc32_update(crc, (const uint8_t *)record, offsetof(settings_record_t, crc32));
    crc = crc32_update(
        crc,
        (const uint8_t *)&record->channels,
        sizeof(settings_record_t) - offsetof(settings_record_t, channels)
    );

    return crc ^ 0xffffffffu;
}

static bool sequence_is_newer(uint32_t candidate, uint32_t current) {
    return (int32_t)(candidate - current) > 0;
}

static uint32_t record_system_clock_khz(const settings_record_t *record) {
    if (record->version >= 3u) {
        return record->system_clock_khz ? record->system_clock_khz : system_clock_default_khz();
    }

    if (record->version >= 2u) {
        const settings_record_v2_t *legacy = (const settings_record_v2_t *)record;
        return legacy->system_clock_khz ? legacy->system_clock_khz : system_clock_default_khz();
    }

    return system_clock_default_khz();
}

static bool record_to_configs(
    const settings_record_t *record,
    trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT],
    uint32_t *system_clock_khz
) {
    uint32_t saved_clock_khz = record_system_clock_khz(record);

    if (!system_clock_khz_is_valid(saved_clock_khz)) {
        return false;
    }

    if (record->version >= 3u) {
        for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
            configs[ch].input_gpio = record->channels[ch].input_gpio;
            configs[ch].output_gpio = record->channels[ch].output_gpio;
            configs[ch].enabled = record->channels[ch].enabled != 0;
            configs[ch].edge_mode = (trigger_edge_mode_t)record->channels[ch].edge_mode;
            configs[ch].input_pull = (trigger_input_pull_t)record->channels[ch].input_pull;
            configs[ch].delay_us = record->channels[ch].delay_us;
            configs[ch].width_us = record->channels[ch].width_us;
            configs[ch].idle_high = record->channels[ch].idle_high != 0;
            configs[ch].active_high = record->channels[ch].active_high != 0;
            configs[ch].trigger_mode = (trigger_mode_t)record->channels[ch].trigger_mode;
            configs[ch].edge_count_target = record->channels[ch].edge_count_target;
            configs[ch].pulse_width_edges = record->channels[ch].pulse_width_edges;
        }
    } else {
        const settings_record_v2_t *legacy = (const settings_record_v2_t *)record;
        for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
            configs[ch].input_gpio = legacy->channels[ch].input_gpio;
            configs[ch].output_gpio = legacy->channels[ch].output_gpio;
            configs[ch].enabled = legacy->channels[ch].enabled != 0;
            configs[ch].edge_mode = (trigger_edge_mode_t)legacy->channels[ch].edge_mode;
            configs[ch].input_pull = (trigger_input_pull_t)legacy->channels[ch].input_pull;
            configs[ch].delay_us = legacy->channels[ch].delay_us;
            configs[ch].width_us = legacy->channels[ch].width_us;
            configs[ch].idle_high = legacy->channels[ch].idle_high != 0;
            configs[ch].active_high = legacy->channels[ch].active_high != 0;
            configs[ch].trigger_mode = TRIGGER_MODE_TIME;
            configs[ch].edge_count_target = TRIGGER_EDGE_COUNT_DEFAULT;
            configs[ch].pulse_width_edges = TRIGGER_PULSE_WIDTH_EDGES_DEFAULT;
        }
    }

    if (!trigger_config_validate_all(configs)) {
        return false;
    }

    if (system_clock_khz) {
        *system_clock_khz = saved_clock_khz;
    }

    return true;
}

static bool record_is_valid(const settings_record_t *record) {
    trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT];

    if (record->magic != SETTINGS_MAGIC ||
        record->version < SETTINGS_MIN_SUPPORTED_VERSION ||
        record->version > SETTINGS_VERSION ||
        record->size != sizeof(settings_record_t) ||
        record->crc32 != record_crc(record)) {
        return false;
    }

    return record_to_configs(record, configs, NULL);
}

static void scan_records(settings_scan_t *scan) {
    scan->latest = NULL;
    scan->latest_page = SETTINGS_NO_PAGE;
    scan->first_empty_page = SETTINGS_NO_PAGE;
    scan->valid_records = 0;

    for (uint32_t page = 0; page < SETTINGS_PAGE_COUNT; page++) {
        const settings_record_t *record = record_at_page(page);

        if (record_is_empty(record)) {
            if (scan->first_empty_page == SETTINGS_NO_PAGE) {
                scan->first_empty_page = page;
            }
            continue;
        }

        if (!record_is_valid(record)) {
            continue;
        }

        scan->valid_records++;
        if (!scan->latest || sequence_is_newer(record->sequence, scan->latest->sequence)) {
            scan->latest = record;
            scan->latest_page = page;
        }
    }
}

static uint32_t page_flash_offset(uint32_t page) {
    return SETTINGS_FLASH_OFFSET + (page * FLASH_PAGE_SIZE);
}

static uint32_t sector_flash_offset(uint32_t sector) {
    return SETTINGS_FLASH_OFFSET + (sector * FLASH_SECTOR_SIZE);
}

static uint32_t page_sector(uint32_t page) {
    return (page * FLASH_PAGE_SIZE) / FLASH_SECTOR_SIZE;
}

static bool choose_write_page(const settings_scan_t *scan, uint32_t *page, bool *erase_first) {
    if (scan->first_empty_page != SETTINGS_NO_PAGE) {
        *page = scan->first_empty_page;
        *erase_first = false;
        return true;
    }

    uint32_t sector = 0;
    if (scan->latest_page != SETTINGS_NO_PAGE) {
        sector = page_sector(scan->latest_page) == 0 ? 1 : 0;
    }

    *page = (sector * FLASH_SECTOR_SIZE) / FLASH_PAGE_SIZE;
    *erase_first = true;
    return true;
}

static void fill_record(
    settings_record_t *record,
    const trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT],
    uint32_t system_clock_khz,
    uint32_t sequence
) {
    memset(record, 0, sizeof(*record));
    record->magic = SETTINGS_MAGIC;
    record->version = SETTINGS_VERSION;
    record->sequence = sequence;
    record->size = sizeof(*record);
    record->system_clock_khz = system_clock_khz;

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        record->channels[ch].input_gpio = configs[ch].input_gpio;
        record->channels[ch].output_gpio = configs[ch].output_gpio;
        record->channels[ch].enabled = configs[ch].enabled ? 1u : 0u;
        record->channels[ch].edge_mode = (uint32_t)configs[ch].edge_mode;
        record->channels[ch].input_pull = (uint32_t)configs[ch].input_pull;
        record->channels[ch].delay_us = configs[ch].delay_us;
        record->channels[ch].width_us = configs[ch].width_us;
        record->channels[ch].idle_high = configs[ch].idle_high ? 1u : 0u;
        record->channels[ch].active_high = configs[ch].active_high ? 1u : 0u;
        record->channels[ch].trigger_mode = (uint32_t)configs[ch].trigger_mode;
        record->channels[ch].edge_count_target = configs[ch].edge_count_target;
        record->channels[ch].pulse_width_edges = configs[ch].pulse_width_edges;
    }

    record->crc32 = record_crc(record);
}

bool settings_load(
    trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT],
    uint32_t *system_clock_khz
) {
    settings_scan_t scan;

    if (!configs) {
        return false;
    }

    scan_records(&scan);
    if (!scan.latest) {
        return false;
    }

    return record_to_configs(scan.latest, configs, system_clock_khz);
}

bool settings_save(
    const trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT],
    uint32_t system_clock_khz
) {
    settings_scan_t scan;
    uint32_t write_page;
    bool erase_first;
    settings_record_t record;

    if (!trigger_config_validate_all(configs)) {
        return false;
    }

    if (!system_clock_khz_is_valid(system_clock_khz)) {
        return false;
    }

    scan_records(&scan);
    if (!choose_write_page(&scan, &write_page, &erase_first)) {
        return false;
    }

    fill_record(
        &record,
        configs,
        system_clock_khz,
        scan.latest ? scan.latest->sequence + 1u : 1u
    );

    uint32_t irq_state = save_and_disable_interrupts();
    if (erase_first) {
        flash_range_erase(sector_flash_offset(page_sector(write_page)), FLASH_SECTOR_SIZE);
    }
    flash_range_program(page_flash_offset(write_page), (const uint8_t *)&record, sizeof(record));
    restore_interrupts(irq_state);

    return record_is_valid(record_at_page(write_page));
}

bool settings_clear(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    flash_range_erase(SETTINGS_FLASH_OFFSET, SETTINGS_STORAGE_BYTES);
    restore_interrupts(irq_state);
    return true;
}

bool settings_get_info(settings_info_t *info) {
    settings_scan_t scan;

    if (!info) {
        return false;
    }

    scan_records(&scan);
    info->has_record = scan.latest != NULL;
    info->sequence = scan.latest ? scan.latest->sequence : 0;
    info->valid_records = scan.valid_records;
    info->system_clock_khz = scan.latest ? record_system_clock_khz(scan.latest) : system_clock_default_khz();
    info->flash_offset = SETTINGS_FLASH_OFFSET;
    info->storage_bytes = SETTINGS_STORAGE_BYTES;
    return true;
}
