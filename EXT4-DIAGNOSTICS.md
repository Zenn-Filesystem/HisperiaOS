# HisperiaOS ext4 probe diagnostics

This document describes the partition-probe diagnostics added before ext4
metadata checksum support. For the current checksum support and its exact
validation scope, see [EXT4-READONLY-V0.2.md](EXT4-READONLY-V0.2.md).

The probe prints the raw superblock magic, `feature_incompat`, and
`feature_ro_compat` masks when it reaches the feature-validation stage.

Common rejection reasons include:

- `could not read superblock...`: block I/O failed or the partition range is
  not readable through the active block device.
- `superblock magic mismatch...`: this partition does not contain an ext4
  superblock at the expected offset; it may be another filesystem or a
  container such as LUKS/LVM/RAID.
- `unsupported EXT4 incompat feature flags`: the ext4 volume uses an
  incompatible feature that this read-only driver does not implement.
- `BIGALLOC feature is unsupported`: the volume uses ext4 bigalloc.
- `invalid ext4 geometry...`: a geometry value is outside the driver's
  currently supported limits.
- `ext4 superblock metadata checksum mismatch`: the superblock checksum did
  not verify.
- `root inode, group descriptor, or inode checksum verification failed`:
  metadata checksum validation failed while loading the root inode.

Build and test:

```sh
make clean
make validate
make iso
```

Runtime verification still needs to be performed on the laptop and in QEMU.
