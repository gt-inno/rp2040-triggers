#include "trigger.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "status_led.h"

#define RP2040_GPIO_COUNT 30
#define EDGE_DIAG_SAMPLE_US 20u
#define EDGE_DIAG_FALLBACK_PERIOD_NS 1000u

typedef enum {
    EDGE_COUNTER_IDLE = 0,
    EDGE_COUNTER_WAIT_TARGET = 1,
    EDGE_COUNTER_ACTIVE = 2
} edge_counter_state_t;

typedef struct {
    uint input_gpio;
    uint output_gpio;

    volatile bool enabled;
    volatile bool pending;

    volatile uint32_t event_count;
    volatile uint64_t last_event_us;

    trigger_edge_mode_t edge_mode;
    trigger_input_pull_t input_pull;

    uint32_t delay_us;
    uint32_t width_us;

    trigger_mode_t trigger_mode;
    uint32_t edge_count_target;
    uint32_t pulse_width_edges;
    bool auto_clear_edges;
    uint32_t auto_clear_delay_ns;
    bool step_reduce_enabled;
    uint32_t step_reduce_every;
    uint32_t step_reduce_edge_delta;
    uint32_t step_reduce_delay_ns;
    volatile uint32_t step_reduce_count;
    volatile uint32_t current_edge_count_target;
    volatile uint32_t current_delay_ns;
    uint32_t delay_remainder_ns;
    volatile uint32_t edge_count_seen;
    volatile edge_counter_state_t edge_counter_state;
    uint pwm_slice;

    bool idle_high;
    bool active_high;

    alarm_id_t start_alarm;
    alarm_id_t end_alarm;
} trigger_channel_t;

typedef struct {
    bool active;
    uint channel;
    uint input_gpio;
    uint pwm_slice;
    volatile uint64_t wrap_edges;
    uint64_t start_us;
} input_monitor_t;

typedef enum {
    EDGE_DIAG_IDLE = 0,
    EDGE_DIAG_WAIT_FIRE = 1,
    EDGE_DIAG_PULSE_ACTIVE = 2,
    EDGE_DIAG_COUNT_AFTER = 3
} edge_diag_phase_t;

typedef struct {
    bool configured;
    bool active;
    bool result_pending;
    bool sweep_mode;
    bool sweep_complete;
    bool fired;
    bool first_edge_seen;
    bool output_active;
    uint channel;
    uint input_gpio;
    uint output_gpio;
    uint pwm_slice;
    uint32_t run_index;
    uint32_t sweep_start_edge;
    uint32_t sweep_stop_edge;
    uint32_t sweep_step;
    uint32_t current_target_edge;
    uint32_t fire_after_edges;
    uint32_t pulse_width_edges;
    uint32_t idle_gap_us;
    uint32_t trigger_off_edge;
    volatile edge_diag_phase_t phase;
    volatile uint64_t base_edges;
    volatile uint64_t last_polled_edges;
    uint64_t sample_last_edges;
    uint64_t sample_last_us;
    uint64_t total_edges;
    uint64_t first_us;
    uint64_t trigger_on_us;
    uint64_t trigger_off_us;
    uint64_t last_edge_us;
    uint64_t transaction_duration_us;
    uint32_t avg_swclk_period_ns;
    uint32_t min_period_ns;
    uint32_t max_period_ns;
    alarm_id_t idle_alarm;
    alarm_id_t output_off_alarm;
    trigger_diag_status_t pending_result;
} edge_diag_t;

#define DEFAULT_TRIGGER_CHANNEL(input, output) \
    {input, output, true, TRIGGER_EDGE_RISING, TRIGGER_PULL_DOWN, 0, 100, TRIGGER_MODE_TIME, \
     TRIGGER_EDGE_COUNT_DEFAULT, TRIGGER_PULSE_WIDTH_EDGES_DEFAULT, \
     TRIGGER_AUTO_CLEAR_EDGES_DEFAULT, TRIGGER_AUTO_CLEAR_DELAY_NS_DEFAULT, \
     TRIGGER_STEP_REDUCE_ENABLED_DEFAULT, TRIGGER_STEP_REDUCE_EVERY_DEFAULT, \
     TRIGGER_STEP_REDUCE_EDGE_DELTA_DEFAULT, TRIGGER_STEP_REDUCE_DELAY_NS_DEFAULT, false, true}

static const trigger_channel_config_t default_configs[TRIGGER_CHANNEL_COUNT] = {
    DEFAULT_TRIGGER_CHANNEL(2, 10),
    DEFAULT_TRIGGER_CHANNEL(3, 11),
    DEFAULT_TRIGGER_CHANNEL(4, 12),
    DEFAULT_TRIGGER_CHANNEL(5, 13),
};

static trigger_channel_t channels[TRIGGER_CHANNEL_COUNT] = {
    {2, 10, true, false, 0, 0, TRIGGER_EDGE_RISING, TRIGGER_PULL_DOWN, 0, 100, TRIGGER_MODE_TIME, TRIGGER_EDGE_COUNT_DEFAULT, TRIGGER_PULSE_WIDTH_EDGES_DEFAULT, TRIGGER_AUTO_CLEAR_EDGES_DEFAULT, TRIGGER_AUTO_CLEAR_DELAY_NS_DEFAULT, TRIGGER_STEP_REDUCE_ENABLED_DEFAULT, TRIGGER_STEP_REDUCE_EVERY_DEFAULT, TRIGGER_STEP_REDUCE_EDGE_DELTA_DEFAULT, TRIGGER_STEP_REDUCE_DELAY_NS_DEFAULT, 0, TRIGGER_EDGE_COUNT_DEFAULT, 0, 0, 0, EDGE_COUNTER_IDLE, 0, false, true, 0, 0},
    {3, 11, true, false, 0, 0, TRIGGER_EDGE_RISING, TRIGGER_PULL_DOWN, 0, 100, TRIGGER_MODE_TIME, TRIGGER_EDGE_COUNT_DEFAULT, TRIGGER_PULSE_WIDTH_EDGES_DEFAULT, TRIGGER_AUTO_CLEAR_EDGES_DEFAULT, TRIGGER_AUTO_CLEAR_DELAY_NS_DEFAULT, TRIGGER_STEP_REDUCE_ENABLED_DEFAULT, TRIGGER_STEP_REDUCE_EVERY_DEFAULT, TRIGGER_STEP_REDUCE_EDGE_DELTA_DEFAULT, TRIGGER_STEP_REDUCE_DELAY_NS_DEFAULT, 0, TRIGGER_EDGE_COUNT_DEFAULT, 0, 0, 0, EDGE_COUNTER_IDLE, 0, false, true, 0, 0},
    {4, 12, true, false, 0, 0, TRIGGER_EDGE_RISING, TRIGGER_PULL_DOWN, 0, 100, TRIGGER_MODE_TIME, TRIGGER_EDGE_COUNT_DEFAULT, TRIGGER_PULSE_WIDTH_EDGES_DEFAULT, TRIGGER_AUTO_CLEAR_EDGES_DEFAULT, TRIGGER_AUTO_CLEAR_DELAY_NS_DEFAULT, TRIGGER_STEP_REDUCE_ENABLED_DEFAULT, TRIGGER_STEP_REDUCE_EVERY_DEFAULT, TRIGGER_STEP_REDUCE_EDGE_DELTA_DEFAULT, TRIGGER_STEP_REDUCE_DELAY_NS_DEFAULT, 0, TRIGGER_EDGE_COUNT_DEFAULT, 0, 0, 0, EDGE_COUNTER_IDLE, 0, false, true, 0, 0},
    {5, 13, true, false, 0, 0, TRIGGER_EDGE_RISING, TRIGGER_PULL_DOWN, 0, 100, TRIGGER_MODE_TIME, TRIGGER_EDGE_COUNT_DEFAULT, TRIGGER_PULSE_WIDTH_EDGES_DEFAULT, TRIGGER_AUTO_CLEAR_EDGES_DEFAULT, TRIGGER_AUTO_CLEAR_DELAY_NS_DEFAULT, TRIGGER_STEP_REDUCE_ENABLED_DEFAULT, TRIGGER_STEP_REDUCE_EVERY_DEFAULT, TRIGGER_STEP_REDUCE_EDGE_DELTA_DEFAULT, TRIGGER_STEP_REDUCE_DELAY_NS_DEFAULT, 0, TRIGGER_EDGE_COUNT_DEFAULT, 0, 0, 0, EDGE_COUNTER_IDLE, 0, false, true, 0, 0},
};

static input_monitor_t input_monitor = {
    false, TRIGGER_CHANNEL_COUNT, 0, 0, 0, 0
};

