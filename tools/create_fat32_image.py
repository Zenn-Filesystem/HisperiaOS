#!/usr/bin/env python3
"""Create a sparse MBR-partitioned FAT32 image for HisperiaOS QEMU tests."""

from __future__ import annotations

import shutil
import struct
import subprocess
import sys
from pathlib import Path

SECTOR_SIZE = 512
PARTITION_START = 2048
DEFAULT_SIZE_MIB = 128


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(f"usage: {sys.argv[0]} OUTPUT.img [SIZE_MiB]", file=sys.stderr)
        return 2

    output = Path(sys.argv[1])
    size_mib = int(sys.argv[2]) if len(sys.argv) == 3 else DEFAULT_SIZE_MIB
    if size_mib < 96:
        print("error: use at least 96 MiB so the volume satisfies FAT32 cluster-count limits",
              file=sys.stderr)
        return 2
    if size_mib > 2048:
        print("error: test image size is capped at 2048 MiB", file=sys.stderr)
        return 2

    mkfs_fat = shutil.which("mkfs.fat")
    if not mkfs_fat:
        print("error: mkfs.fat not found; install dosfstools", file=sys.stderr)
        return 1

    total_sectors = size_mib * 1024 * 1024 // SECTOR_SIZE
    partition_sectors = total_sectors - PARTITION_START
    if partition_sectors <= 0 or total_sectors > 0xFFFFFFFF:
        print("error: invalid image size", file=sys.stderr)
        return 2

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as image:
        image.truncate(total_sectors * SECTOR_SIZE)
        mbr = bytearray(SECTOR_SIZE)
        entry_offset = 446
        # One primary FAT32 LBA partition, type 0x0C.
        mbr[entry_offset + 0] = 0x00
        mbr[entry_offset + 1:entry_offset + 4] = b"\xFE\xFF\xFF"
        mbr[entry_offset + 4] = 0x0C
        mbr[entry_offset + 5:entry_offset + 8] = b"\xFE\xFF\xFF"
        struct.pack_into("<II", mbr, entry_offset + 8,
                         PARTITION_START, partition_sectors)
        mbr[510:512] = b"\x55\xAA"
        image.seek(0)
        image.write(mbr)

    command = [
        mkfs_fat, "-F", "32", "-n", "HISPERIA",
        f"--offset={PARTITION_START}", str(output),
        str(partition_sectors // 2),   # mkfs.fat BLOCK-COUNT is in 1 KiB blocks
    ]
    print("[DISK] Creating MBR partition: start LBA", PARTITION_START,
          "sectors", partition_sectors)
    print("[DISK] Formatting FAT32 volume...")
    subprocess.run(command, check=True)
    print(f"[DISK] Created {output} ({size_mib} MiB, FAT32)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(f"error: mkfs.fat failed with exit code {exc.returncode}", file=sys.stderr)
        raise SystemExit(exc.returncode or 1)
