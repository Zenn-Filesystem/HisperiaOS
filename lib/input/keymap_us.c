#include "input/keyboard.h"

static int is_letter(uint16_t keycode)
{
    return keycode >= KEY_A && keycode <= KEY_Z;
}

uint8_t keyboard_key_to_ascii(uint16_t keycode, uint8_t modifiers)
{
    if (is_letter(keycode)) {
        uint8_t c = (uint8_t)('a' + (keycode - KEY_A));
        if (!!(modifiers & KEY_MOD_SHIFT) != !!(modifiers & KEY_MOD_CAPS))
            c = (uint8_t)(c - 'a' + 'A');
        if (modifiers & KEY_MOD_CTRL)
            c = (uint8_t)(c & 0x1f);
        return c;
    }

    if (keycode >= KEY_1 && keycode <= KEY_9) {
        static const char normal[] = "123456789";
        static const char shifted[] = "!@#$%^&*(";
        uint32_t i = keycode - KEY_1;
        return (uint8_t)((modifiers & KEY_MOD_SHIFT) ? shifted[i] : normal[i]);
    }
    if (keycode == KEY_0)
        return (uint8_t)((modifiers & KEY_MOD_SHIFT) ? ')' : '0');

    switch (keycode) {
    case KEY_ENTER:       return '\n';
    case KEY_BACKSPACE:   return '\b';
    case KEY_TAB:         return '\t';
    case KEY_SPACE:       return ' ';
    case KEY_MINUS:       return (modifiers & KEY_MOD_SHIFT) ? '_' : '-';
    case KEY_EQUAL:       return (modifiers & KEY_MOD_SHIFT) ? '+' : '=';
    case KEY_LEFT_BRACE:  return (modifiers & KEY_MOD_SHIFT) ? '{' : '[';
    case KEY_RIGHT_BRACE: return (modifiers & KEY_MOD_SHIFT) ? '}' : ']';
    case KEY_BACKSLASH:   return (modifiers & KEY_MOD_SHIFT) ? '|' : '\\';
    case KEY_SEMICOLON:   return (modifiers & KEY_MOD_SHIFT) ? ':' : ';';
    case KEY_APOSTROPHE:  return (modifiers & KEY_MOD_SHIFT) ? '"' : '\'';
    case KEY_GRAVE:       return (modifiers & KEY_MOD_SHIFT) ? '~' : '`';
    case KEY_COMMA:       return (modifiers & KEY_MOD_SHIFT) ? '<' : ',';
    case KEY_DOT:         return (modifiers & KEY_MOD_SHIFT) ? '>' : '.';
    case KEY_SLASH:       return (modifiers & KEY_MOD_SHIFT) ? '?' : '/';
    default:              return 0;
    }
}
