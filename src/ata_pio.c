#include <stdint.h>
#include <stddef.h>

#include "ata_pio.h"

#define ATA_DATA          0x1F0U
#define ATA_ERROR         0x1F1U
#define ATA_SECTOR_COUNT  0x1F2U
#define ATA_LBA_LOW       0x1F3U
#define ATA_LBA_MID       0x1F4U
#define ATA_LBA_HIGH      0x1F5U
#define ATA_DRIVE_HEAD    0x1F6U
#define ATA_STATUS        0x1F7U
#define ATA_COMMAND       0x1F7U
#define ATA_ALT_STATUS    0x3F6U
#define ATA_CONTROL       0x3F6U

#define ATA_CMD_IDENTIFY  0xECU
#define ATA_CMD_READ_PIO  0x20U
#define ATA_STATUS_BSY    0x80U
#define ATA_STATUS_DRDY   0x40U
#define ATA_STATUS_DF     0x20U
#define ATA_STATUS_DRQ    0x08U
#define ATA_STATUS_ERR    0x01U
#define ATA_MAX_LBA28     (1ULL << 28)
#define ATA_POLL_LIMIT    1000000U

static uint64_t primary_master_sectors;
static uint16_t identify_words[256];

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void ata_400ns_delay(void)
{
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
}

static int ata_wait_not_busy(uint8_t *status_out)
{
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (status == 0 || status == 0xFFU)
            return 0;
        if (!(status & ATA_STATUS_BSY)) {
            if (status_out)
                *status_out = status;
            return !(status & (ATA_STATUS_ERR | ATA_STATUS_DF));
        }
        __asm__ volatile ("pause");
    }
    return 0;
}

static int ata_wait_drq(void)
{
    for (uint32_t i = 0; i < ATA_POLL_LIMIT; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (status == 0 || status == 0xFFU)
            return 0;
        if (!(status & ATA_STATUS_BSY)) {
            if (status & (ATA_STATUS_ERR | ATA_STATUS_DF))
                return 0;
            if (status & ATA_STATUS_DRQ)
                return 1;
        }
        __asm__ volatile ("pause");
    }
    return 0;
}

static int ata_read_sectors(struct block_device *device, uint64_t lba,
                            uint32_t count, void *buffer)
{
    if (!device || !buffer || !count ||
        lba >= primary_master_sectors ||
        (uint64_t)count > primary_master_sectors - lba ||
        lba >= ATA_MAX_LBA28 ||
        (uint64_t)count > ATA_MAX_LBA28 - lba)
        return 0;

    uint8_t *out = (uint8_t *)buffer;
    uint32_t remaining = count;

    while (remaining) {
        uint32_t chunk = remaining > 255U ? 255U : remaining;
        uint64_t current_lba = lba;

        if (!ata_wait_not_busy(0))
            return 0;

        outb(ATA_DRIVE_HEAD,
             (uint8_t)(0xE0U | ((current_lba >> 24) & 0x0FU)));
        ata_400ns_delay();
        outb(ATA_SECTOR_COUNT, (uint8_t)chunk);
        outb(ATA_LBA_LOW, (uint8_t)current_lba);
        outb(ATA_LBA_MID, (uint8_t)(current_lba >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(current_lba >> 16));
        outb(ATA_COMMAND, ATA_CMD_READ_PIO);

        for (uint32_t sector = 0; sector < chunk; sector++) {
            if (!ata_wait_drq())
                return 0;

            uint16_t *words = (uint16_t *)(void *)out;
            for (unsigned word = 0; word < 256; word++)
                words[word] = inw(ATA_DATA);
            out += BLOCKDEV_SECTOR_SIZE;
        }

        lba += chunk;
        remaining -= chunk;
    }

    return 1;
}

int ata_pio_init_primary_master(struct block_device *device)
{
    if (!device)
        return 0;

    primary_master_sectors = 0;
    outb(ATA_CONTROL, 0x02U); /* Disable device IRQ; use polling. */
    outb(ATA_DRIVE_HEAD, 0xA0U); /* Primary master. */
    ata_400ns_delay();
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_STATUS);
    if (status == 0 || status == 0xFFU)
        return 0;

    if (!ata_wait_not_busy(&status) ||
        inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0 ||
        !ata_wait_drq())
        return 0;

    for (unsigned i = 0; i < 256; i++)
        identify_words[i] = inw(ATA_DATA);

    if (!(identify_words[49] & (1U << 9)))
        return 0; /* Require LBA support. */

    primary_master_sectors =
        (uint64_t)identify_words[60] |
        ((uint64_t)identify_words[61] << 16);
    if (!primary_master_sectors || primary_master_sectors > ATA_MAX_LBA28)
        primary_master_sectors = ATA_MAX_LBA28;

    device->name = "ata0-master";
    device->sector_count = primary_master_sectors;
    device->sector_size = BLOCKDEV_SECTOR_SIZE;
    device->read = ata_read_sectors;
    device->write = 0;
    device->context = 0;
    return 1;
}
