#include <stdint.h>
#include <stddef.h>

#include "partition.h"

#define MBR_SIGNATURE          0xAA55U
#define MBR_PARTITION_OFFSET   446U
#define MBR_ENTRY_SIZE         16U
#define GPT_HEADER_MIN_SIZE    92U
#define GPT_MAX_ENTRY_COUNT    128U
#define GPT_MAX_ENTRY_SIZE     512U
#define GPT_BUFFER_SIZE        (GPT_MAX_ENTRY_COUNT * GPT_MAX_ENTRY_SIZE)

static uint8_t gpt_entry_buffer[GPT_BUFFER_SIZE];

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void write_le64(uint8_t *p, uint64_t value)
{
    write_le32(p, (uint32_t)value);
    write_le32(p + 4, (uint32_t)(value >> 32));
}

static int guid_is_zero(const uint8_t *guid)
{
    for (size_t i = 0; i < 16; i++) {
        if (guid[i] != 0)
            return 0;
    }
    return 1;
}

static uint32_t crc32_bytes(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }

    return ~crc;
}

static int append_partition(struct partition_entry *out,
                             size_t capacity, size_t *count,
                             const struct partition_entry *entry)
{
    if (*count >= capacity)
        return 1;
    out[*count] = *entry;
    (*count)++;
    return 1;
}

static int scan_gpt(struct block_device *device,
                    struct partition_entry *entries,
                    size_t capacity, size_t *count)
{
    uint8_t header[BLOCKDEV_SECTOR_SIZE];
    uint8_t crc_header[BLOCKDEV_SECTOR_SIZE];

    if (!blockdev_read(device, 1, 1, header))
        return 0;

    static const uint8_t signature[8] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    for (size_t i = 0; i < sizeof(signature); i++) {
        if (header[i] != signature[i])
            return 0;
    }

    uint32_t header_size = read_le32(header + 12);
    if (header_size < GPT_HEADER_MIN_SIZE ||
        header_size > BLOCKDEV_SECTOR_SIZE)
        return 0;

    uint32_t expected_header_crc = read_le32(header + 16);
    for (size_t i = 0; i < sizeof(crc_header); i++)
        crc_header[i] = header[i];
    for (size_t i = 16; i < 20; i++)
        crc_header[i] = 0;
    if (crc32_bytes(crc_header, header_size) != expected_header_crc)
        return 0;

    uint64_t current_lba = read_le64(header + 24);
    uint64_t backup_lba = read_le64(header + 32);
    uint64_t first_usable = read_le64(header + 40);
    uint64_t last_usable = read_le64(header + 48);
    uint64_t entries_lba = read_le64(header + 72);
    uint32_t entry_count = read_le32(header + 80);
    uint32_t entry_size = read_le32(header + 84);
    uint32_t expected_entries_crc = read_le32(header + 88);

    if (current_lba != 1 || backup_lba >= device->sector_count ||
        backup_lba == current_lba || first_usable > last_usable ||
        last_usable >= device->sector_count ||
        entry_count == 0 || entry_count > GPT_MAX_ENTRY_COUNT ||
        entry_size < 128 || entry_size > GPT_MAX_ENTRY_SIZE ||
        (entry_size & 7U) != 0 || entries_lba < 2)
        return 0;

    uint64_t bytes = (uint64_t)entry_count * entry_size;
    if (bytes > GPT_BUFFER_SIZE)
        return 0;

    uint64_t sectors = (bytes + BLOCKDEV_SECTOR_SIZE - 1U) /
                       BLOCKDEV_SECTOR_SIZE;
    if (entries_lba >= device->sector_count ||
        sectors > device->sector_count - entries_lba ||
        entries_lba + sectors > first_usable)
        return 0;

    uint32_t sectors_left = (uint32_t)sectors;
    uint64_t lba = entries_lba;
    size_t byte_offset = 0;
    while (sectors_left) {
        uint32_t chunk = sectors_left > 32U ? 32U : sectors_left;
        if (!blockdev_read(device, lba, chunk,
                           gpt_entry_buffer + byte_offset))
            return 0;
        lba += chunk;
        sectors_left -= chunk;
        byte_offset += (size_t)chunk * BLOCKDEV_SECTOR_SIZE;
    }

    if (crc32_bytes(gpt_entry_buffer, (size_t)bytes) !=
        expected_entries_crc)
        return 0;

    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *raw = gpt_entry_buffer +
            ((size_t)i * entry_size);
        if (guid_is_zero(raw))
            continue;

        uint64_t first = read_le64(raw + 32);
        uint64_t last = read_le64(raw + 40);
        if (first > last || first < first_usable || last > last_usable)
            return 0;

        struct partition_entry result = {0};
        result.first_lba = first;
        result.sector_count = last - first + 1U;
        result.scheme = PARTITION_SCHEME_GPT;
        for (size_t j = 0; j < 16; j++) {
            result.type_guid[j] = raw[j];
            result.unique_guid[j] = raw[16 + j];
        }
        (void)append_partition(entries, capacity, count, &result);
    }

    return 1;
}