static edge_diag_t edge_diag = {
    .configured = true,
    .active = false,
    .result_pending = false,
    .sweep_mode = false,
    .sweep_complete = false,
    .fired = false,
    .first_edge_seen = false,
    .output_active = false,
    .channel = 0,
    .input_gpio = 3,
    .output_gpio = 10,
    .pwm_slice = 0,
    .run_index = 0,
    .sweep_start_edge = TRIGGER_DIAG_SWEEP_START_EDGE_DEFAULT,
    .sweep_stop_edge = TRIGGER_DIAG_SWEEP_STOP_EDGE_DEFAULT,
    .sweep_step = TRIGGER_DIAG_SWEEP_STEP_DEFAULT,
    .current_target_edge = TRIGGER_DIAG_FIRE_AFTER_EDGES_DEFAULT,
    .fire_after_edges = TRIGGER_DIAG_FIRE_AFTER_EDGES_DEFAULT,
    .pulse_width_edges = TRIGGER_DIAG_PULSE_WIDTH_EDGES_DEFAULT,
    .idle_gap_us = TRIGGER_DIAG_IDLE_GAP_US_DEFAULT,
    .trigger_off_edge = 0,
    .phase = EDGE_DIAG_IDLE,
    .base_edges = 0,
    .last_polled_edges = 0,
    .sample_last_edges = 0,
    .sample_last_us = 0,
    .total_edges = 0,
    .first_us = 0,
    .trigger_on_us = 0,
    .trigger_off_us = 0,
    .last_edge_us = 0,
    .transaction_duration_us = 0,
    .avg_swclk_period_ns = 0,
    .min_period_ns = 0,
    .max_period_ns = 0,
    .idle_alarm = 0,
    .output_off_alarm = 0,
};

static volatile bool armed = false;
static bool initialized = false;

static void set_output_idle(uint ch);
static void set_output_active(uint ch);
static void enable_channel_irqs(void);
static void clear_pending_outputs(void);
static void handle_edge_diag_pwm_wrap_locked(uint slice);
static int64_t edge_diag_output_off_alarm(alarm_id_t id, void *user_data);

static bool valid_channel(uint ch) {
    return ch < TRIGGER_CHANNEL_COUNT;
}

static bool valid_gpio_pin(uint gpio) {
    if (gpio >= RP2040_GPIO_COUNT) {
        return false;
    }

#ifdef PICO_DEFAULT_LED_PIN
    if (gpio == PICO_DEFAULT_LED_PIN) {
        return false;
    }
#endif

    return true;
}

static bool edge_count_gpio_allowed(uint gpio) {
    return valid_gpio_pin(gpio) && pwm_gpio_to_channel(gpio) == PWM_CHAN_B;
}

static bool gpio_used_by_other_channel(uint gpio, uint ignore_ch) {
    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        if (ch == ignore_ch) {
            continue;
        }

        if (channels[ch].input_gpio == gpio || channels[ch].output_gpio == gpio) {
            return true;
        }
    }

    return false;
}

static bool edge_count_slice_used_by_other_channel(uint gpio, uint ignore_ch) {
    uint slice = pwm_gpio_to_slice_num(gpio);

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        if (ch == ignore_ch || channels[ch].trigger_mode != TRIGGER_MODE_EDGE_COUNT) {
            continue;
        }

        if (pwm_gpio_to_slice_num(channels[ch].input_gpio) == slice) {
            return true;
        }
    }

    return false;
}

static bool input_gpio_allowed(uint ch, uint gpio) {
    if (!valid_gpio_pin(gpio) ||
        channels[ch].output_gpio == gpio ||
        gpio_used_by_other_channel(gpio, ch)) {
        return false;
    }

    if (channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT) {
        return edge_count_gpio_allowed(gpio) &&
               !edge_count_slice_used_by_other_channel(gpio, ch);
    }

    return true;
}

static bool output_gpio_allowed(uint ch, uint gpio) {
    return valid_gpio_pin(gpio) &&
           channels[ch].input_gpio != gpio &&
           !gpio_used_by_other_channel(gpio, ch);
}

static void busy_wait_ns(uint32_t delay_ns) {
    if (delay_ns == 0) {
        return;
    }

    uint64_t cycles = ((uint64_t)clock_get_hz(clk_sys) * delay_ns + 999999999ull) /
                      1000000000ull;
    if (cycles == 0) {
        cycles = 1;
    }
    if (cycles > UINT32_MAX) {
        cycles = UINT32_MAX;
    }

    busy_wait_at_least_cycles((uint32_t)cycles);
}

static uint32_t delay_us_to_ns_saturated(uint32_t delay_us) {
    if (delay_us > UINT32_MAX / 1000u) {
        return UINT32_MAX;
    }

    return delay_us * 1000u;
}

static void reset_step_reduce_runtime(uint ch) {
    channels[ch].step_reduce_count = 0;
    channels[ch].current_edge_count_target = channels[ch].edge_count_target;
    channels[ch].current_delay_ns = delay_us_to_ns_saturated(channels[ch].delay_us);
    channels[ch].delay_remainder_ns = 0;
}

static uint32_t active_edge_count_target(uint ch) {
    uint32_t target = channels[ch].current_edge_count_target;
    if (target == 0 || target > TRIGGER_EDGE_COUNT_MAX) {
        target = channels[ch].edge_count_target;
    }

    return target;
}

static uint32_t active_delay_ns(uint ch) {
    if (channels[ch].step_reduce_enabled) {
        return channels[ch].current_delay_ns;
    }

    return delay_us_to_ns_saturated(channels[ch].delay_us);
}

static void note_channel_trigger_complete(uint ch) {
    if (!valid_channel(ch) ||
        !channels[ch].step_reduce_enabled ||
        channels[ch].step_reduce_every == 0) {
        return;
    }

    channels[ch].step_reduce_count++;
    if ((channels[ch].step_reduce_count % channels[ch].step_reduce_every) != 0) {
        return;
    }

    if (channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT &&
        channels[ch].step_reduce_edge_delta > 0) {
        uint32_t target = active_edge_count_target(ch);
        if (target > channels[ch].step_reduce_edge_delta) {
            channels[ch].current_edge_count_target = target - channels[ch].step_reduce_edge_delta;
        } else {
            channels[ch].current_edge_count_target = 1;
        }
        return;
    }

    if (channels[ch].trigger_mode == TRIGGER_MODE_TIME &&
        channels[ch].step_reduce_delay_ns > 0) {
        uint32_t delay_ns = channels[ch].current_delay_ns;
        if (delay_ns > channels[ch].step_reduce_delay_ns) {
            channels[ch].current_delay_ns = delay_ns - channels[ch].step_reduce_delay_ns;
        } else {
            channels[ch].current_delay_ns = 0;
        }
    }
}

static void apply_config_values(const trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT]) {
    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        channels[ch].input_gpio = configs[ch].input_gpio;
        channels[ch].output_gpio = configs[ch].output_gpio;
        channels[ch].enabled = configs[ch].enabled;
        channels[ch].pending = false;
        channels[ch].edge_mode = configs[ch].edge_mode;
        channels[ch].input_pull = configs[ch].input_pull;
        channels[ch].delay_us = configs[ch].delay_us;
        channels[ch].width_us = configs[ch].width_us;
        channels[ch].trigger_mode = configs[ch].trigger_mode;
        channels[ch].edge_count_target = configs[ch].edge_count_target;
        channels[ch].pulse_width_edges = configs[ch].pulse_width_edges;
        channels[ch].auto_clear_edges = configs[ch].auto_clear_edges;
        channels[ch].auto_clear_delay_ns = configs[ch].auto_clear_delay_ns;
        channels[ch].step_reduce_enabled = configs[ch].step_reduce_enabled;
        channels[ch].step_reduce_every = configs[ch].step_reduce_every;
        channels[ch].step_reduce_edge_delta = configs[ch].step_reduce_edge_delta;
        channels[ch].step_reduce_delay_ns = configs[ch].step_reduce_delay_ns;
        reset_step_reduce_runtime(ch);
        channels[ch].edge_count_seen = 0;
        channels[ch].edge_counter_state = EDGE_COUNTER_IDLE;
        channels[ch].pwm_slice = pwm_gpio_to_slice_num(configs[ch].input_gpio);
        channels[ch].idle_high = configs[ch].idle_high;
        channels[ch].active_high = configs[ch].active_high;
        channels[ch].start_alarm = 0;
        channels[ch].end_alarm = 0;
    }
}

static void apply_input_pull(uint ch) {
    if (channels[ch].input_pull == TRIGGER_PULL_UP) {
        gpio_pull_up(channels[ch].input_gpio);
    } else if (channels[ch].input_pull == TRIGGER_PULL_DOWN) {
        gpio_pull_down(channels[ch].input_gpio);
    } else {
        gpio_disable_pulls(channels[ch].input_gpio);
    }
}

static void configure_input_gpio(uint ch) {
    gpio_init(channels[ch].input_gpio);
    gpio_set_dir(channels[ch].input_gpio, GPIO_IN);
    apply_input_pull(ch);
}

static void configure_edge_count_input(uint ch) {
    uint slice = pwm_gpio_to_slice_num(channels[ch].input_gpio);
    pwm_config config = pwm_get_default_config();

    channels[ch].pwm_slice = slice;
    channels[ch].edge_count_seen = 0;
    channels[ch].edge_counter_state = EDGE_COUNTER_IDLE;

    gpio_init(channels[ch].input_gpio);
    gpio_set_dir(channels[ch].input_gpio, GPIO_IN);
    apply_input_pull(ch);
    gpio_set_function(channels[ch].input_gpio, GPIO_FUNC_PWM);

    pwm_set_enabled(slice, false);
    pwm_set_irq_enabled(slice, false);
    pwm_clear_irq(slice);
    pwm_config_set_clkdiv_mode(&config, PWM_DIV_B_RISING);
    pwm_config_set_clkdiv_int(&config, 1);
    pwm_config_set_wrap(&config, (uint16_t)(active_edge_count_target(ch) - 1u));
    pwm_init(slice, &config, false);
    pwm_set_counter(slice, 0);
}

