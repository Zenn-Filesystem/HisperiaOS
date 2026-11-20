# HisperiaOS ext4 + VFS (read-only v0.1)

This change adds an ext4 read-only filesystem backend behind a generic path-based
VFS dispatcher. The VFS owns mount selection, operation dispatch, and open-file
lifetime; ext4 provides the backend callbacks. It uses the existing `block_device`
interface, so the same backend can read from AHCI SATA, legacy ATA PIO, or a RAM disk.

## VFS API

- `vfs_mount_ext4(fs, device, partition)` — mount one ext4 partition as `/`.
- `vfs_open(path, &file)` — open a regular file by absolute path.
- `vfs_read(&file, buffer, count)` — sequential file reads; returns bytes read,
  `0` at EOF, or `-1` on error.
- `vfs_close(&file)` — close a file handle.
- `vfs_readdir(path, &cursor, &entry)` — enumerate a directory. Initialize
  `cursor` to zero; returns `1` for an entry, `0` at end, `-1` on error.
- `vfs_stat(path, &size, &type)` — obtain basic metadata.

The implementation intentionally mounts one filesystem as the VFS root. The
`vfs_mount(operations, context)` interface is filesystem-agnostic; ext4 currently
registers its callback table through `vfs_mount_ext4()`. FAT32 remains available
through its existing driver but is not yet registered as another VFS backend.

## Supported ext4 profile

- 1 KiB, 2 KiB, and 4 KiB block sizes.
- 128–256 byte inodes.
- Extent-based regular files and directories, including external extent-tree
  nodes and 64-bit physical block addresses.
- FILETYPE, EXTENTS, 64BIT, and FLEX_BG incompat features.
- Read-only mount, directory lookup/listing, file open/stat/read.
- Sparse extents are returned as zero-filled data.

## Deliberate limitations

- Read-only: no create, write, truncate, unlink, rename, or allocation.
- No journal replay. Volumes requiring recovery are rejected rather than modified.
- Metadata and extent checksums are not verified. Use only on a cleanly unmounted
  volume for development/testing; do not treat this as production-safe access.
- No legacy direct/indirect block mapping; files must use extents.
- No symlink following, xattrs, encryption, inline data, compression, bigalloc,
  casefold, or directory indexing acceleration.
- Only one mounted VFS root; no mount namespace. The current Ring 3 test harness has a single global 16-entry file-descriptor table, not per-process descriptor tables.
- `vfs_readdir` uses a 32-bit byte-offset cursor in this initial version.

## Build and tests

```sh
make clean
make kernel
make test-storage-host
make test-vfs-host
make test-ext4-host
```

`test-vfs-host` validates the filesystem-agnostic mount/dispatch layer using a
mock backend. `test-ext4-host` builds a small synthetic ext4 image in memory and
verifies mount, root directory enumeration, path lookup, and file read through VFS.

The kernel boot path probes AHCI first, then legacy ATA PIO if no ext4 root was
mounted. Both transports feed the same `block_device` interface; partition
scanning and ext4/VFS mounting do not depend on the controller. The first
supported ext4 partition is mounted read-only as `/`, then up to eight root
entries are printed. Runtime validation on QEMU and the physical laptop is
still required.
