#ifndef HISPERIA_INPUT_KEYBOARD_H
#define HISPERIA_INPUT_KEYBOARD_H

#include <stdint.h>
#include "input/keycodes.h"

/* Device-neutral keyboard event queue and keymap interface. */
int keyboard_event_push(const struct key_event *event);
int keyboard_event_pop(struct key_event *event);
uint32_t keyboard_event_dropped(void);
uint8_t keyboard_key_to_ascii(uint16_t keycode, uint8_t modifiers);

#endif