static void configure_channel_input(uint ch) {
    if (channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT) {
        configure_edge_count_input(ch);
    } else {
        configure_input_gpio(ch);
    }
}

static void configure_output_gpio(uint ch) {
    gpio_init(channels[ch].output_gpio);
    gpio_set_dir(channels[ch].output_gpio, GPIO_OUT);
    gpio_set_pulls(channels[ch].output_gpio, false, false);
    gpio_set_drive_strength(channels[ch].output_gpio, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(channels[ch].output_gpio, GPIO_SLEW_RATE_FAST);
    set_output_idle(ch);
}

static void set_output_idle(uint ch) {
    gpio_put(channels[ch].output_gpio, channels[ch].idle_high ? 1 : 0);
}

static void set_output_active(uint ch) {
    gpio_put(channels[ch].output_gpio, channels[ch].active_high ? 1 : 0);
}

static int64_t end_pulse_alarm(alarm_id_t id, void *user_data) {
    (void)id;

    uint ch = (uint)(uintptr_t)user_data;
    set_output_idle(ch);
    channels[ch].pending = false;
    channels[ch].end_alarm = 0;
    note_channel_trigger_complete(ch);
    return 0;
}

static int64_t start_pulse_alarm(alarm_id_t id, void *user_data) {
    (void)id;

    uint ch = (uint)(uintptr_t)user_data;
    channels[ch].start_alarm = 0;
    if (channels[ch].delay_remainder_ns != 0) {
        busy_wait_ns(channels[ch].delay_remainder_ns);
        channels[ch].delay_remainder_ns = 0;
    }
    set_output_active(ch);

    if (channels[ch].width_us == 0) {
        set_output_idle(ch);
        channels[ch].pending = false;
        note_channel_trigger_complete(ch);
        return 0;
    }

    channels[ch].end_alarm = add_alarm_in_us(
        (int64_t)channels[ch].width_us,
        end_pulse_alarm,
        (void *)(uintptr_t)ch,
        true
    );

    return 0;
}

static bool gpio_event_matches(uint32_t events, trigger_edge_mode_t mode) {
    bool rising = (events & GPIO_IRQ_EDGE_RISE) != 0;
    bool falling = (events & GPIO_IRQ_EDGE_FALL) != 0;

    if (mode == TRIGGER_EDGE_RISING) {
        return rising;
    }

    if (mode == TRIGGER_EDGE_FALLING) {
        return falling;
    }

    return rising || falling;
}

static void schedule_channel_pulse(uint ch) {
    if (channels[ch].pending) {
        return;
    }

    channels[ch].pending = true;
    channels[ch].event_count++;
    channels[ch].last_event_us = time_us_64();
    status_led_blink_triggered();

    if (!channels[ch].step_reduce_enabled) {
        channels[ch].delay_remainder_ns = 0;
        if (channels[ch].delay_us == 0) {
            start_pulse_alarm(0, (void *)(uintptr_t)ch);
        } else {
            channels[ch].start_alarm = add_alarm_in_us(
                (int64_t)channels[ch].delay_us,
                start_pulse_alarm,
                (void *)(uintptr_t)ch,
                true
            );
        }
        return;
    }

    uint32_t delay_ns = active_delay_ns(ch);
    uint32_t delay_us = delay_ns / 1000u;
    channels[ch].delay_remainder_ns = delay_ns % 1000u;

    if (delay_us == 0) {
        start_pulse_alarm(0, (void *)(uintptr_t)ch);
    } else {
        channels[ch].start_alarm = add_alarm_in_us(
            (int64_t)delay_us,
            start_pulse_alarm,
            (void *)(uintptr_t)ch,
            true
        );
    }
}

static void stop_edge_counter(uint ch) {
    if (channels[ch].trigger_mode != TRIGGER_MODE_EDGE_COUNT) {
        return;
    }

    pwm_set_enabled(channels[ch].pwm_slice, false);
    pwm_set_irq_enabled(channels[ch].pwm_slice, false);
    pwm_clear_irq(channels[ch].pwm_slice);
    pwm_set_counter(channels[ch].pwm_slice, 0);
    channels[ch].edge_count_seen = 0;
    channels[ch].edge_counter_state = EDGE_COUNTER_IDLE;
}

static void start_edge_counter(uint ch) {
    if (channels[ch].trigger_mode != TRIGGER_MODE_EDGE_COUNT || !channels[ch].enabled) {
        return;
    }

    pwm_set_enabled(channels[ch].pwm_slice, false);
    pwm_clear_irq(channels[ch].pwm_slice);
    pwm_set_counter(channels[ch].pwm_slice, 0);
    pwm_set_wrap(channels[ch].pwm_slice, (uint16_t)(active_edge_count_target(ch) - 1u));
    channels[ch].edge_count_seen = 0;
    channels[ch].edge_counter_state = EDGE_COUNTER_WAIT_TARGET;
    pwm_set_irq_enabled(channels[ch].pwm_slice, true);
    pwm_set_enabled(channels[ch].pwm_slice, true);
}

static void restart_edge_counter_after_pulse(uint ch, uint slice) {
    pwm_set_enabled(slice, false);
    pwm_set_irq_enabled(slice, false);
    pwm_clear_irq(slice);
    if (channels[ch].auto_clear_edges) {
        busy_wait_ns(channels[ch].auto_clear_delay_ns);
    }
    pwm_set_counter(slice, 0);
    pwm_set_wrap(slice, (uint16_t)(active_edge_count_target(ch) - 1u));
    channels[ch].edge_count_seen = 0;
    channels[ch].edge_counter_state = EDGE_COUNTER_WAIT_TARGET;
    pwm_set_irq_enabled(slice, true);
    pwm_set_enabled(slice, true);
}

static uint edge_count_channel_for_slice(uint slice) {
    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        if (channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT &&
            channels[ch].pwm_slice == slice) {
            return ch;
        }
    }

    return TRIGGER_CHANNEL_COUNT;
}

static uint32_t edge_count_current(uint ch) {
    if (channels[ch].trigger_mode != TRIGGER_MODE_EDGE_COUNT) {
        return 0;
    }

    if (channels[ch].edge_counter_state == EDGE_COUNTER_WAIT_TARGET) {
        return pwm_get_counter(channels[ch].pwm_slice);
    }

    if (channels[ch].edge_counter_state == EDGE_COUNTER_ACTIVE) {
        return active_edge_count_target(ch) + pwm_get_counter(channels[ch].pwm_slice);
    }

    return channels[ch].edge_count_seen;
}

static void fill_monitor_status_locked(trigger_monitor_status_t *status) {
    uint64_t now_us = time_us_64();
    uint64_t elapsed_us = 0;
    uint64_t edges = 0;

    if (!status) {
        return;
    }

    status->active = input_monitor.active;
    status->channel = input_monitor.channel;
    status->input_gpio = input_monitor.input_gpio;

    if (input_monitor.active) {
        elapsed_us = now_us - input_monitor.start_us;
        edges = input_monitor.wrap_edges + pwm_get_counter(input_monitor.pwm_slice);
    }

    status->edges = edges;
    status->elapsed_us = elapsed_us;
    status->rate_hz = elapsed_us ? (edges * 1000000ull) / elapsed_us : 0;
    status->period_ns = edges ? (uint32_t)((elapsed_us * 1000ull) / edges) : 0;
}

static void stop_input_monitor_locked(trigger_monitor_status_t *status) {
    if (!input_monitor.active) {
        if (status) {
            fill_monitor_status_locked(status);
        }
        return;
    }

    uint ch = input_monitor.channel;
    fill_monitor_status_locked(status);
    pwm_set_enabled(input_monitor.pwm_slice, false);
    pwm_set_irq_enabled(input_monitor.pwm_slice, false);
    pwm_clear_irq(input_monitor.pwm_slice);
    pwm_set_counter(input_monitor.pwm_slice, 0);
    input_monitor.active = false;
    input_monitor.channel = TRIGGER_CHANNEL_COUNT;
    input_monitor.wrap_edges = 0;
    if (status) {
        status->active = false;
    }

    if (valid_channel(ch)) {
        gpio_set_function(channels[ch].input_gpio, GPIO_FUNC_SIO);
        configure_channel_input(ch);
        if (channels[ch].trigger_mode == TRIGGER_MODE_TIME) {
            gpio_set_irq_enabled(
                channels[ch].input_gpio,
                GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
                true
            );
        }
    }
}

static void configure_input_monitor_locked(uint ch) {
    uint slice = pwm_gpio_to_slice_num(channels[ch].input_gpio);
    pwm_config config = pwm_get_default_config();

    if (channels[ch].trigger_mode == TRIGGER_MODE_TIME) {
        gpio_set_irq_enabled(
            channels[ch].input_gpio,
            GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
            false
        );
    } else {
        stop_edge_counter(ch);
    }

    input_monitor.active = true;
    input_monitor.channel = ch;
    input_monitor.input_gpio = channels[ch].input_gpio;
    input_monitor.pwm_slice = slice;
    input_monitor.wrap_edges = 0;
    input_monitor.start_us = time_us_64();

    gpio_init(channels[ch].input_gpio);
    gpio_set_dir(channels[ch].input_gpio, GPIO_IN);
    apply_input_pull(ch);
    gpio_set_function(channels[ch].input_gpio, GPIO_FUNC_PWM);

    pwm_set_enabled(slice, false);
    pwm_set_irq_enabled(slice, false);
    pwm_clear_irq(slice);
    pwm_config_set_clkdiv_mode(&config, PWM_DIV_B_RISING);
    pwm_config_set_clkdiv_int(&config, 1);
    pwm_config_set_wrap(&config, 0xffffu);
    pwm_init(slice, &config, false);
    pwm_set_counter(slice, 0);
    pwm_set_irq_enabled(slice, true);
    pwm_set_enabled(slice, true);
}

