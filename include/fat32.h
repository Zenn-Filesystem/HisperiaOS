#ifndef HISPERIA_FAT32_H
#define HISPERIA_FAT32_H

#include <stdint.h>
#include <stddef.h>
#include "blockdev.h"
#include "partition.h"

struct fat32_fs {
    struct block_device *device;
    struct partition_entry partition;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t fat_size_sectors;
    uint32_t total_sectors;
    uint32_t first_data_sector;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint16_t extended_flags;
    char volume_label[12];
    uint8_t mounted;
};

int fat32_mount(struct fat32_fs *fs,
                struct block_device *device,
                const struct partition_entry *partition);
int fat32_bpb_self_test(void);

#endif
