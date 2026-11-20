# HisperiaOS storage foundation

## Current implementation

The storage layer currently contains:

- A common 512-byte `block_device` interface and RAM-disk test backend.
- Legacy primary-master ATA PIO, polling, LBA28, read-only.
- AHCI SATA, polling READ DMA EXT, read-only.
- MBR/GPT partition discovery, including GPT CRC validation.
- FAT32 BPB/geometry validation and volume-label detection.
- A read-only ext4 backend registered behind generic VFS operations.

FAT32 file/path operations are **not** currently exposed by the source in this
package and FAT32 is not mounted into VFS. Ring 3 file syscalls use the ext4 VFS
root only.

## Cross-transport boot

At boot HisperiaOS probes AHCI first. If it does not mount a supported ext4
partition, it probes the primary-master ATA PIO device and applies the same
partition scanner and ext4/VFS mount path. The upper storage layers consume only
`struct block_device`, so they are independent of the transport driver.

See `CROSS-LOAD.md` for the boot sequence and QEMU test commands, and
`EXT4-VFS-README.md` for the supported ext4 profile and limitations.
