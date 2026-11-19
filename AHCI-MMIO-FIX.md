# AHCI MMIO mapping fix (v0.2)

## Root cause

The previous AHCI initialization translated PCI BAR5 / ABAR with
`pmm_phys_to_virt()`. That helper only computes a Limine HHDM alias for RAM.
A PCI AHCI ABAR is device MMIO and is not guaranteed to be present in the HHDM.
The resulting access to `GHC` at `ABAR + 0x04` caused the observed page fault.

The faulting instruction from the reported QEMU build was:

```asm
orl $0x80000000, 0x4(%rax)
```

where `%rax` was the HHDM-translated ABAR address.

## Changes

- Added `vmm_map_mmio(physical_address, length)`.
- MMIO mappings are allocated from a dedicated supervisor virtual window beginning
  at `0xFFFFC10000000000`, separate from the RAM HHDM, kernel image, stack, and heap.
- Each MMIO page is mapped writable, NX, and with PCD+PWT cache-disable attributes.
- The regular `vmm_map_page()` retains its PMM-frame physical-address restriction;
  only the explicit MMIO mapping path may map physical addresses outside
  `PMM_MAX_PHYS` (subject to the CPU physical-address width).
- AHCI maps the full HBA register aperture via `vmm_map_mmio()` and no longer
  uses `pmm_phys_to_virt()` for ABAR.
- AHCI init now checks the SATA port engine start result.
- Added `ahci_last_error()` and a boot-log message identifying the stage at which
  AHCI initialization fails.

## Validation performed

- `make validate`: PASS.
- Host block-device, MBR/GPT, and FAT32 BPB tests: PASS.
- Kernel compile/link configurations for `RUN_MPROTECT_TEST=0`, `1`, and `2`: PASS.
- QEMU AHCI runtime: not run in the build environment because QEMU, `mkfs.fat`,
  and `xorriso` are not installed there.

## Runtime test on the developer machine

```sh
make clean
make run-sata
```

The Makefile target is `run-sata` (alias `qemu-sata`), not `run-ahci`.

A successful controller/device initialization should advance beyond the old
`[AHCI] Scanning PCI for AHCI controller...` point and print the detected AHCI
sector count. If it fails cleanly, the kernel now prints an `[AHCI] Initialization
failed: ...` reason. That reason will guide the next debugging step.
