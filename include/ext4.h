#ifndef HISPERIA_EXT4_H
#define HISPERIA_EXT4_H

#include <stdint.h>
#include "blockdev.h"
#include "partition.h"

struct ext4_fs {
    struct block_device *device;
    struct partition_entry partition;
    uint64_t block_count;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint16_t descriptor_size;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint32_t groups_count;
    uint8_t mounted;
};

int ext4_mount(struct ext4_fs *fs, struct block_device *device,
               const struct partition_entry *partition);
int ext4_self_test(void);

#endif
