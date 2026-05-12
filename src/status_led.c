#include "status_led.h"

#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#define STATUS_LED_TRIGGER_BLINK_US 80000

static volatile bool status_led_armed = false;

#ifdef PICO_DEFAULT_LED_PIN
static alarm_id_t status_led_blink_alarm = 0;
#endif

static void status_led_put(bool on) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, on ? 1 : 0);
#else
    (void)on;
#endif
}

static int64_t status_led_blink_done_alarm(alarm_id_t id, void *user_data) {
    (void)id;
    (void)user_data;

#ifdef PICO_DEFAULT_LED_PIN
    status_led_blink_alarm = 0;
#endif
    status_led_put(status_led_armed);
    return 0;
}

void status_led_init(void) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif
    status_led_put(false);
}

void status_led_set_armed(bool armed) {
    status_led_armed = armed;

#ifdef PICO_DEFAULT_LED_PIN
    if (status_led_blink_alarm != 0) {
        cancel_alarm(status_led_blink_alarm);
        status_led_blink_alarm = 0;
    }
#endif

    status_led_put(status_led_armed);
}

void status_led_blink_triggered(void) {
#ifdef PICO_DEFAULT_LED_PIN
    if (status_led_blink_alarm != 0) {
        cancel_alarm(status_led_blink_alarm);
        status_led_blink_alarm = 0;
    }

    status_led_put(!status_led_armed);
    status_led_blink_alarm = add_alarm_in_us(
        STATUS_LED_TRIGGER_BLINK_US,
        status_led_blink_done_alarm,
        NULL,
        true
    );
#endif
}
