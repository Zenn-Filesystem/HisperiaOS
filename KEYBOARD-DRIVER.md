# PS/2 Keyboard Driver v0.1

## Scope

The first keyboard implementation targets the legacy i8042 controller and
its first PS/2 port. It is intentionally polling-based so it does not depend
on PIC remapping, IRQ1, IOAPIC routing, or changes to the existing exception
ISR path.

## Runtime path

```text
i8042 status/data ports (0x64 / 0x60)
        |
        v
drivers/input/ps2_keyboard.c
  - controller setup
  - Set-2 to Set-1 translation
  - scan-code make/break decoding
        |
        v
lib/input/keyboard.c
  - device-neutral key_event queue
        |
        +------> lib/input/keymap_us.c -> ASCII console echo
        |
        +------> future TTY / shell / other keyboard transports
```

## Supported input

- A-Z, 0-9, US punctuation and Space
- Enter, Backspace, Tab, Escape
- Left/right Shift, Ctrl and Alt
- Caps Lock
- F1-F12 and common extended navigation keys (events only)

The controller is configured with keyboard IRQ disabled, auxiliary port
masked, first port enabled, and translation enabled. Keyboard scanning is
re-enabled with the standard F4 command. The driver drains up to 32 bytes per
poll to avoid monopolizing one iteration of the kernel input loop.

## Public interfaces

- `ps2_keyboard_init()` / `ps2_keyboard_poll()` — physical PS/2 driver
- `keyboard_event_pop()` — consume a normalized `struct key_event`
- `keyboard_event_push()` — submit an event from another input driver
- `keyboard_key_to_ascii()` — US layout conversion helper

Keycodes use HID Keyboard/Keypad usage values, not PS/2 scan-code numbers.
This allows future USB HID or another transport to submit the same logical
key events without changing the console or keymap layer.

## Limitations

- Polling only; no IRQ1/PIC/APIC integration yet.
- No USB HID, Bluetooth, mouse, or non-US layouts.
- No terminal line discipline, command history, or input focus management.
- Early polling loop is a busy loop with `pause`; it is not an idle scheduler.
- Hardware support depends on firmware exposing an i8042-compatible PS/2
  controller. Some modern systems expose only USB HID for their keyboard.

## Tests

`make test-keyboard-host` validates US keymap conversions, event queue order,
and queue overflow behavior. It does not emulate i8042 hardware. Runtime
validation must be performed in QEMU or on a machine exposing a PS/2 port.
