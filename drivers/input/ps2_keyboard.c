#include <stdint.h>
#include "drivers/input/ps2_keyboard.h"
#include "input/keyboard.h"

#define PS2_DATA_PORT   0x60u
#define PS2_STATUS_PORT 0x64u
#define PS2_CMD_PORT    0x64u

#define PS2_STATUS_OUTPUT_FULL 0x01u
#define PS2_STATUS_INPUT_FULL  0x02u
#define PS2_STATUS_AUX_DATA    0x20u

#define PS2_WAIT_LIMIT 100000u
#define PS2_POLL_LIMIT 32u

static int g_available;
static uint8_t g_extended;
static uint8_t g_pause_remaining;
static uint8_t g_left_shift, g_right_shift;
static uint8_t g_left_ctrl, g_right_ctrl;
static uint8_t g_left_alt, g_right_alt;
static uint8_t g_caps_lock;
static uint32_t g_dropped_scancodes;

static inline void io_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t io_in8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static int wait_input_empty(void)
{
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; ++i) {
        if (!(io_in8(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL))
            return 1;
        __asm__ volatile ("pause");
    }
    return 0;
}

static int wait_output_full(void)
{
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; ++i) {
        if (io_in8(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)
            return 1;
        __asm__ volatile ("pause");
    }
    return 0;
}

static void drain_output(void)
{
    for (uint32_t i = 0; i < 32; ++i) {
        if (!(io_in8(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL))
            break;
        (void)io_in8(PS2_DATA_PORT);
    }
}

static int controller_command(uint8_t command)
{
    if (!wait_input_empty())
        return 0;
    io_out8(PS2_CMD_PORT, command);
    return wait_input_empty();
}

static int controller_read_config(uint8_t *config)
{
    if (!config || !controller_command(0x20))
        return 0;
    if (!wait_output_full())
        return 0;
    *config = io_in8(PS2_DATA_PORT);
    return *config != 0xFF;
}

static int controller_write_config(uint8_t config)
{
    if (!controller_command(0x60) || !wait_input_empty())
        return 0;
    io_out8(PS2_DATA_PORT, config);
    return wait_input_empty();
}

static int keyboard_response(uint8_t *response)
{
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; ++i) {
        uint8_t status = io_in8(PS2_STATUS_PORT);
        if (status & PS2_STATUS_OUTPUT_FULL) {
            uint8_t value = io_in8(PS2_DATA_PORT);
            if (status & PS2_STATUS_AUX_DATA)
                continue;
            if (response)
                *response = value;
            return 1;
        }
        __asm__ volatile ("pause");
    }
    return 0;
}

static int send_keyboard_command(uint8_t command)
{
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        if (!wait_input_empty())
            return 0;
        io_out8(PS2_DATA_PORT, command);

        uint8_t response = 0;
        if (!keyboard_response(&response))
            return 0;
        if (response == 0xFA)
            return 1; /* ACK */
        if (response != 0xFE)
            return 0; /* Not ACK or RESEND. */
    }
    return 0;
}

static uint16_t translate_scancode(uint8_t code, int extended)
{
    if (extended) {
        switch (code) {
        case 0x1C: return KEY_ENTER;
        case 0x1D: return KEY_RIGHT_CTRL;
        case 0x35: return KEY_SLASH;
        case 0x38: return KEY_RIGHT_ALT;
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UP;
        case 0x49: return KEY_PAGE_UP;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x4F: return KEY_END;
        case 0x50: return KEY_DOWN;
        case 0x51: return KEY_PAGE_DOWN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        case 0x5B: return KEY_LEFT_META;
        case 0x5C: return KEY_RIGHT_META;
        default:   return KEY_NONE;
        }
    }

    if (code >= 0x02 && code <= 0x0A)
        return (uint16_t)(KEY_1 + code - 0x02);
    switch (code) {
    case 0x01: return KEY_ESCAPE;
    case 0x0B: return KEY_0;
    case 0x0C: return KEY_MINUS;
    case 0x0D: return KEY_EQUAL;
    case 0x0E: return KEY_BACKSPACE;
    case 0x0F: return KEY_TAB;
    case 0x1A: return KEY_LEFT_BRACE;
    case 0x1B: return KEY_RIGHT_BRACE;
    case 0x1C: return KEY_ENTER;
    case 0x1D: return KEY_LEFT_CTRL;
    case 0x2A: return KEY_LEFT_SHIFT;
    case 0x2B: return KEY_BACKSLASH;
    case 0x36: return KEY_RIGHT_SHIFT;
    case 0x37: return KEY_PRINT_SCREEN;
    case 0x38: return KEY_LEFT_ALT;
    case 0x39: return KEY_SPACE;
    case 0x3A: return KEY_CAPS_LOCK;
    case 0x3B: return KEY_F1;
    case 0x3C: return KEY_F2;
    case 0x3D: return KEY_F3;
    case 0x3E: return KEY_F4;
    case 0x3F: return KEY_F5;
    case 0x40: return KEY_F6;
    case 0x41: return KEY_F7;
    case 0x42: return KEY_F8;
    case 0x43: return KEY_F9;
    case 0x44: return KEY_F10;
    case 0x45: return KEY_NUM_LOCK;
    case 0x46: return KEY_SCROLL_LOCK;
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;
    case 0x27: return KEY_SEMICOLON;
    case 0x28: return KEY_APOSTROPHE;
    case 0x29: return KEY_GRAVE;
    case 0x33: return KEY_COMMA;
    case 0x34: return KEY_DOT;
    case 0x35: return KEY_SLASH;
    default: break;
    }

    /* Main alphanumeric rows, translated to HID usages. */
    switch (code) {
    case 0x10: return KEY_Q; case 0x11: return KEY_W;
    case 0x12: return KEY_E; case 0x13: return KEY_R;
    case 0x14: return KEY_T; case 0x15: return KEY_Y;
    case 0x16: return KEY_U; case 0x17: return KEY_I;
    case 0x18: return KEY_O; case 0x19: return KEY_P;
    case 0x1E: return KEY_A; case 0x1F: return KEY_S;
    case 0x20: return KEY_D; case 0x21: return KEY_F;
    case 0x22: return KEY_G; case 0x23: return KEY_H;
    case 0x24: return KEY_J; case 0x25: return KEY_K;
    case 0x26: return KEY_L;
    case 0x2C: return KEY_Z; case 0x2D: return KEY_X;
    case 0x2E: return KEY_C; case 0x2F: return KEY_V;
    case 0x30: return KEY_B; case 0x31: return KEY_N;
    case 0x32: return KEY_M;
    default:   return KEY_NONE;
    }
}

