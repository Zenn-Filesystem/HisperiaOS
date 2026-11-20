#ifndef HISPERIA_INPUT_KEYCODES_H
#define HISPERIA_INPUT_KEYCODES_H

#include <stdint.h>

/* Keyboard usages follow the USB HID Keyboard/Keypad usage page.
 * This keeps input events independent from PS/2 scan-code values. */
enum keycode {
    KEY_NONE = 0,
    KEY_A = 0x04, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H,
    KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q,
    KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,

    KEY_1 = 0x1E, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8,
    KEY_9, KEY_0,
    KEY_ENTER = 0x28, KEY_ESCAPE, KEY_BACKSPACE, KEY_TAB, KEY_SPACE,
    KEY_MINUS, KEY_EQUAL, KEY_LEFT_BRACE, KEY_RIGHT_BRACE,
    KEY_BACKSLASH, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
    KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_CAPS_LOCK,

    KEY_F1 = 0x3A, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_PRINT_SCREEN, KEY_SCROLL_LOCK, KEY_PAUSE,
    KEY_INSERT, KEY_HOME, KEY_PAGE_UP, KEY_DELETE, KEY_END,
    KEY_PAGE_DOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    KEY_NUM_LOCK,

    KEY_LEFT_CTRL = 0xE0, KEY_LEFT_SHIFT, KEY_LEFT_ALT, KEY_LEFT_META,
    KEY_RIGHT_CTRL, KEY_RIGHT_SHIFT, KEY_RIGHT_ALT, KEY_RIGHT_META
};

#define KEY_MOD_SHIFT  (1u << 0)
#define KEY_MOD_CTRL   (1u << 1)
#define KEY_MOD_ALT    (1u << 2)
#define KEY_MOD_CAPS   (1u << 3)

struct key_event {
    uint16_t keycode;
    uint8_t ascii;
    uint8_t modifiers;
    uint8_t pressed;
    uint8_t reserved[3];
};

#endif
