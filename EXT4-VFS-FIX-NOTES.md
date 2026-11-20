# ext4-VFS v0.1 -> v0.1.1 fix

## Bug
HisperiaOS-EXT4-VFS-v0_1 was built on the AHCI code from *before* the MMIO fix
(v0.2). Its ahci.c still did `pmm_phys_to_virt(ABAR)`, so the first write to
GHC (`orl $0x80000000, 0x4(%rax)`) page-faulted again.

## Changes
- Re-applied the v0.2 AHCI MMIO fix: src/ahci.c, src/vmm.c, include/ahci.h,
  include/vmm.h (vmm_map_mmio, ahci_last_error).
- main.c: restored "[AHCI] Initialization failed: <reason>" boot message.
- main.c: `ext4_root` is now `static`. vfs_mount_ext4() stores a pointer to it
  in a global, and it previously lived on the stack of a nested block, so the
  pointer dangled once that block ended.

## Validation
- make test-storage-host / test-ext4-host: PASS
- All src/*.c compile freestanding (gcc; clang not available in this sandbox).
- QEMU runtime not run here: use `make clean && make run-sata`.