static void disable_all_channel_inputs_locked(void) {
    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        if (channels[ch].trigger_mode == TRIGGER_MODE_TIME) {
            gpio_set_irq_enabled(
                channels[ch].input_gpio,
                GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
                false
            );
        } else {
            stop_edge_counter(ch);
        }
    }
}

static void restore_all_channel_gpios_locked(void) {
    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        gpio_set_function(channels[ch].input_gpio, GPIO_FUNC_SIO);
        configure_channel_input(ch);
        configure_output_gpio(ch);
    }

    enable_channel_irqs();
}

static bool edge_diag_gpio_allowed(uint input_gpio, uint output_gpio) {
    return edge_count_gpio_allowed(input_gpio) &&
           valid_gpio_pin(output_gpio) &&
           input_gpio != output_gpio;
}

static void set_edge_diag_output_idle(void) {
    if (!valid_channel(edge_diag.channel)) {
        gpio_put(edge_diag.output_gpio, 0);
        return;
    }

    gpio_put(edge_diag.output_gpio, channels[edge_diag.channel].idle_high ? 1 : 0);
}

static void set_edge_diag_output_active(void) {
    if (!valid_channel(edge_diag.channel)) {
        gpio_put(edge_diag.output_gpio, 1);
        return;
    }

    gpio_put(edge_diag.output_gpio, channels[edge_diag.channel].active_high ? 1 : 0);
}

static void release_edge_diag_gpios_locked(void) {
    gpio_set_function(edge_diag.input_gpio, GPIO_FUNC_SIO);
    gpio_set_dir(edge_diag.input_gpio, GPIO_IN);
    gpio_disable_pulls(edge_diag.input_gpio);

    gpio_set_function(edge_diag.output_gpio, GPIO_FUNC_SIO);
    gpio_put(edge_diag.output_gpio, 0);
    gpio_set_dir(edge_diag.output_gpio, GPIO_IN);
    gpio_disable_pulls(edge_diag.output_gpio);
}

static void service_edge_diag_pending_wrap_locked(void) {
    if (!edge_diag.active) {
        return;
    }

    uint32_t mask = pwm_get_irq_status_mask();
    if ((mask & (1u << edge_diag.pwm_slice)) == 0) {
        return;
    }

    pwm_clear_irq(edge_diag.pwm_slice);
    handle_edge_diag_pwm_wrap_locked(edge_diag.pwm_slice);
}

static uint64_t edge_diag_current_edges_locked(void) {
    if (!edge_diag.active) {
        return edge_diag.total_edges;
    }

    return edge_diag.base_edges + pwm_get_counter(edge_diag.pwm_slice);
}

static uint32_t edge_diag_alarm_interval_us(void) {
    if (edge_diag.idle_gap_us == 0 || edge_diag.idle_gap_us > EDGE_DIAG_SAMPLE_US) {
        return EDGE_DIAG_SAMPLE_US;
    }

    return edge_diag.idle_gap_us;
}

static void edge_diag_update_period_stats_locked(
    uint64_t previous_edges,
    uint64_t edges,
    uint64_t now_us
) {
    if (edge_diag.sample_last_us == 0 ||
        edges <= previous_edges ||
        now_us <= edge_diag.sample_last_us) {
        return;
    }

    uint64_t edge_delta = edges - previous_edges;
    uint64_t time_delta_us = now_us - edge_diag.sample_last_us;
    uint32_t period_ns = (uint32_t)((time_delta_us * 1000ull) / edge_delta);

    if (period_ns == 0) {
        period_ns = 1;
    }

    if (edges != edge_diag.last_polled_edges) {
        if (edge_diag.min_period_ns == 0 || period_ns < edge_diag.min_period_ns) {
            edge_diag.min_period_ns = period_ns;
        }

        if (period_ns > edge_diag.max_period_ns) {
            edge_diag.max_period_ns = period_ns;
        }
    }
}

static void edge_diag_note_edges_locked(uint64_t edges, uint64_t now_us) {
    if (edges == edge_diag.last_polled_edges) {
        return;
    }

    uint64_t previous_edges = edge_diag.last_polled_edges;

    if (!edge_diag.first_edge_seen && edges > 0) {
        edge_diag.first_edge_seen = true;
        edge_diag.first_us = now_us;
    }

    edge_diag_update_period_stats_locked(previous_edges, edges, now_us);
    edge_diag.sample_last_edges = edges;
    edge_diag.sample_last_us = now_us;
    edge_diag.last_edge_us = now_us;
    edge_diag.last_polled_edges = edges;
    edge_diag.total_edges = edges;
}

static uint64_t edge_diag_update_edges_locked(uint64_t now_us) {
    service_edge_diag_pending_wrap_locked();
    uint64_t edges = edge_diag_current_edges_locked();
    edge_diag_note_edges_locked(edges, now_us);

    return edges;
}

static void edge_diag_calculate_summary_locked(void) {
    edge_diag.transaction_duration_us = 0;
    edge_diag.avg_swclk_period_ns = 0;

    if (edge_diag.first_edge_seen && edge_diag.last_edge_us > edge_diag.first_us) {
        edge_diag.transaction_duration_us = edge_diag.last_edge_us - edge_diag.first_us;
    }

    if (edge_diag.transaction_duration_us > 0 && edge_diag.total_edges > 0) {
        edge_diag.avg_swclk_period_ns = (uint32_t)(
            (edge_diag.transaction_duration_us * 1000ull) / edge_diag.total_edges
        );
    }

    if (edge_diag.avg_swclk_period_ns == 0 && edge_diag.min_period_ns != 0) {
        edge_diag.avg_swclk_period_ns = edge_diag.min_period_ns;
    }

}

static void fill_edge_diag_status_locked(trigger_diag_status_t *status) {
    if (!status) {
        return;
    }

    if (edge_diag.active) {
        edge_diag_update_edges_locked(time_us_64());
    }

    edge_diag_calculate_summary_locked();
    status->valid = edge_diag.configured;
    status->active = edge_diag.active;
    status->sweep = edge_diag.sweep_mode;
    status->sweep_complete = edge_diag.sweep_complete;
    status->fired = edge_diag.fired;
    status->first_edge_seen = edge_diag.first_edge_seen;
    status->channel = edge_diag.channel;
    status->input_gpio = edge_diag.input_gpio;
    status->output_gpio = edge_diag.output_gpio;
    status->run_index = edge_diag.run_index;
    status->trigger_edge = edge_diag.fire_after_edges;
    status->fire_after_edges = edge_diag.fire_after_edges;
    status->pulse_width_edges = edge_diag.pulse_width_edges;
    status->idle_gap_us = edge_diag.idle_gap_us;
    status->total_edges = edge_diag.total_edges;
    status->first_us = edge_diag.first_us;
    status->trigger_on_us = edge_diag.trigger_on_us;
    status->trigger_off_us = edge_diag.trigger_off_us;
    status->last_edge_us = edge_diag.last_edge_us;
    status->transaction_duration_us = edge_diag.transaction_duration_us;
    uint32_t avg_period_ns = edge_diag.avg_swclk_period_ns;
    if (avg_period_ns == 0 && edge_diag.first_edge_seen) {
        avg_period_ns = EDGE_DIAG_FALLBACK_PERIOD_NS;
    }
    status->avg_swclk_period_ns = avg_period_ns;
    status->min_period_ns = edge_diag.min_period_ns != 0 ?
        edge_diag.min_period_ns :
        avg_period_ns;
    status->max_period_ns = edge_diag.max_period_ns != 0 ?
        edge_diag.max_period_ns :
        avg_period_ns;
}

static void edge_diag_store_pending_result_locked(void) {
    fill_edge_diag_status_locked(&edge_diag.pending_result);
    edge_diag.pending_result.active = false;
    edge_diag.result_pending = true;
}

static uint32_t edge_diag_estimated_period_ns_locked(void) {
    if (edge_diag.avg_swclk_period_ns != 0) {
        return edge_diag.avg_swclk_period_ns;
    }

    if (edge_diag.min_period_ns != 0) {
        return edge_diag.min_period_ns;
    }

    if (edge_diag.first_edge_seen &&
        edge_diag.trigger_on_us > edge_diag.first_us &&
        edge_diag.fire_after_edges > 0) {
        uint64_t elapsed_us = edge_diag.trigger_on_us - edge_diag.first_us;
        uint64_t period_ns = (elapsed_us * 1000ull) / edge_diag.fire_after_edges;
        return period_ns != 0 ? (uint32_t)period_ns : 1;
    }

    return EDGE_DIAG_FALLBACK_PERIOD_NS;
}

