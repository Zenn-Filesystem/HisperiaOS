# HisperiaOS AHCI SATA backend (experimental)

This update adds a polling, read-only AHCI backend while preserving the existing block-device abstraction and legacy ATA PIO driver.

## Included

- PCI configuration mechanism #1 enumeration (I/O ports 0xCF8/0xCFC).
- AHCI controller discovery (PCI class 01/subclass 06/prog-if 01).
- PCI memory-space and bus-master enable.
- BAR5 ABAR discovery.
- SATA ATA port detection.
- DMA command-list, received-FIS, command-table, and bounce-buffer allocation.
- ATA IDENTIFY DEVICE using AHCI DMA.
- READ DMA EXT (48-bit LBA), polling completion, bounded waits.
- Read-only `block_device` registration as `ahci-sata0`.
- Partition scan and FAT32 mount attempt through the existing layers.
- `make run-sata` / `make qemu-sata` QEMU target using q35 + ICH9 AHCI.

## Build and test

```sh
make clean
make validate
make run-sata
```

The QEMU target creates/reuses `build/fat32-disk.img` and attaches it to AHCI port 0. Watch the serial log for `[AHCI]`, `[PART] AHCI`, and `[FAT ] AHCI`.

## Current limitations / important caveats

- This is an initial experimental driver, not a production-ready SATA driver.
- Polling only; no MSI/MSI-X or interrupt-driven completion.
- Uses command slot 0 and one PRDT entry with a 4 KiB bounce buffer.
- Read-only; no WRITE DMA EXT, NCQ, hot-plug, ATAPI, or port multiplier support.
- Requires an AHCI controller and a SATA ATA disk; legacy IDE compatibility is still handled by the separate ATA PIO backend.
- Requires LBA48 support and rejects controllers/devices outside the implemented capability subset.
- ABAR is mapped through `vmm_map_mmio()` into a dedicated supervisor virtual window. The mapping is writable, NX, and uses PCD+PWT (uncached under the architectural reset PAT layout); device MMIO is not translated through the RAM HHDM.
- The mapping path has compile/link validation only in this environment. QEMU and physical-hardware runtime behavior still need to be verified.
- No QEMU runtime result is claimed by this package. `make validate` only verifies host storage tests and kernel compile/link configurations.

## Expected QEMU log shape

```text
[AHCI] Scanning PCI for AHCI controller...
[AHCI] Device: ahci-sata0 | sectors: ... | SATA READ DMA EXT, polling, read-only.
[PART] AHCI partitions detected: 1
[FAT ] AHCI FAT32 mounted read-only: HISPERIA
```

The exact output depends on QEMU device enumeration and the attached disk.
