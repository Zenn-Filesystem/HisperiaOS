# HisperiaOS read-only FAT32 file access

The storage path is layered as:

`ATA PIO -> block_device -> MBR/GPT partition -> FAT32 volume -> file lookup/read`

## Implemented

- 512-byte sector block-device interface.
- Legacy primary-master ATA PIO, LBA28, polling, read-only.
- Primary MBR and GPT partition scanning (GPT CRC validation).
- FAT32 BPB/geometry validation and read-only mount.
- FAT32 FAT-entry reads with active-FAT selection when mirroring is disabled.
- Cluster-chain traversal with bounds checks and hop limits.
- Directory traversal across cluster chains.
- Case-insensitive 8.3 short-name path lookup, including subdirectories.
- File reads by offset and length, including reads crossing cluster boundaries.
- No heap allocation is required by the read path; it uses one 512-byte sector buffer.

## API

```c
struct fat32_file file;
size_t bytes_read;
uint8_t buffer[256];

if (fat32_lookup_path(&filesystem, "/DOCS/HELLO.TXT", &file)) {
    if (fat32_read_file(&filesystem, &file, 0,
                        buffer, sizeof(buffer), &bytes_read)) {
        /* bytes_read bytes now contain file data. */
    }
}
```

`fat32_lookup_path()` accepts paths with or without a leading slash. Path
components currently use the FAT 8.3 short-name format; long filename (LFN)
records are skipped. `fat32_read_file()` clamps reads at EOF and reports the
actual byte count through `bytes_read`. It rejects directory and volume-label
entries. The filesystem is read-only: there are no create, delete, rename, or
write operations.

## Tests

```sh
make test-storage-host
make clean && make kernel
make run-fat32
```

The host test builds a synthetic FAT32 volume and verifies a nested
`/DOCS/HELLO.TXT` file whose 700-byte contents span two clusters, including a
random-offset read across the boundary. The QEMU image generator formats a
128 MiB FAT32 partition and inserts `/HELLO.TXT`; on boot the kernel should
print its contents after the FAT32 mount message.

The QEMU target uses a legacy IDE controller because the current hardware
backend is ATA PIO. It does not implement AHCI, NVMe, USB mass storage, LFN,
FAT32 writes, a VFS, or a user-space file syscall yet. Runtime behavior on real
hardware still needs to be confirmed separately from host tests and compilation.
