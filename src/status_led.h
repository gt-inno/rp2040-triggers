#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>

void status_led_init(void);
void status_led_set_armed(bool armed);
void status_led_blink_triggered(void);

#endif