static void edge_diag_schedule_output_off_locked(void) {
    uint64_t delay_ns =
        (uint64_t)edge_diag_estimated_period_ns_locked() * edge_diag.pulse_width_edges;
    uint64_t delay_us = (delay_ns + 999ull) / 1000ull;

    if (delay_us == 0) {
        delay_us = 1;
    }

    if (edge_diag.output_off_alarm != 0) {
        cancel_alarm(edge_diag.output_off_alarm);
    }

    edge_diag.output_off_alarm = add_alarm_in_us(
        (int64_t)delay_us,
        edge_diag_output_off_alarm,
        NULL,
        true
    );
}

static void edge_diag_turn_output_off_locked(uint64_t now_us, bool timer_fallback) {
    if (!edge_diag.output_active) {
        return;
    }

    uint64_t current_edges = edge_diag_current_edges_locked();
    edge_diag_note_edges_locked(current_edges, now_us);
    edge_diag.trigger_off_us = now_us;
    edge_diag.trigger_off_edge = edge_diag.fire_after_edges + edge_diag.pulse_width_edges;
    edge_diag.output_active = false;
    set_edge_diag_output_idle();

    if (timer_fallback && edge_diag.phase == EDGE_DIAG_PULSE_ACTIVE) {
        pwm_set_enabled(edge_diag.pwm_slice, false);
        edge_diag.base_edges = current_edges;
        edge_diag.total_edges = current_edges;
        edge_diag.last_polled_edges = current_edges;
        pwm_set_counter(edge_diag.pwm_slice, 0);
        pwm_set_wrap(edge_diag.pwm_slice, 0xffffu);
        edge_diag.phase = EDGE_DIAG_COUNT_AFTER;
        pwm_set_enabled(edge_diag.pwm_slice, true);
    }
}

static int64_t edge_diag_output_off_alarm(alarm_id_t id, void *user_data) {
    (void)id;
    (void)user_data;

    uint32_t irq_state = save_and_disable_interrupts();
    edge_diag.output_off_alarm = 0;

    if (edge_diag.active) {
        uint32_t mask = pwm_get_irq_status_mask();
        if ((mask & (1u << edge_diag.pwm_slice)) != 0) {
            pwm_clear_irq(edge_diag.pwm_slice);
            handle_edge_diag_pwm_wrap_locked(edge_diag.pwm_slice);
            restore_interrupts(irq_state);
            return 0;
        }

        edge_diag_turn_output_off_locked(time_us_64(), true);
    }

    restore_interrupts(irq_state);
    return 0;
}

static void finish_edge_diag_locked(
    trigger_diag_status_t *status,
    bool make_pending,
    bool cancel_idle_alarm,
    bool force_output_off
) {
    if (!edge_diag.active) {
        fill_edge_diag_status_locked(status);
        return;
    }

    uint64_t now_us = time_us_64();
    edge_diag_update_edges_locked(now_us);
    edge_diag.total_edges = edge_diag_current_edges_locked();
    if (edge_diag.output_active && force_output_off) {
        edge_diag_turn_output_off_locked(now_us, true);
    }

    pwm_set_enabled(edge_diag.pwm_slice, false);
    pwm_set_irq_enabled(edge_diag.pwm_slice, false);
    pwm_clear_irq(edge_diag.pwm_slice);
    pwm_set_counter(edge_diag.pwm_slice, 0);

    if (cancel_idle_alarm && edge_diag.idle_alarm != 0) {
        cancel_alarm(edge_diag.idle_alarm);
    }
    edge_diag.idle_alarm = 0;
    if (edge_diag.output_off_alarm != 0) {
        cancel_alarm(edge_diag.output_off_alarm);
        edge_diag.output_off_alarm = 0;
    }
    edge_diag.active = false;
    edge_diag.phase = EDGE_DIAG_IDLE;
    if (make_pending) {
        edge_diag_store_pending_result_locked();
    } else {
        edge_diag.result_pending = false;
    }
    set_edge_diag_output_idle();
    release_edge_diag_gpios_locked();
    status_led_set_armed(false);
    restore_all_channel_gpios_locked();

    fill_edge_diag_status_locked(status);

    if (make_pending && edge_diag.sweep_mode && !edge_diag.sweep_complete) {
        uint32_t next_target = edge_diag.current_target_edge + edge_diag.sweep_step;
        if (next_target > edge_diag.sweep_stop_edge || next_target < edge_diag.current_target_edge) {
            edge_diag.sweep_complete = true;
        } else {
            edge_diag.current_target_edge = next_target;
            edge_diag.fire_after_edges = next_target;
            edge_diag.run_index++;
        }
    }
}

static int64_t edge_diag_idle_alarm(alarm_id_t id, void *user_data) {
    (void)id;
    (void)user_data;

    uint32_t irq_state = save_and_disable_interrupts();

    if (!edge_diag.active) {
        edge_diag.idle_alarm = 0;
        restore_interrupts(irq_state);
        return 0;
    }

    uint64_t now_us = time_us_64();
    edge_diag_update_edges_locked(now_us);

    if (edge_diag.first_edge_seen &&
        now_us - edge_diag.last_edge_us >= edge_diag.idle_gap_us) {
        if (edge_diag.output_active) {
            restore_interrupts(irq_state);
            return edge_diag_alarm_interval_us();
        }

        edge_diag.idle_alarm = 0;
        finish_edge_diag_locked(NULL, true, false, false);
        restore_interrupts(irq_state);
        return 0;
    }

    restore_interrupts(irq_state);
    return edge_diag_alarm_interval_us();
}

static void configure_edge_diag_io_locked(void) {
    pwm_config config = pwm_get_default_config();

    edge_diag.pwm_slice = pwm_gpio_to_slice_num(edge_diag.input_gpio);

    gpio_init(edge_diag.input_gpio);
    gpio_set_dir(edge_diag.input_gpio, GPIO_IN);
    gpio_disable_pulls(edge_diag.input_gpio);
    gpio_set_function(edge_diag.input_gpio, GPIO_FUNC_PWM);

    gpio_init(edge_diag.output_gpio);
    gpio_set_dir(edge_diag.output_gpio, GPIO_OUT);
    gpio_set_pulls(edge_diag.output_gpio, false, false);
    gpio_set_drive_strength(edge_diag.output_gpio, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_slew_rate(edge_diag.output_gpio, GPIO_SLEW_RATE_FAST);
    set_edge_diag_output_idle();

    pwm_set_enabled(edge_diag.pwm_slice, false);
    pwm_set_irq_enabled(edge_diag.pwm_slice, false);
    pwm_clear_irq(edge_diag.pwm_slice);
    pwm_config_set_clkdiv_mode(&config, PWM_DIV_B_RISING);
    pwm_config_set_clkdiv_int(&config, 1);
    pwm_config_set_wrap(&config, (uint16_t)(edge_diag.fire_after_edges - 1u));
    pwm_init(edge_diag.pwm_slice, &config, false);
    pwm_set_counter(edge_diag.pwm_slice, 0);
}

static void start_edge_diag_locked(void) {
    clear_pending_outputs();
    disable_all_channel_inputs_locked();
    configure_edge_diag_io_locked();

    edge_diag.active = true;
    edge_diag.result_pending = false;
    edge_diag.fired = false;
    edge_diag.first_edge_seen = false;
    edge_diag.output_active = false;
    edge_diag.phase = EDGE_DIAG_WAIT_FIRE;
    edge_diag.base_edges = 0;
    edge_diag.last_polled_edges = 0;
    edge_diag.sample_last_edges = 0;
    edge_diag.sample_last_us = 0;
    edge_diag.total_edges = 0;
    edge_diag.first_us = 0;
    edge_diag.trigger_on_us = 0;
    edge_diag.trigger_off_us = 0;
    edge_diag.last_edge_us = 0;
    edge_diag.transaction_duration_us = 0;
    edge_diag.avg_swclk_period_ns = 0;
    edge_diag.min_period_ns = 0;
    edge_diag.max_period_ns = 0;
    edge_diag.trigger_off_edge = 0;
    edge_diag.output_off_alarm = 0;

    pwm_set_counter(edge_diag.pwm_slice, 0);
    pwm_set_wrap(edge_diag.pwm_slice, (uint16_t)(edge_diag.fire_after_edges - 1u));
    pwm_set_irq_enabled(edge_diag.pwm_slice, true);
    pwm_set_enabled(edge_diag.pwm_slice, true);

    edge_diag.idle_alarm = add_alarm_in_us(
        (int64_t)edge_diag_alarm_interval_us(),
        edge_diag_idle_alarm,
        NULL,
        true
    );
    status_led_set_armed(true);
}

static void handle_edge_diag_pwm_wrap_locked(uint slice) {
    if (!edge_diag.active || edge_diag.pwm_slice != slice) {
        return;
    }

    uint64_t now_us = time_us_64();

    if (edge_diag.phase == EDGE_DIAG_WAIT_FIRE) {
        edge_diag.base_edges = edge_diag.fire_after_edges;
        edge_diag_note_edges_locked(edge_diag.base_edges, now_us);
        edge_diag.trigger_on_us = now_us;
        edge_diag.fired = true;
        edge_diag.output_active = true;
        status_led_blink_triggered();
        set_edge_diag_output_active();
        edge_diag_schedule_output_off_locked();

        pwm_set_enabled(slice, false);
        pwm_set_counter(slice, 0);
        pwm_set_wrap(slice, (uint16_t)(edge_diag.pulse_width_edges - 1u));
        edge_diag.phase = EDGE_DIAG_PULSE_ACTIVE;
        pwm_set_enabled(slice, true);
        return;
    }

    if (edge_diag.phase == EDGE_DIAG_PULSE_ACTIVE) {
        edge_diag.base_edges += edge_diag.pulse_width_edges;
        edge_diag_note_edges_locked(edge_diag.base_edges, now_us);
        if (edge_diag.output_off_alarm != 0) {
            cancel_alarm(edge_diag.output_off_alarm);
            edge_diag.output_off_alarm = 0;
        }
        edge_diag_turn_output_off_locked(now_us, false);

        pwm_set_enabled(slice, false);
        pwm_set_counter(slice, 0);
        pwm_set_wrap(slice, 0xffffu);
        edge_diag.phase = EDGE_DIAG_COUNT_AFTER;
        pwm_set_enabled(slice, true);
        return;
    }

    if (edge_diag.phase == EDGE_DIAG_COUNT_AFTER) {
        edge_diag.base_edges += 0x10000ull;
        edge_diag_note_edges_locked(edge_diag.base_edges, now_us);
    }
}

static void pwm_wrap_callback(void) {
    uint32_t mask = pwm_get_irq_status_mask();

    for (uint slice = 0; slice < NUM_PWM_SLICES; slice++) {
        if ((mask & (1u << slice)) == 0) {
            continue;
        }

        pwm_clear_irq(slice);
        if (edge_diag.active && edge_diag.pwm_slice == slice) {
            handle_edge_diag_pwm_wrap_locked(slice);
            continue;
        }

        if (input_monitor.active && input_monitor.pwm_slice == slice) {
            input_monitor.wrap_edges += 0x10000ull;
            continue;
        }

        uint ch = edge_count_channel_for_slice(slice);
        if (!valid_channel(ch)) {
            continue;
        }

        if (!armed || !channels[ch].enabled) {
            stop_edge_counter(ch);
            continue;
        }

        if (channels[ch].edge_counter_state == EDGE_COUNTER_WAIT_TARGET) {
            uint32_t target = active_edge_count_target(ch);
            channels[ch].edge_count_seen = target;
            channels[ch].pending = true;
            channels[ch].event_count++;
            channels[ch].last_event_us = time_us_64();
            status_led_blink_triggered();
            set_output_active(ch);

            pwm_set_enabled(slice, false);
            pwm_set_counter(slice, 0);
            pwm_set_wrap(slice, (uint16_t)(channels[ch].pulse_width_edges - 1u));
            channels[ch].edge_counter_state = EDGE_COUNTER_ACTIVE;
            pwm_set_enabled(slice, true);
            continue;
        }

        if (channels[ch].edge_counter_state == EDGE_COUNTER_ACTIVE) {
            set_output_idle(ch);
            channels[ch].pending = false;
            note_channel_trigger_complete(ch);
            restart_edge_counter_after_pulse(ch, slice);
        }
    }
}

static void gpio_callback(uint gpio, uint32_t events) {
    if (!armed) {
        return;
    }

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        if (gpio == channels[ch].input_gpio &&
            channels[ch].enabled &&
            channels[ch].trigger_mode == TRIGGER_MODE_TIME &&
            gpio_event_matches(events, channels[ch].edge_mode)) {
            schedule_channel_pulse(ch);
            return;
        }
    }
}

