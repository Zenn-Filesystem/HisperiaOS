# HisperiaOS device-neutral libraries

Reusable, hardware-independent code lives under `lib/`.

- `fs/` — filesystem format implementations (FAT32 and ext4).
- `input/` — generic keyboard events, queue, and keymap translation.
- `storage/` — partition-table parsing and related storage-format helpers.

A library must not issue device I/O directly. Hardware access belongs in
`drivers/`; kernel services and orchestration remain in `src/` for now.
