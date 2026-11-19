# Storage v0.2 — FAT32 read-only file access

## Added

- FAT32 cluster-to-LBA translation with volume/device bounds checks.
- FAT entry lookup, including FAT32 28-bit masking and active-FAT selection.
- End-of-chain, bad-cluster, reserved-cluster, and out-of-range validation.
- 8.3 short-name normalization and case-insensitive hierarchical path lookup.
- Subdirectory traversal across cluster chains.
- Offset-based file reads across sectors and cluster boundaries.
- Directory/file-read self-test using a synthetic FAT32 disk.
- QEMU disk fixture generator that inserts `/HELLO.TXT` into the FAT32 root directory.
- Boot-time read of `/HELLO.TXT` after mounting the QEMU disk.

## Validation performed in the packaging environment

- `make test-storage-host`: PASS.
- Host test with AddressSanitizer and UndefinedBehaviorSanitizer: PASS.
- `make validate`: normal kernel and both memory-protection configurations compile/link PASS.
- Python image-generator syntax check: PASS.
- Image fixture injection was exercised against a temporary mock `mkfs.fat`-generated FAT32 geometry and the resulting directory entry, FAT chain, and file payload were verified.

`qemu-system-x86_64`, `xorriso`, and `mkfs.fat` were not available in the packaging environment, so a full ISO build and QEMU boot/runtime test were not performed here. Run `make run-fat32` on a machine with those tools and verify the expected `/HELLO.TXT` serial output.
