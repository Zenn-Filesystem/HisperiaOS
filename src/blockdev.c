#include <stdint.h>
#include <stddef.h>

#include "blockdev.h"

static int range_valid(const struct block_device *device,
                       uint64_t lba, uint32_t count)
{
    if (!device || !count || !device->sector_count ||
        device->sector_size != BLOCKDEV_SECTOR_SIZE)
        return 0;

    if (lba >= device->sector_count)
        return 0;

    return (uint64_t)count <= device->sector_count - lba;
}

int blockdev_read(struct block_device *device, uint64_t lba,
                  uint32_t sector_count, void *buffer)
{
    if (!buffer || !range_valid(device, lba, sector_count) || !device->read)
        return 0;

    return device->read(device, lba, sector_count, buffer);
}

int blockdev_write(struct block_device *device, uint64_t lba,
                   uint32_t sector_count, const void *buffer)
{
    if (!buffer || !range_valid(device, lba, sector_count) || !device->write)
        return 0;

    return device->write(device, lba, sector_count, buffer);
}

static int ramdisk_read(struct block_device *device, uint64_t lba,
                        uint32_t count, void *buffer)
{
    const uint8_t *source = (const uint8_t *)device->context +
        (size_t)(lba * BLOCKDEV_SECTOR_SIZE);
    uint8_t *destination = (uint8_t *)buffer;
    size_t bytes = (size_t)count * BLOCKDEV_SECTOR_SIZE;

    for (size_t i = 0; i < bytes; i++)
        destination[i] = source[i];
    return 1;
}

static int ramdisk_write(struct block_device *device, uint64_t lba,
                         uint32_t count, const void *buffer)
{
    uint8_t *destination = (uint8_t *)device->context +
        (size_t)(lba * BLOCKDEV_SECTOR_SIZE);
    const uint8_t *source = (const uint8_t *)buffer;
    size_t bytes = (size_t)count * BLOCKDEV_SECTOR_SIZE;

    for (size_t i = 0; i < bytes; i++)
        destination[i] = source[i];
    return 1;
}

int blockdev_ramdisk_init(struct block_device *device, const char *name,
                          void *storage, uint64_t sector_count)
{
    if (!device || !storage || !sector_count ||
        sector_count > (uint64_t)(SIZE_MAX / BLOCKDEV_SECTOR_SIZE))
        return 0;

    device->name = name ? name : "ramdisk";
    device->sector_count = sector_count;
    device->sector_size = BLOCKDEV_SECTOR_SIZE;
    device->read = ramdisk_read;
    device->write = ramdisk_write;
    device->context = storage;
    return 1;
}

int blockdev_ramdisk_self_test(void)
{
    static uint8_t backing[2 * BLOCKDEV_SECTOR_SIZE];
    uint8_t input[BLOCKDEV_SECTOR_SIZE];
    uint8_t output[BLOCKDEV_SECTOR_SIZE];
    struct block_device device;

    for (size_t i = 0; i < sizeof(backing); i++)
        backing[i] = 0;
    for (size_t i = 0; i < sizeof(input); i++)
        input[i] = (uint8_t)((i * 37U) ^ 0xA5U);

    if (!blockdev_ramdisk_init(&device, "ramtest", backing, 2))
        return 0;
    if (!blockdev_write(&device, 1, 1, input))
        return 0;
    if (!blockdev_read(&device, 1, 1, output))
        return 0;

    for (size_t i = 0; i < sizeof(input); i++) {
        if (input[i] != output[i])
            return 0;
    }

    if (blockdev_read(&device, 2, 1, output))
        return 0;
    if (blockdev_write(&device, 0, 0, input))
        return 0;

    return 1;
}