static void cancel_channel_alarms(uint ch) {
    if (channels[ch].start_alarm != 0) {
        cancel_alarm(channels[ch].start_alarm);
        channels[ch].start_alarm = 0;
    }

    if (channels[ch].end_alarm != 0) {
        cancel_alarm(channels[ch].end_alarm);
        channels[ch].end_alarm = 0;
    }
}

static void clear_pending_outputs(void) {
    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        cancel_channel_alarms(ch);
        stop_edge_counter(ch);
        channels[ch].pending = false;
        set_output_idle(ch);
    }
}

static void disable_channel_gpio(uint ch) {
    if (channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT) {
        stop_edge_counter(ch);
    } else {
        gpio_set_irq_enabled(
            channels[ch].input_gpio,
            GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
            false
        );
    }

    gpio_set_function(channels[ch].input_gpio, GPIO_FUNC_SIO);
    gpio_disable_pulls(channels[ch].input_gpio);

    gpio_put(channels[ch].output_gpio, 0);
    gpio_set_dir(channels[ch].output_gpio, GPIO_IN);
    gpio_disable_pulls(channels[ch].output_gpio);
}

static void enable_channel_irqs(void) {
    gpio_set_irq_callback(&gpio_callback);

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        if (channels[ch].trigger_mode != TRIGGER_MODE_TIME) {
            continue;
        }

        gpio_set_irq_enabled(
            channels[ch].input_gpio,
            GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
            true
        );
    }

    irq_set_enabled(IO_IRQ_BANK0, true);
}

void trigger_init(void) {
    status_led_init();

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        configure_channel_input(ch);
        configure_output_gpio(ch);
    }

    enable_channel_irqs();
    irq_set_exclusive_handler(PWM_DEFAULT_IRQ_NUM(), pwm_wrap_callback);
    irq_set_enabled(PWM_DEFAULT_IRQ_NUM(), true);
    initialized = true;
}

bool trigger_is_armed(void) {
    return armed;
}

void trigger_arm(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    finish_edge_diag_locked(NULL, false, true, true);
    stop_input_monitor_locked(NULL);
    clear_pending_outputs();
    armed = true;
    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        reset_step_reduce_runtime(ch);
        start_edge_counter(ch);
    }
    status_led_set_armed(true);
    restore_interrupts(irq_state);
}

void trigger_disarm(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    armed = false;
    finish_edge_diag_locked(NULL, false, true, true);
    stop_input_monitor_locked(NULL);
    clear_pending_outputs();
    status_led_set_armed(false);
    restore_interrupts(irq_state);
}

bool trigger_fire(uint ch) {
    if (!valid_channel(ch)) {
        return false;
    }

    status_led_blink_triggered();
    set_output_idle(ch);
    sleep_us(2);
    set_output_active(ch);
    sleep_us(channels[ch].width_us);
    set_output_idle(ch);
    return true;
}

bool trigger_drive_idle(uint ch) {
    if (!valid_channel(ch)) {
        return false;
    }

    set_output_idle(ch);
    return true;
}

bool trigger_drive_active(uint ch) {
    if (!valid_channel(ch)) {
        return false;
    }

    set_output_active(ch);
    return true;
}

bool trigger_drive_level(uint ch, bool high) {
    if (!valid_channel(ch)) {
        return false;
    }

    gpio_put(channels[ch].output_gpio, high ? 1 : 0);
    return true;
}

void trigger_config_defaults(trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT]) {
    if (!configs) {
        return;
    }

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        configs[ch] = default_configs[ch];
    }
}

bool trigger_config_validate_all(const trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT]) {
    if (!configs) {
        return false;
    }

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        if (!valid_gpio_pin(configs[ch].input_gpio) ||
            !valid_gpio_pin(configs[ch].output_gpio) ||
            configs[ch].input_gpio == configs[ch].output_gpio ||
            configs[ch].edge_mode > TRIGGER_EDGE_BOTH ||
            configs[ch].input_pull > TRIGGER_PULL_DOWN ||
            configs[ch].trigger_mode > TRIGGER_MODE_EDGE_COUNT ||
            configs[ch].edge_count_target == 0 ||
            configs[ch].edge_count_target > TRIGGER_EDGE_COUNT_MAX ||
            configs[ch].pulse_width_edges == 0 ||
            configs[ch].pulse_width_edges > TRIGGER_EDGE_COUNT_MAX ||
            configs[ch].step_reduce_every == 0 ||
            configs[ch].step_reduce_edge_delta > TRIGGER_EDGE_COUNT_MAX ||
            configs[ch].width_us == 0) {
            return false;
        }

        if (configs[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT &&
            !edge_count_gpio_allowed(configs[ch].input_gpio)) {
            return false;
        }

        for (uint other = ch + 1; other < TRIGGER_CHANNEL_COUNT; other++) {
            if (configs[ch].input_gpio == configs[other].input_gpio ||
                configs[ch].input_gpio == configs[other].output_gpio ||
                configs[ch].output_gpio == configs[other].input_gpio ||
                configs[ch].output_gpio == configs[other].output_gpio) {
                return false;
            }

            if (configs[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT &&
                configs[other].trigger_mode == TRIGGER_MODE_EDGE_COUNT &&
                pwm_gpio_to_slice_num(configs[ch].input_gpio) ==
                    pwm_gpio_to_slice_num(configs[other].input_gpio)) {
                return false;
            }
        }
    }

    return true;
}

bool trigger_get_config(uint ch, trigger_channel_config_t *config) {
    if (!valid_channel(ch) || !config) {
        return false;
    }

    config->input_gpio = channels[ch].input_gpio;
    config->output_gpio = channels[ch].output_gpio;
    config->enabled = channels[ch].enabled;
    config->edge_mode = channels[ch].edge_mode;
    config->input_pull = channels[ch].input_pull;
    config->delay_us = channels[ch].delay_us;
    config->width_us = channels[ch].width_us;
    config->trigger_mode = channels[ch].trigger_mode;
    config->edge_count_target = channels[ch].edge_count_target;
    config->pulse_width_edges = channels[ch].pulse_width_edges;
    config->auto_clear_edges = channels[ch].auto_clear_edges;
    config->auto_clear_delay_ns = channels[ch].auto_clear_delay_ns;
    config->step_reduce_enabled = channels[ch].step_reduce_enabled;
    config->step_reduce_every = channels[ch].step_reduce_every;
    config->step_reduce_edge_delta = channels[ch].step_reduce_edge_delta;
    config->step_reduce_delay_ns = channels[ch].step_reduce_delay_ns;
    config->idle_high = channels[ch].idle_high;
    config->active_high = channels[ch].active_high;
    return true;
}

bool trigger_get_all_configs(trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT]) {
    if (!configs) {
        return false;
    }

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        if (!trigger_get_config(ch, &configs[ch])) {
            return false;
        }
    }

    return true;
}

