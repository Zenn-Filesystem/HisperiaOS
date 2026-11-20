# Storage changes

## Cross-transport VFS loading

- Added AHCI-first, ATA-PIO-fallback probing to the boot path.
- Both controller drivers now feed the same `block_device` → MBR/GPT → ext4 →
  generic VFS path.
- A VFS root is mounted from the first supported ext4 partition discovered.
- Kept block-device and ext4 state at static lifetime because VFS retains pointers.
- Clarified that FAT32 currently provides BPB/geometry detection only and is not
  a VFS backend.

## Validation in the packaging environment

- `make test-storage-host`: PASS.
- `make test-vfs-host`: PASS.
- `make test-ext4-host`: PASS.
- `make validate`: all three kernel configurations compile and link.
- Runtime AHCI/ATA cross-loading was not executed in this environment; run the
  QEMU targets in `CROSS-LOAD.md` and test the physical laptop separately.
