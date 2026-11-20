#include <stdint.h>
#include <stddef.h>

#include "fat32.h"

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int is_power_of_two(uint32_t value)
{
    return value && ((value & (value - 1U)) == 0);
}

int fat32_mount(struct fat32_fs *fs,
                struct block_device *device,
                const struct partition_entry *partition)
{
    uint8_t boot[BLOCKDEV_SECTOR_SIZE];

    if (!fs || !device || !partition ||
        device->sector_size != BLOCKDEV_SECTOR_SIZE ||
        partition->sector_count == 0 ||
        partition->first_lba >= device->sector_count ||
        partition->sector_count > device->sector_count - partition->first_lba)
        return 0;

    fs->mounted = 0;
    if (!blockdev_read(device, partition->first_lba, 1, boot))
        return 0;
    if (boot[510] != 0x55 || boot[511] != 0xAA)
        return 0;

    uint32_t bytes_per_sector = read_le16(boot + 11);
    uint32_t sectors_per_cluster = boot[13];
    uint32_t reserved_sectors = read_le16(boot + 14);
    uint32_t fat_count = boot[16];
    uint32_t root_entry_count = read_le16(boot + 17);
    uint32_t total_sectors_16 = read_le16(boot + 19);
    uint32_t fat_size_16 = read_le16(boot + 22);
    uint32_t total_sectors = read_le32(boot + 32);
    uint32_t fat_size_32 = read_le32(boot + 36);
    uint16_t extended_flags = read_le16(boot + 40);
    uint16_t filesystem_version = read_le16(boot + 42);
    uint32_t root_cluster = read_le32(boot + 44);
    uint16_t fs_info_sector = read_le16(boot + 48);
    uint16_t backup_boot_sector = read_le16(boot + 50);

    /* This driver intentionally implements the FAT32, 512-byte-sector profile. */
    if (bytes_per_sector != BLOCKDEV_SECTOR_SIZE ||
        !is_power_of_two(sectors_per_cluster) ||
        sectors_per_cluster > 128U || reserved_sectors == 0 ||
        (fat_count != 1U && fat_count != 2U) ||
        root_entry_count != 0 || total_sectors_16 != 0 ||
        fat_size_16 != 0 || total_sectors == 0 || fat_size_32 == 0 ||
        filesystem_version != 0)
        return 0;

    if (total_sectors > partition->sector_count)
        return 0;

    uint64_t fats_total = (uint64_t)fat_count * fat_size_32;
    uint64_t first_data = (uint64_t)reserved_sectors + fats_total;
    if (first_data >= total_sectors)
        return 0;

    uint64_t data_sectors = (uint64_t)total_sectors - first_data;
    uint64_t cluster_count = data_sectors / sectors_per_cluster;

    /* FAT32's cluster-count range is defined by the on-disk specification. */
    if (cluster_count < 65525ULL || cluster_count > 0x0FFFFFF5ULL)
        return 0;

    uint64_t fat_entries = ((uint64_t)fat_size_32 *
                            BLOCKDEV_SECTOR_SIZE) / sizeof(uint32_t);
    if (fat_entries < cluster_count + 2ULL)
        return 0;

    if (root_cluster < 2U || root_cluster >= cluster_count + 2ULL)
        return 0;

    if (fs_info_sector != 0xFFFFU &&
        (fs_info_sector == 0 || fs_info_sector >= reserved_sectors))
        return 0;
    if (backup_boot_sector != 0 && backup_boot_sector != 0xFFFFU &&
        backup_boot_sector >= reserved_sectors)
        return 0;

    /* If mirroring is disabled, the selected active FAT must exist. */
    if (extended_flags & 0x0080U) {
        uint16_t active_fat = extended_flags & 0x000FU;
        if (active_fat >= fat_count)
            return 0;
    }

    uint64_t volume_end = partition->first_lba + total_sectors;
    if (volume_end < partition->first_lba ||
        volume_end > device->sector_count)
        return 0;

    fs->device = device;
    fs->partition = *partition;
    fs->sectors_per_cluster = sectors_per_cluster;
    fs->reserved_sectors = reserved_sectors;
    fs->fat_count = fat_count;
    fs->fat_size_sectors = fat_size_32;
    fs->total_sectors = total_sectors;
    fs->first_data_sector = (uint32_t)first_data;
    fs->cluster_count = (uint32_t)cluster_count;
    fs->root_cluster = root_cluster;
    fs->fs_info_sector = fs_info_sector;
    fs->backup_boot_sector = backup_boot_sector;
    fs->extended_flags = extended_flags;

    for (size_t i = 0; i < 11; i++)
        fs->volume_label[i] = (char)boot[71 + i];
    fs->volume_label[11] = '\0';
    for (size_t i = 11; i > 0; i--) {
        if (fs->volume_label[i - 1] == ' ' ||
            fs->volume_label[i - 1] == '\0')
            fs->volume_label[i - 1] = '\0';
        else
            break;
    }
    fs->mounted = 1;
    return 1;
}

