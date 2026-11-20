# HisperiaOS cross-transport storage loading

The boot-time storage path now supports the same filesystem/VFS loading path
over either legacy ATA PIO or AHCI SATA. The controller driver only supplies a
`struct block_device`; the partition parser, ext4 backend, generic VFS, and Ring 3
syscalls do not know which transport supplied the sectors.

## Boot order

1. Probe PCI for an AHCI controller. If a SATA disk is found, register it as a
   block device and scan its MBR/GPT partition table.
2. Probe partitions for a FAT32 BPB (diagnostic only) and supported ext4 volumes.
3. If ext4 mounts, register it as the generic VFS root and stop probing.
4. If no ext4 root was mounted through AHCI, probe the primary-master ATA PIO
   device and run the exact same partition/filesystem path.
5. Ring 3 `stat`, `readdir`, `open`, `read`, `seek`, and `close` calls use the
   mounted VFS root, regardless of whether AHCI or ATA supplied the disk.

The block devices are static for the lifetime of the kernel because the ext4
filesystem and VFS retain their pointers after mount. The first supported ext4
partition encountered becomes the single read-only VFS root.

## QEMU checks

```sh
# Legacy IDE / ATA PIO transport
make clean && make run-fat32

# ICH9 AHCI / SATA transport
make clean && make run-sata
```

Both targets currently attach the generated MBR/FAT32 test disk. They validate
controller discovery, block reads, partition parsing, and FAT32 BPB detection.
They do **not** mount a VFS root because FAT32 is not yet registered as a VFS
backend. To verify Ring 3 file reads in QEMU, attach a supported ext4 partition
over the transport being tested. On the physical laptop, the maintainer has
reported successful AHCI disk detection and partition enumeration; the new
AHCI-to-VFS and ATA-to-VFS boot flows still require runtime verification.

## Validation

`make validate` runs the RAM-disk/block/partition/FAT32 host tests, generic VFS
dispatch tests, synthetic ext4+VFS integration tests, and three kernel
compile/link configurations. Hardware/QEMU execution is a separate test.

## EXT4 probe diagnostics

The boot log now prints each candidate partition's index, starting LBA,
sector count, and MBR type before probing ext4. A rejected candidate reports a
specific reason such as unreadable superblock, magic mismatch, unsupported
incompat feature flags, invalid geometry, or unreadable root inode. The raw
superblock magic and incompat/ro-compat feature masks are printed as hex too. This helps
distinguish a partition-discovery problem from an ext4-driver compatibility
problem without assuming that a visible FAT32 partition is the only partition
on the disk.

The driver remains deliberately conservative: it mounts ext4 read-only only
when its currently supported feature subset and root inode checks pass. A
partition containing LUKS, LVM, Linux software RAID, or another container
format is not itself a directly mountable ext4 filesystem.
