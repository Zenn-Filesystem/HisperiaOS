# HisperiaOS ext4 read-only compatibility v0.2

This update adds a narrowly scoped implementation of ext4 metadata checksums
and `CSUM_SEED` support. It is intended to read Linux ext4 volumes safely when
their required on-disk features match the driver's supported profile. It is not
a general-purpose or write-capable ext4 implementation.

## Implemented checksum handling

- CRC32C using the Castagnoli polynomial and Linux/ext4 raw accumulator
  convention.
- Superblock checksum and CRC32C checksum-type validation.
- Metadata checksum seed from `s_checksum_seed` when `CSUM_SEED` is enabled;
  otherwise derive the seed from the filesystem UUID.
- Group descriptor checksum verification before using the inode-table pointer.
- Linux inode checksum verification, including the high checksum field when
  `i_extra_isize` exposes it.
- External extent-tree block checksum verification.
- Directory leaf checksum verification.
- HTree root and interior-node checksum verification. Lookup/readdir currently
  scans directory logical blocks linearly and validates index blocks; it does
  not implement the ext4 HTree hash traversal algorithm.

## Host validation

`make validate` runs the synthetic ext4 + VFS fixture. The fixture uses:

- 1 KiB filesystem blocks, 256-byte inodes, and 64-byte group descriptors;
- the same `feature_incompat` mask observed on the laptop (`0x22C2`, including
  `64BIT`, `FLEX_BG`, and `CSUM_SEED`) and `feature_ro_compat` mask `0x046B`;
- a checksummed group descriptor and superblock;
- 32-bit inode checksums (`i_checksum_hi`), with independent low/high-field
  corruption tests;
- an indexed root directory with a checksummed HTree root and leaf;
- an external checksummed extent-tree block; and
- a VFS open/read of `/hello.txt`.

The test also corrupts the superblock, group descriptor, inode low/high fields,
extent-tree block, HTree root, and directory leaf checksum values independently
and expects the relevant operation to reject the metadata. Run the host test under sanitizers with:

```sh
cc -std=c11 -O1 -g -Wall -Wextra -fno-omit-frame-pointer \
  -fsanitize=address,undefined -Iinclude \
  tools/ext4_host_test.c src/blockdev.c src/ext4.c src/vfs.c \
  -o /tmp/ext4-host-asan
/tmp/ext4-host-asan
```

## Current limitations

- Read-only only. No journal replay and no filesystem writes.
- No guarantee of compatibility with every ext4 feature combination. Unsupported
  incompat features, bigalloc, unsupported block/inode/descriptor geometry, and
  checksum algorithms other than CRC32C are rejected.
- The metadata checksum coverage is limited to metadata read by this driver:
  superblocks, group descriptors, inodes, external extent-tree blocks, and
  directory leaf/HTree blocks. It does not yet verify every ext4 metadata type
  (for example allocation bitmaps) because the current read path does not use
  them.
- Indexed directory lookup currently scans logical blocks linearly instead
  of following HTree hashes. Large directories may therefore be slow.
- The synthetic host fixture is not a substitute for testing against a real
  Linux-created ext4 image. Runtime AHCI/ATA + VFS + Ring 3 file access still
  needs to be verified on QEMU and the target laptop.

## Build and test

```sh
make clean
make validate
make iso
```

Do not reformat or modify the laptop's Linux partition for this test. Prefer a
separate disposable ext4 disk image for initial runtime validation.
