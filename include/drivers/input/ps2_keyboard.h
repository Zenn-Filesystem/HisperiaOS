#ifndef HISPERIA_DRIVER_PS2_KEYBOARD_H
#define HISPERIA_DRIVER_PS2_KEYBOARD_H

#include <stdint.h>

/* Legacy i8042/PS/2 keyboard driver. Polling only; no IRQ/PIC dependency. */
int ps2_keyboard_init(void);
void ps2_keyboard_poll(void);
int ps2_keyboard_available(void);
uint32_t ps2_keyboard_scancodes_dropped(void);

#endif