bool trigger_apply_all_configs(const trigger_channel_config_t configs[TRIGGER_CHANNEL_COUNT]) {
    if (!trigger_config_validate_all(configs)) {
        return false;
    }

    if (!initialized) {
        apply_config_values(configs);
        return true;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    armed = false;
    finish_edge_diag_locked(NULL, false, true, true);
    stop_input_monitor_locked(NULL);
    clear_pending_outputs();

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        disable_channel_gpio(ch);
    }

    apply_config_values(configs);

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        configure_channel_input(ch);
        configure_output_gpio(ch);
    }

    enable_channel_irqs();
    status_led_set_armed(false);
    restore_interrupts(irq_state);
    return true;
}

bool trigger_get_status(uint ch, trigger_channel_status_t *status) {
    if (!valid_channel(ch) || !status) {
        return false;
    }

    status->input_gpio = channels[ch].input_gpio;
    status->output_gpio = channels[ch].output_gpio;
    status->input_level = gpio_get(channels[ch].input_gpio);
    status->output_level = gpio_get(channels[ch].output_gpio);
    status->enabled = channels[ch].enabled;
    status->pending = channels[ch].pending;
    status->event_count = channels[ch].event_count;
    status->last_event_us = channels[ch].last_event_us;
    status->edge_mode = channels[ch].edge_mode;
    status->input_pull = channels[ch].input_pull;
    status->delay_us = channels[ch].delay_us;
    status->width_us = channels[ch].width_us;
    status->trigger_mode = channels[ch].trigger_mode;
    status->edge_count_target = channels[ch].edge_count_target;
    status->pulse_width_edges = channels[ch].pulse_width_edges;
    status->auto_clear_edges = channels[ch].auto_clear_edges;
    status->auto_clear_delay_ns = channels[ch].auto_clear_delay_ns;
    status->step_reduce_enabled = channels[ch].step_reduce_enabled;
    status->step_reduce_every = channels[ch].step_reduce_every;
    status->step_reduce_edge_delta = channels[ch].step_reduce_edge_delta;
    status->step_reduce_delay_ns = channels[ch].step_reduce_delay_ns;
    status->step_reduce_count = channels[ch].step_reduce_count;
    status->step_current_edge_count = active_edge_count_target(ch);
    status->step_current_delay_ns = active_delay_ns(ch);
    status->edge_count_seen = edge_count_current(ch);
    status->idle_high = channels[ch].idle_high;
    status->active_high = channels[ch].active_high;
    return true;
}

