# HisperiaOS device drivers

Hardware-facing code lives under `drivers/`; kernel policy and device-neutral
interfaces should not be placed here.

- `bus/` — bus discovery and enumeration (currently PCI).
- `firmware/` — firmware-defined interfaces (currently ACPI fixed events and S5).
- `input/` — physical input transports (currently the legacy i8042/PS/2 keyboard).
- `storage/` — physical storage controllers (ATA PIO and AHCI).

Drivers expose narrow interfaces through `include/` headers. They should not
own filesystem policy, keymaps, terminal rendering, or generic event queues.