static uint8_t current_modifiers(void)
{
    uint8_t mods = 0;
    if (g_left_shift || g_right_shift) mods |= KEY_MOD_SHIFT;
    if (g_left_ctrl || g_right_ctrl)   mods |= KEY_MOD_CTRL;
    if (g_left_alt || g_right_alt)     mods |= KEY_MOD_ALT;
    if (g_caps_lock)                   mods |= KEY_MOD_CAPS;
    return mods;
}

static void update_modifier(uint16_t keycode, int pressed)
{
    uint8_t value = pressed ? 1u : 0u;
    switch (keycode) {
    case KEY_LEFT_SHIFT:  g_left_shift = value; break;
    case KEY_RIGHT_SHIFT: g_right_shift = value; break;
    case KEY_LEFT_CTRL:   g_left_ctrl = value; break;
    case KEY_RIGHT_CTRL:  g_right_ctrl = value; break;
    case KEY_LEFT_ALT:    g_left_alt = value; break;
    case KEY_RIGHT_ALT:   g_right_alt = value; break;
    case KEY_CAPS_LOCK:   if (pressed) g_caps_lock ^= 1u; break;
    default: break;
    }
}

static void handle_scancode(uint8_t scan)
{
    if (g_pause_remaining) {
        --g_pause_remaining;
        return;
    }
    if (scan == 0xE1) {
        g_pause_remaining = 5;
        g_extended = 0;
        return;
    }
    if (scan == 0xE0) {
        g_extended = 1;
        return;
    }

    int extended = g_extended;
    g_extended = 0;
    int pressed = (scan & 0x80u) == 0;
    uint8_t code = scan & 0x7Fu;
    uint16_t keycode = translate_scancode(code, extended);
    if (keycode == KEY_NONE) {
        ++g_dropped_scancodes;
        return;
    }

    update_modifier(keycode, pressed);
    uint8_t modifiers = current_modifiers();
    struct key_event event = {
        .keycode = keycode,
        .ascii = pressed ? keyboard_key_to_ascii(keycode, modifiers) : 0,
        .modifiers = modifiers,
        .pressed = pressed ? 1u : 0u
    };
    if (!keyboard_event_push(&event))
        ++g_dropped_scancodes;
}

int ps2_keyboard_init(void)
{
    g_available = 0;
    g_extended = 0;
    g_pause_remaining = 0;
    g_left_shift = g_right_shift = 0;
    g_left_ctrl = g_right_ctrl = 0;
    g_left_alt = g_right_alt = 0;
    g_caps_lock = 0;
    g_dropped_scancodes = 0;

    /* Disable both ports while changing controller configuration. */
    if (!controller_command(0xAD) || !controller_command(0xA7))
        return 0;
    drain_output();

    uint8_t config = 0;
    if (!controller_read_config(&config))
        return 0;

    /* Polling mode: IRQ1/IRQ12 off. Enable port 1 and Set-2->Set-1 translation. */
    config &= (uint8_t)~(0x01u | 0x02u | 0x10u);
    config |= 0x40u;
    if (!controller_write_config(config))
        return 0;

    if (!controller_command(0xAE))
        return 0; /* Enable first PS/2 port. */

    /* Reset keyboard scanning state and then enable scan stream. */
    if (!send_keyboard_command(0xF5) || !send_keyboard_command(0xF4))
        return 0;

    g_available = 1;
    return 1;
}

void ps2_keyboard_poll(void)
{
    if (!g_available)
        return;

    for (uint32_t i = 0; i < PS2_POLL_LIMIT; ++i) {
        uint8_t status = io_in8(PS2_STATUS_PORT);
        if (!(status & PS2_STATUS_OUTPUT_FULL))
            break;
        uint8_t data = io_in8(PS2_DATA_PORT);
        if (status & PS2_STATUS_AUX_DATA)
            continue;
        handle_scancode(data);
    }
}

int ps2_keyboard_available(void)
{
    return g_available;
}

uint32_t ps2_keyboard_scancodes_dropped(void)
{
    return g_dropped_scancodes;
}