struct fat32_mock_disk {
    uint64_t sectors;
    uint64_t bpb_lba;
    uint8_t boot_sector[BLOCKDEV_SECTOR_SIZE];
};

static int mock_read(struct block_device *device, uint64_t lba,
                     uint32_t count, void *buffer)
{
    struct fat32_mock_disk *mock =
        (struct fat32_mock_disk *)device->context;
    uint8_t *out = (uint8_t *)buffer;

    for (uint32_t s = 0; s < count; s++) {
        uint64_t current = lba + s;
        for (size_t i = 0; i < BLOCKDEV_SECTOR_SIZE; i++)
            out[(size_t)s * BLOCKDEV_SECTOR_SIZE + i] =
                current == mock->bpb_lba ? mock->boot_sector[i] : 0;
    }
    return 1;
}

static void mock_write_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void mock_write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

int fat32_bpb_self_test(void)
{
    struct fat32_mock_disk mock;
    struct block_device device;
    struct partition_entry partition;
    struct fat32_fs fs;
    uint8_t *zero_mock = (uint8_t *)&mock;
    uint8_t *zero_device = (uint8_t *)&device;
    uint8_t *zero_partition = (uint8_t *)&partition;
    uint8_t *zero_fs = (uint8_t *)&fs;
    for (size_t i = 0; i < sizeof(mock); i++) zero_mock[i] = 0;
    for (size_t i = 0; i < sizeof(device); i++) zero_device[i] = 0;
    for (size_t i = 0; i < sizeof(partition); i++) zero_partition[i] = 0;
    for (size_t i = 0; i < sizeof(fs); i++) zero_fs[i] = 0;

    mock.sectors = 71000;
    mock.bpb_lba = 2048;
    device.name = "fat32-bpb-test";
    device.sector_count = mock.sectors;
    device.sector_size = BLOCKDEV_SECTOR_SIZE;
    device.read = mock_read;
    device.write = 0;
    device.context = &mock;

    partition.first_lba = mock.bpb_lba;
    partition.sector_count = 68000;
    partition.scheme = PARTITION_SCHEME_MBR;

    uint8_t *b = mock.boot_sector;
    mock_write_le16(b + 11, 512);
    b[13] = 1;
    mock_write_le16(b + 14, 32);
    b[16] = 2;
    mock_write_le16(b + 17, 0);
    mock_write_le16(b + 19, 0);
    b[21] = 0xF8;
    mock_write_le16(b + 22, 0);
    mock_write_le32(b + 32, 67000);
    mock_write_le32(b + 36, 520);
    mock_write_le16(b + 40, 0);
    mock_write_le16(b + 42, 0);
    mock_write_le32(b + 44, 2);
    mock_write_le16(b + 48, 1);
    mock_write_le16(b + 50, 6);
    for (unsigned i = 0; i < 11; i++)
        b[71 + i] = (uint8_t)"HISPERIA   "[i];
    b[510] = 0x55;
    b[511] = 0xAA;

    if (!fat32_mount(&fs, &device, &partition) || !fs.mounted ||
        fs.sectors_per_cluster != 1 || fs.root_cluster != 2 ||
        fs.cluster_count < 65525U)
        return 0;

    /* A non-power-of-two cluster size is invalid. */
    b[13] = 3;
    if (fat32_mount(&fs, &device, &partition))
        return 0;

    return 1;
}
