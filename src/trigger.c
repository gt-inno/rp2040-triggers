#include "trigger.h"

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "status_led.h"

#define RP2040_GPIO_COUNT 30

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

    bool idle_high;
    bool active_high;

    alarm_id_t start_alarm;
    alarm_id_t end_alarm;
} trigger_channel_t;

static trigger_channel_t channels[TRIGGER_CHANNEL_COUNT] = {
    {2, 10, true, false, 0, 0, TRIGGER_EDGE_RISING, TRIGGER_PULL_DOWN, 0, 100, false, true, 0, 0},
    {3, 11, true, false, 0, 0, TRIGGER_EDGE_RISING, TRIGGER_PULL_DOWN, 0, 100, false, true, 0, 0},
    {4, 12, true, false, 0, 0, TRIGGER_EDGE_RISING, TRIGGER_PULL_DOWN, 0, 100, false, true, 0, 0},
    {5, 13, true, false, 0, 0, TRIGGER_EDGE_RISING, TRIGGER_PULL_DOWN, 0, 100, false, true, 0, 0},
};

static volatile bool armed = false;

static void set_output_idle(uint ch);
static void set_output_active(uint ch);

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

static bool input_gpio_allowed(uint ch, uint gpio) {
    return valid_gpio_pin(gpio) &&
           channels[ch].output_gpio != gpio &&
           !gpio_used_by_other_channel(gpio, ch);
}

static bool output_gpio_allowed(uint ch, uint gpio) {
    return valid_gpio_pin(gpio) &&
           channels[ch].input_gpio != gpio &&
           !gpio_used_by_other_channel(gpio, ch);
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
    return 0;
}

static int64_t start_pulse_alarm(alarm_id_t id, void *user_data) {
    (void)id;

    uint ch = (uint)(uintptr_t)user_data;
    channels[ch].start_alarm = 0;
    set_output_active(ch);

    if (channels[ch].width_us == 0) {
        set_output_idle(ch);
        channels[ch].pending = false;
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
}

static void gpio_callback(uint gpio, uint32_t events) {
    if (!armed) {
        return;
    }

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        if (gpio == channels[ch].input_gpio &&
            channels[ch].enabled &&
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
        channels[ch].pending = false;
        set_output_idle(ch);
    }
}

void trigger_init(void) {
    status_led_init();

    for (uint ch = 0; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        configure_input_gpio(ch);
        configure_output_gpio(ch);
    }

    gpio_set_irq_enabled_with_callback(
        channels[0].input_gpio,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
        true,
        &gpio_callback
    );

    for (uint ch = 1; ch < TRIGGER_CHANNEL_COUNT; ch++) {
        gpio_set_irq_enabled(
            channels[ch].input_gpio,
            GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
            true
        );
    }
}

bool trigger_is_armed(void) {
    return armed;
}

void trigger_arm(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    clear_pending_outputs();
    armed = true;
    status_led_set_armed(true);
    restore_interrupts(irq_state);
}

void trigger_disarm(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    armed = false;
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
    status->idle_high = channels[ch].idle_high;
    status->active_high = channels[ch].active_high;
    return true;
}

bool trigger_set_input_gpio(uint ch, uint gpio) {
    if (!valid_channel(ch) || !input_gpio_allowed(ch, gpio)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
    uint old_gpio = channels[ch].input_gpio;
    gpio_set_irq_enabled(old_gpio, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    cancel_channel_alarms(ch);
    channels[ch].pending = false;
    channels[ch].input_gpio = gpio;
    configure_input_gpio(ch);
    gpio_set_irq_enabled(
        channels[ch].input_gpio,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
        true
    );
    restore_interrupts(irq_state);
    return true;
}

bool trigger_set_output_gpio(uint ch, uint gpio) {
    if (!valid_channel(ch) || !output_gpio_allowed(ch, gpio)) {
        return false;
    }

    uint32_t irq_state = save_and_disable_interrupts();
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

    channels[ch].delay_us = delay_us;
    return true;
}

bool trigger_set_width_us(uint ch, uint32_t width_us) {
    if (!valid_channel(ch) || width_us == 0) {
        return false;
    }

    channels[ch].width_us = width_us;
    return true;
}

bool trigger_set_enabled(uint ch, bool enabled) {
    if (!valid_channel(ch)) {
        return false;
    }

    channels[ch].enabled = enabled;
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
