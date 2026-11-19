#ifndef HISPERIA_BLOCKDEV_H
#define HISPERIA_BLOCKDEV_H

#include <stdint.h>
#include <stddef.h>

#define BLOCKDEV_SECTOR_SIZE 512U

struct block_device;

typedef int (*blockdev_read_fn)(struct block_device *device,
                                uint64_t lba,
                                uint32_t sector_count,
                                void *buffer);
typedef int (*blockdev_write_fn)(struct block_device *device,
                                 uint64_t lba,
                                 uint32_t sector_count,
                                 const void *buffer);

struct block_device {
    const char *name;
    uint64_t sector_count;
    uint32_t sector_size;
    blockdev_read_fn read;
    blockdev_write_fn write;
    void *context;
};

int blockdev_read(struct block_device *device, uint64_t lba,
                  uint32_t sector_count, void *buffer);
int blockdev_write(struct block_device *device, uint64_t lba,
                   uint32_t sector_count, const void *buffer);
int blockdev_ramdisk_init(struct block_device *device, const char *name,
                          void *storage, uint64_t sector_count);
int blockdev_ramdisk_self_test(void);

#endif