bool trigger_set_input_gpio(uint ch, uint gpio) {
    if (!valid_channel(ch) || !input_gpio_allowed(ch, gpio)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    finish_edge_diag_locked(NULL, false, true, true);
    stop_input_monitor_locked(NULL);
    uint old_gpio = channels[ch].input_gpio;
    if (channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT) {
        stop_edge_counter(ch);
    } else {
        gpio_set_irq_enabled(old_gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    }
    gpio_set_function(old_gpio, GPIO_FUNC_SIO);
    cancel_channel_alarms(ch);
    channels[ch].pending = false;
    channels[ch].input_gpio = gpio;
    configure_channel_input(ch);
    if (channels[ch].trigger_mode == TRIGGER_MODE_TIME) {
        gpio_set_irq_enabled(
            channels[ch].input_gpio,
            GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
            true
        );
    } else if (armed) {
        start_edge_counter(ch);
    }
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_output_gpio(uint ch, uint gpio) {
    if (!valid_channel(ch) || !output_gpio_allowed(ch, gpio)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    finish_edge_diag_locked(NULL, false, true, true);
    uint old_gpio = channels[ch].output_gpio;
    cancel_channel_alarms(ch);
    channels[ch].pending = false;
    gpio_put(old_gpio, 0);
    gpio_set_dir(old_gpio, GPIO_IN);
    gpio_disable_pulls(old_gpio);
    channels[ch].output_gpio = gpio;
    configure_output_gpio(ch);
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_edge(uint ch, trigger_edge_mode_t mode) {
    if (!valid_channel(ch) || mode > TRIGGER_EDGE_BOTH) {
        return false;
    }

    channels[ch].edge_mode = mode;
    return true;
}

bool trigger_set_input_pull(uint ch, trigger_input_pull_t pull) {
    if (!valid_channel(ch) || pull > TRIGGER_PULL_DOWN) {
        return false;
    }

    channels[ch].input_pull = pull;
    apply_input_pull(ch);
    return true;
}

bool trigger_set_delay_us(uint ch, uint32_t delay_us) {
    if (!valid_channel(ch)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    channels[ch].delay_us = delay_us;
    channels[ch].current_delay_ns = delay_us_to_ns_saturated(delay_us);
    channels[ch].step_reduce_count = 0;
    channels[ch].delay_remainder_ns = 0;
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_width_us(uint ch, uint32_t width_us) {
    if (!valid_channel(ch) || width_us == 0) {
        return false;
    }

    channels[ch].width_us = width_us;
    return true;
}

bool trigger_set_mode(uint ch, trigger_mode_t mode) {
    if (!valid_channel(ch) || mode > TRIGGER_MODE_EDGE_COUNT) {
        return false;
    }

    if (mode == TRIGGER_MODE_EDGE_COUNT &&
        (!edge_count_gpio_allowed(channels[ch].input_gpio) ||
         edge_count_slice_used_by_other_channel(channels[ch].input_gpio, ch))) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    finish_edge_diag_locked(NULL, false, true, true);
    stop_input_monitor_locked(NULL);
    cancel_channel_alarms(ch);
    stop_edge_counter(ch);
    channels[ch].pending = false;
    if (channels[ch].trigger_mode == TRIGGER_MODE_TIME) {
        gpio_set_irq_enabled(channels[ch].input_gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    }
    gpio_set_function(channels[ch].input_gpio, GPIO_FUNC_SIO);
    channels[ch].trigger_mode = mode;
    reset_step_reduce_runtime(ch);
    configure_channel_input(ch);
    if (channels[ch].trigger_mode == TRIGGER_MODE_TIME) {
        gpio_set_irq_enabled(channels[ch].input_gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    } else if (armed) {
        start_edge_counter(ch);
    }
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_edge_count_target(uint ch, uint32_t edge_count) {
    if (!valid_channel(ch) || edge_count == 0 || edge_count > TRIGGER_EDGE_COUNT_MAX) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    channels[ch].edge_count_target = edge_count;
    channels[ch].current_edge_count_target = edge_count;
    channels[ch].step_reduce_count = 0;
    if (armed && channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT) {
        stop_edge_counter(ch);
        start_edge_counter(ch);
    }
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_pulse_width_edges(uint ch, uint32_t edge_count) {
    if (!valid_channel(ch) || edge_count == 0 || edge_count > TRIGGER_EDGE_COUNT_MAX) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    channels[ch].pulse_width_edges = edge_count;
    if (armed && channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT) {
        stop_edge_counter(ch);
        start_edge_counter(ch);
    }
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_auto_clear_edges(uint ch, bool enabled) {
    if (!valid_channel(ch)) {
        return false;
    }

    channels[ch].auto_clear_edges = enabled;
    return true;
}

bool trigger_set_auto_clear_delay_ns(uint ch, uint32_t delay_ns) {
    if (!valid_channel(ch)) {
        return false;
    }

    channels[ch].auto_clear_delay_ns = delay_ns;
    return true;
}

bool trigger_set_step_reduce_enabled(uint ch, bool enabled) {
    if (!valid_channel(ch)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    channels[ch].step_reduce_enabled = enabled;
    reset_step_reduce_runtime(ch);
    if (armed && channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT) {
        stop_edge_counter(ch);
        start_edge_counter(ch);
    }
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_step_reduce_every(uint ch, uint32_t every) {
    if (!valid_channel(ch) || every == 0) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    channels[ch].step_reduce_every = every;
    reset_step_reduce_runtime(ch);
    if (armed && channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT) {
        stop_edge_counter(ch);
        start_edge_counter(ch);
    }
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_step_reduce_edge_delta(uint ch, uint32_t delta) {
    if (!valid_channel(ch) || delta > TRIGGER_EDGE_COUNT_MAX) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    channels[ch].step_reduce_edge_delta = delta;
    reset_step_reduce_runtime(ch);
    if (armed && channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT) {
        stop_edge_counter(ch);
        start_edge_counter(ch);
    }
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_step_reduce_delay_ns(uint ch, uint32_t delay_ns) {
    if (!valid_channel(ch)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    channels[ch].step_reduce_delay_ns = delay_ns;
    reset_step_reduce_runtime(ch);
    restore_interrupts(irq_state);
    return true;
}

bool trigger_clear_edge_seen(uint ch) {
    if (!valid_channel(ch)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    channels[ch].edge_count_seen = 0;

    if (channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT) {
        channels[ch].pending = false;
        set_output_idle(ch);

        if (armed && channels[ch].enabled) {
            start_edge_counter(ch);
        } else {
            stop_edge_counter(ch);
        }
    }

    restore_interrupts(irq_state);
    return true;
}

bool trigger_monitor_start(uint ch) {
    if (!valid_channel(ch) ||
        armed ||
        input_monitor.active ||
        edge_diag.active ||
        !edge_count_gpio_allowed(channels[ch].input_gpio)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    configure_input_monitor_locked(ch);
    restore_interrupts(irq_state);
    return true;
}

bool trigger_monitor_stop(trigger_monitor_status_t *status) {
    if (!input_monitor.active) {
        if (status) {
            uint32_t irq_state = save_and_disable_interrupts();
            fill_monitor_status_locked(status);
            restore_interrupts(irq_state);
        }
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    stop_input_monitor_locked(status);
    restore_interrupts(irq_state);
    return true;
}

bool trigger_monitor_get_status(trigger_monitor_status_t *status) {
    if (!status) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    fill_monitor_status_locked(status);
    restore_interrupts(irq_state);
    return status->active;
}

bool trigger_diag_config(
    uint ch,
    uint input_gpio,
    uint output_gpio,
    uint32_t fire_after_edges,
    uint32_t pulse_width_edges,
    uint32_t idle_gap_us
) {
    if (!valid_channel(ch) ||
        !edge_diag_gpio_allowed(input_gpio, output_gpio) ||
        fire_after_edges == 0 ||
        fire_after_edges > TRIGGER_EDGE_COUNT_MAX ||
        pulse_width_edges == 0 ||
        pulse_width_edges > TRIGGER_EDGE_COUNT_MAX ||
        idle_gap_us == 0) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    if (armed || input_monitor.active || edge_diag.active) {
        restore_interrupts(irq_state);
        return false;
    }

    edge_diag.configured = true;
    edge_diag.result_pending = false;
    edge_diag.sweep_mode = false;
    edge_diag.sweep_complete = false;
    edge_diag.channel = ch;
    edge_diag.input_gpio = input_gpio;
    edge_diag.output_gpio = output_gpio;
    edge_diag.pwm_slice = pwm_gpio_to_slice_num(input_gpio);
    edge_diag.run_index = 0;
    edge_diag.current_target_edge = fire_after_edges;
    edge_diag.fire_after_edges = fire_after_edges;
    edge_diag.pulse_width_edges = pulse_width_edges;
    edge_diag.idle_gap_us = idle_gap_us;
    edge_diag.phase = EDGE_DIAG_IDLE;
    edge_diag.fired = false;
    edge_diag.first_edge_seen = false;
    edge_diag.output_active = false;
    edge_diag.total_edges = 0;
    edge_diag.first_us = 0;
    edge_diag.trigger_on_us = 0;
    edge_diag.trigger_off_us = 0;
    edge_diag.last_edge_us = 0;
    edge_diag.transaction_duration_us = 0;
    edge_diag.avg_swclk_period_ns = 0;
    edge_diag.min_period_ns = 0;
    edge_diag.max_period_ns = 0;
    restore_interrupts(irq_state);
    return true;
}

bool trigger_diag_arm(uint ch) {
    if (!valid_channel(ch)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    if (armed ||
        input_monitor.active ||
        edge_diag.active ||
        !edge_diag.configured ||
        edge_diag.sweep_mode ||
        edge_diag.channel != ch ||
        !edge_diag_gpio_allowed(edge_diag.input_gpio, edge_diag.output_gpio)) {
        restore_interrupts(irq_state);
        return false;
    }

    start_edge_diag_locked();
    restore_interrupts(irq_state);
    return true;
}

bool trigger_diag_sweep_config(
    uint ch,
    uint input_gpio,
    uint output_gpio,
    uint32_t start_edge,
    uint32_t stop_edge,
    uint32_t step,
    uint32_t pulse_width_edges,
    uint32_t idle_gap_us
) {
    if (!valid_channel(ch) ||
        !edge_diag_gpio_allowed(input_gpio, output_gpio) ||
        start_edge == 0 ||
        start_edge > TRIGGER_EDGE_COUNT_MAX ||
        stop_edge == 0 ||
        stop_edge > TRIGGER_EDGE_COUNT_MAX ||
        stop_edge < start_edge ||
        step == 0 ||
        pulse_width_edges == 0 ||
        pulse_width_edges > TRIGGER_EDGE_COUNT_MAX ||
        idle_gap_us == 0) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    if (armed || input_monitor.active || edge_diag.active) {
        restore_interrupts(irq_state);
        return false;
    }

    edge_diag.configured = true;
    edge_diag.result_pending = false;
    edge_diag.sweep_mode = true;
    edge_diag.sweep_complete = false;
    edge_diag.channel = ch;
    edge_diag.input_gpio = input_gpio;
    edge_diag.output_gpio = output_gpio;
    edge_diag.pwm_slice = pwm_gpio_to_slice_num(input_gpio);
    edge_diag.run_index = 0;
    edge_diag.sweep_start_edge = start_edge;
    edge_diag.sweep_stop_edge = stop_edge;
    edge_diag.sweep_step = step;
    edge_diag.current_target_edge = start_edge;
    edge_diag.fire_after_edges = start_edge;
    edge_diag.pulse_width_edges = pulse_width_edges;
    edge_diag.idle_gap_us = idle_gap_us;
    edge_diag.phase = EDGE_DIAG_IDLE;
    edge_diag.fired = false;
    edge_diag.first_edge_seen = false;
    edge_diag.output_active = false;
    edge_diag.total_edges = 0;
    edge_diag.first_us = 0;
    edge_diag.trigger_on_us = 0;
    edge_diag.trigger_off_us = 0;
    edge_diag.last_edge_us = 0;
    edge_diag.transaction_duration_us = 0;
    edge_diag.avg_swclk_period_ns = 0;
    edge_diag.min_period_ns = 0;
    edge_diag.max_period_ns = 0;
    restore_interrupts(irq_state);
    return true;
}

bool trigger_diag_sweep_arm(uint ch) {
    if (!valid_channel(ch)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    if (armed ||
        input_monitor.active ||
        edge_diag.active ||
        !edge_diag.configured ||
        !edge_diag.sweep_mode ||
        edge_diag.sweep_complete ||
        edge_diag.channel != ch ||
        !edge_diag_gpio_allowed(edge_diag.input_gpio, edge_diag.output_gpio)) {
        restore_interrupts(irq_state);
        return false;
    }

    edge_diag.fire_after_edges = edge_diag.current_target_edge;
    start_edge_diag_locked();
    restore_interrupts(irq_state);
    return true;
}

bool trigger_diag_stop(trigger_diag_status_t *status) {
    if (!edge_diag.active) {
        if (status) {
            uint32_t irq_state = save_and_disable_interrupts();
            fill_edge_diag_status_locked(status);
            restore_interrupts(irq_state);
        }
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    finish_edge_diag_locked(status, false, true, true);
    restore_interrupts(irq_state);
    return true;
}

bool trigger_diag_get_status(trigger_diag_status_t *status) {
    if (!status) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    fill_edge_diag_status_locked(status);
    restore_interrupts(irq_state);
    return status->active;
}

bool trigger_diag_consume_result(trigger_diag_status_t *status) {
    if (!status) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    if (!edge_diag.result_pending) {
        restore_interrupts(irq_state);
        return false;
    }

    *status = edge_diag.pending_result;
    edge_diag.result_pending = false;
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_enabled(uint ch, bool enabled) {
    if (!valid_channel(ch)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    channels[ch].enabled = enabled;
    if (channels[ch].trigger_mode == TRIGGER_MODE_EDGE_COUNT && armed) {
        if (enabled) {
            start_edge_counter(ch);
        } else {
            stop_edge_counter(ch);
            channels[ch].pending = false;
            set_output_idle(ch);
        }
    }
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_idle_high(uint ch, bool high) {
    if (!valid_channel(ch)) {
        return false;
    }

    channels[ch].idle_high = high;
    set_output_idle(ch);
    return true;
}

bool trigger_set_active_high(uint ch, bool high) {
    if (!valid_channel(ch)) {
        return false;
    }

    channels[ch].active_high = high;
    return true;
}

const char *trigger_mode_name(trigger_mode_t mode) {
    if (mode == TRIGGER_MODE_EDGE_COUNT) {
        return "edge_count";
    }
    return "time";
}

const char *trigger_edge_name(trigger_edge_mode_t mode) {
    if (mode == TRIGGER_EDGE_RISING) {
        return "rising";
    }
    if (mode == TRIGGER_EDGE_FALLING) {
        return "falling";
    }
    return "both";
}

const char *trigger_pull_name(trigger_input_pull_t pull) {
    if (pull == TRIGGER_PULL_UP) {
        return "up";
    }
    if (pull == TRIGGER_PULL_DOWN) {
        return "down";
    }
    return "none";
}

const char *trigger_level_name(bool high) {
    return high ? "high" : "low";
}