int partition_scan(struct block_device *device,
                   struct partition_entry *entries,
                   size_t entry_capacity,
                   size_t *entry_count)
{
    uint8_t mbr[BLOCKDEV_SECTOR_SIZE];

    if (!device || !entries || !entry_count || entry_capacity == 0 ||
        device->sector_size != BLOCKDEV_SECTOR_SIZE ||
        device->sector_count < 2)
        return 0;

    *entry_count = 0;
    if (!blockdev_read(device, 0, 1, mbr) ||
        read_le16(mbr + 510) != MBR_SIGNATURE)
        return 0;

    int protective_gpt = 0;
    for (unsigned i = 0; i < 4; i++) {
        const uint8_t *raw = mbr + MBR_PARTITION_OFFSET +
            (i * MBR_ENTRY_SIZE);
        uint8_t type = raw[4];
        uint32_t first = read_le32(raw + 8);
        uint32_t sectors = read_le32(raw + 12);

        if (type == 0xEEU) {
            protective_gpt = 1;
            continue;
        }
        if (type == 0 || sectors == 0)
            continue;
        if (first == 0 || first >= device->sector_count ||
            (uint64_t)sectors > device->sector_count - first)
            return 0;

        struct partition_entry result = {0};
        result.first_lba = first;
        result.sector_count = sectors;
        result.scheme = PARTITION_SCHEME_MBR;
        result.mbr_type = type;
        (void)append_partition(entries, entry_capacity,
                               entry_count, &result);
    }

    if (protective_gpt) {
        *entry_count = 0;
        return scan_gpt(device, entries, entry_capacity, entry_count);
    }

    return 1;
}

static void test_make_mbr(uint8_t *mbr, uint8_t type,
                          uint32_t first, uint32_t sectors)
{
    for (size_t i = 0; i < BLOCKDEV_SECTOR_SIZE; i++)
        mbr[i] = 0;
    uint8_t *entry = mbr + MBR_PARTITION_OFFSET;
    entry[0] = 0;
    entry[1] = 0xFE;
    entry[2] = 0xFF;
    entry[3] = 0xFF;
    entry[4] = type;
    entry[5] = 0xFE;
    entry[6] = 0xFF;
    entry[7] = 0xFF;
    write_le32(entry + 8, first);
    write_le32(entry + 12, sectors);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
}

int partition_self_test(void)
{
    static uint8_t storage[128U * BLOCKDEV_SECTOR_SIZE];
    struct block_device ramdisk;
    struct partition_entry found[8];
    size_t count = 0;

    for (size_t i = 0; i < sizeof(storage); i++)
        storage[i] = 0;
    if (!blockdev_ramdisk_init(&ramdisk, "partition-test", storage, 128))
        return 0;

    test_make_mbr(storage, 0x0CU, 40, 20);
    if (!partition_scan(&ramdisk, found, 8, &count) || count != 1 ||
        found[0].scheme != PARTITION_SCHEME_MBR ||
        found[0].first_lba != 40 || found[0].sector_count != 20 ||
        found[0].mbr_type != 0x0CU)
        return 0;

    /* Construct a small but structurally valid GPT with one partition. */
    for (size_t i = 0; i < sizeof(storage); i++)
        storage[i] = 0;
    test_make_mbr(storage, 0xEEU, 1, 127);

    uint8_t *entry = storage + (2U * BLOCKDEV_SECTOR_SIZE);
    entry[0] = 0xA2; entry[1] = 0xA0; entry[2] = 0xD0; entry[3] = 0xEB;
    entry[4] = 0xE5; entry[5] = 0xB9; entry[6] = 0x33; entry[7] = 0x44;
    entry[8] = 0x87; entry[9] = 0xC0; entry[10] = 0x68; entry[11] = 0xB6;
    entry[12] = 0xB7; entry[13] = 0x26; entry[14] = 0x99; entry[15] = 0xC7;
    for (unsigned i = 0; i < 16; i++)
        entry[16 + i] = (uint8_t)(i + 1U);
    write_le64(entry + 32, 40);
    write_le64(entry + 40, 100);

    uint8_t *header = storage + BLOCKDEV_SECTOR_SIZE;
    const uint8_t sig[8] = {'E','F','I',' ','P','A','R','T'};
    for (size_t i = 0; i < 8; i++)
        header[i] = sig[i];
    write_le32(header + 8, 0x00010000U);
    write_le32(header + 12, GPT_HEADER_MIN_SIZE);
    write_le64(header + 24, 1);
    write_le64(header + 32, 127);
    write_le64(header + 40, 34);
    write_le64(header + 48, 126);
    write_le64(header + 72, 2);
    write_le32(header + 80, GPT_MAX_ENTRY_COUNT);
    write_le32(header + 84, 128);
    write_le32(header + 88,
               crc32_bytes(storage + (2U * BLOCKDEV_SECTOR_SIZE),
                           GPT_MAX_ENTRY_COUNT * 128U));
    write_le32(header + 16, 0);
    write_le32(header + 16, crc32_bytes(header, GPT_HEADER_MIN_SIZE));

    if (!partition_scan(&ramdisk, found, 8, &count) || count != 1 ||
        found[0].scheme != PARTITION_SCHEME_GPT ||
        found[0].first_lba != 40 || found[0].sector_count != 61)
        return 0;

    /* CRC corruption must be rejected, not silently treated as a partition. */
    header[16] ^= 0x01U;
    if (partition_scan(&ramdisk, found, 8, &count))
        return 0;

    return 1;
}
