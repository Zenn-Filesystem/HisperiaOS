#include <stdio.h>
#include <string.h>
#include "input/keyboard.h"

static int expect_ascii(uint16_t key, uint8_t mods, uint8_t expected)
{
    uint8_t actual = keyboard_key_to_ascii(key, mods);
    if (actual != expected) {
        fprintf(stderr, "keymap mismatch: key=%u mods=%u got=%u expected=%u\n",
                (unsigned)key, (unsigned)mods, (unsigned)actual, (unsigned)expected);
        return 0;
    }
    return 1;
}

int main(void)
{
    struct key_event event = {
        .keycode = KEY_A, .ascii = 'a', .modifiers = 0, .pressed = 1
    };
    struct key_event received;

    if (!expect_ascii(KEY_A, 0, 'a') ||
        !expect_ascii(KEY_A, KEY_MOD_SHIFT, 'A') ||
        !expect_ascii(KEY_A, KEY_MOD_CAPS, 'A') ||
        !expect_ascii(KEY_A, KEY_MOD_SHIFT | KEY_MOD_CAPS, 'a') ||
        !expect_ascii(KEY_1, KEY_MOD_SHIFT, '!') ||
        !expect_ascii(KEY_ENTER, 0, '\n') ||
        !expect_ascii(KEY_BACKSPACE, 0, '\b') ||
        !expect_ascii(KEY_SLASH, KEY_MOD_SHIFT, '?'))
        return 1;

    if (!keyboard_event_push(&event) || !keyboard_event_pop(&received))
        return 1;
    if (memcmp(&event, &received, sizeof(event)) != 0)
        return 1;

    for (unsigned i = 0; i < 80; ++i)
        (void)keyboard_event_push(&event);
    if (keyboard_event_dropped() == 0)
        return 1;

    puts("[KBD-TEST] US keymap, event queue, and overflow handling PASS.");
    return 0;
}
