#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "ext4.h"
#include "vfs.h"
#include "blockdev.h"

#define TEST_SEED 0xA1B2C3D4U
#define TEST_BLOCK_SIZE 1024U
#define TEST_INODE_SIZE 256U

static uint8_t disk[64U * 1024U];

void *kmalloc(size_t n) { return __builtin_malloc(n); }
void kfree(void *p) { __builtin_free(p); }

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t crc32c(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *p = (const uint8_t *)data;
    while (length--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0x82F63B78U & (0U - (crc & 1U)));
    }
    return crc;
}

static uint32_t inode_seed(uint32_t ino, uint32_t generation)
{
    uint8_t le_ino[4], le_generation[4];
    put32(le_ino, ino);
    put32(le_generation, generation);
    return crc32c(crc32c(TEST_SEED, le_ino, 4), le_generation, 4);
}

static void put_extent(uint8_t *p, uint32_t logical, uint16_t len,
                       uint32_t physical)
{
    put16(p, 0xF30A);
    put16(p + 2, 1);
    put16(p + 4, 4);
    put16(p + 6, 0);
    put32(p + 8, 0);
    put32(p + 12, logical);
    put16(p + 16, len);
    put16(p + 18, 0);
    put32(p + 20, physical);
}

static void make_inode(uint8_t *p, uint16_t mode, uint32_t size,
                       uint32_t block)
{
    put16(p, mode);
    put32(p + 4, size);
    put32(p + 32, 0x00080000U);
    put_extent(p + 40, 0, 1, block);
    put32(p + 100, 0); /* generation */
    put16(p + 128, 32); /* i_extra_isize: enables 32-bit inode checksum */
}

static void make_extent_tree_inode(uint8_t *p, uint16_t mode, uint32_t size,
                                   uint32_t extent_node_block)
{
    put16(p, mode);
    put32(p + 4, size);
    put32(p + 32, 0x00080000U);
    put16(p + 40, 0xF30A);
    put16(p + 42, 1);
    put16(p + 44, 4);
    put16(p + 46, 1); /* one external extent-tree level */
    put32(p + 48, 0);
    put32(p + 52, 0); /* logical block 0 */
    put32(p + 56, extent_node_block);
    put16(p + 60, 0);
    put32(p + 100, 0); /* generation */
    put16(p + 128, 32); /* i_extra_isize: enables 32-bit inode checksum */
}

static void seal_inode(uint8_t *inode, uint32_t ino)
{
    static const uint8_t zero[2] = {0, 0};
    uint8_t ino_le[4];
    uint32_t crc;

    put32(ino_le, ino);
    crc = crc32c(TEST_SEED, ino_le, sizeof(ino_le));
    crc = crc32c(crc, inode + 100U, 4U); /* i_generation */
    crc = crc32c(crc, inode, 124U);
    crc = crc32c(crc, zero, sizeof(zero)); /* i_checksum_lo */
    crc = crc32c(crc, inode + 126U, 4U); /* bytes 126..129 */
    crc = crc32c(crc, zero, sizeof(zero)); /* i_checksum_hi */
    crc = crc32c(crc, inode + 132U, TEST_INODE_SIZE - 132U);
    put16(inode + 124U, (uint16_t)crc);
    put16(inode + 130U, (uint16_t)(crc >> 16));
}

static void seal_group_desc(uint8_t *desc, size_t descriptor_size)
{
    static const uint8_t zero[2] = {0, 0};
    static const uint8_t group_zero[4] = {0, 0, 0, 0};
    uint32_t crc = crc32c(TEST_SEED, group_zero, sizeof(group_zero));
    crc = crc32c(crc, desc, 30);
    crc = crc32c(crc, zero, sizeof(zero));
    if (descriptor_size > 32U)
        crc = crc32c(crc, desc + 32U, descriptor_size - 32U);
    put16(desc + 30, (uint16_t)crc);
}

static void seal_directory(uint8_t *block, uint32_t dir_ino)
{
    uint8_t *tail = block + TEST_BLOCK_SIZE - 12U;
    uint32_t crc;

    put32(tail, 0);
    put16(tail + 4, 12);
    tail[6] = 0;
    tail[7] = 0xDE;
    crc = crc32c(inode_seed(dir_ino, 0), block, TEST_BLOCK_SIZE - 12U);
    put32(tail + 8, crc);
}

static void seal_extent_block(uint8_t *block, uint32_t ino)
{
    uint32_t crc;
    put16(block, 0xF30A);
    put16(block + 2, 1);
    put16(block + 4, 84); /* 1 KiB block with four-byte extent tail */
    put16(block + 6, 0);
    put32(block + 8, 0);
    put32(block + 12, 0); /* logical block 0 */
    put16(block + 16, 1); /* one initialized block */
    put16(block + 18, 0);
    put32(block + 20, 41); /* physical data block */
    crc = crc32c(inode_seed(ino, 0), block, TEST_BLOCK_SIZE - 4U);
    put32(block + TEST_BLOCK_SIZE - 4U, crc);
}

static void seal_dx_root(uint8_t *block, uint32_t dir_ino)
{
    const uint32_t count_offset = 32U;
    const uint16_t limit = (TEST_BLOCK_SIZE - count_offset - 8U) / 8U;
    uint8_t *tail;
    uint32_t crc;

    /* '.' and '..' followed by dx_root_info and one dx entry. */
    put32(block, dir_ino);
    put16(block + 4, 12);
    block[6] = 1; block[7] = 2; block[8] = '.';
    put32(block + 12, dir_ino); /* root's parent in this fixture */
    put16(block + 16, TEST_BLOCK_SIZE - 12U);
    block[18] = 2; block[19] = 2;
    block[20] = '.'; block[21] = '.';
    put32(block + 24, 0); /* reserved_zero */
    block[28] = 0;        /* hash version */
    block[29] = 8;        /* info_length */
    block[30] = 0;        /* indirect_levels */
    block[31] = 0;
    put16(block + count_offset, limit);
    put16(block + count_offset + 2U, 1);
    put32(block + count_offset + 4U, 1); /* leaf is logical block 1 */

    tail = block + count_offset + (uint32_t)limit * 8U;
    put32(tail, 0);
    crc = crc32c(inode_seed(dir_ino, 0), block, count_offset + 8U);
    crc = crc32c(crc, tail, 4U);
    crc = crc32c(crc, (const uint8_t[4]){0, 0, 0, 0}, 4U);
    put32(tail + 4U, crc);
}

static void build_metadata_csum_image(void)
{
    uint8_t *sb = disk + 1024U;
    uint8_t *gdt = disk + 2U * TEST_BLOCK_SIZE;
    uint8_t *root_inode = disk + 8U * TEST_BLOCK_SIZE + TEST_INODE_SIZE;
    uint8_t *file_inode = disk + 8U * TEST_BLOCK_SIZE + 2U * TEST_INODE_SIZE;
    uint8_t *dx_root = disk + 42U * TEST_BLOCK_SIZE;
    uint8_t *dir_leaf = disk + 43U * TEST_BLOCK_SIZE;
    uint8_t *extent_node = disk + 40U * TEST_BLOCK_SIZE;

    for (size_t i = 0; i < sizeof(disk); i++)
        disk[i] = 0;

    /* 1 KiB blocks; one group; 256-byte inodes and 64-byte 64BIT GDT. */
    put32(sb, 128);                 /* inodes_count */
    put32(sb + 4, 64);              /* blocks_count_lo */
    put32(sb + 20, 1);              /* first_data_block */
    put32(sb + 24, 0);              /* log_block_size */
    put32(sb + 32, 63);             /* blocks_per_group */
    put32(sb + 40, 128);            /* inodes_per_group */
    put16(sb + 56, 0xEF53);         /* magic */
    put32(sb + 96, 0x22C2);         /* FILETYPE | EXTENTS | 64BIT | FLEX_BG | CSUM_SEED */
    put32(sb + 100, 0x046B);        /* sparse/large/huge/dir_nlink/extra_isize/metadata_csum */
    put16(sb + 88, TEST_INODE_SIZE);
    put16(sb + 254, 64);            /* 64-byte descriptor for 64BIT */
    sb[0x175] = 1;                  /* crc32c checksum algorithm */
    put32(sb + 0x270, TEST_SEED);   /* checksum seed */
    for (unsigned i = 0; i < 16; i++)
        sb[0x68 + i] = (uint8_t)(0x30U + i);
    put32(sb + 0x3FC, crc32c(0xFFFFFFFFU, sb, 0x3FC));

    put32(gdt + 8, 8);              /* group 0 inode table block */
    seal_group_desc(gdt, 64U);

    /* Root is an indexed directory with a checksummed dx root + one leaf. */
    make_inode(root_inode, 0x41ED, 2U * TEST_BLOCK_SIZE, 42);
    put32(root_inode + 32, 0x00080000U | 0x00001000U); /* EXTENTS | INDEX */
    put16(root_inode + 56, 2);      /* root extent covers logical blocks 0..1 */
    seal_inode(root_inode, 2);

    /* File data uses an external, checksummed extent tree node. */
    make_extent_tree_inode(file_inode, 0x81A4, 5, 40);
    seal_inode(file_inode, 3);
    seal_dx_root(dx_root, 2);

    put32(dir_leaf, 3);
    put16(dir_leaf + 4, TEST_BLOCK_SIZE - 12U);
    dir_leaf[6] = 9;
    dir_leaf[7] = 1;
    for (unsigned i = 0; i < 9; i++)
        dir_leaf[8 + i] = (uint8_t)"hello.txt"[i];
    seal_directory(dir_leaf, 2);

    seal_extent_block(extent_node, 3);
    for (unsigned i = 0; i < 5; i++)
        disk[41U * TEST_BLOCK_SIZE + i] = (uint8_t)"hello"[i];
}

int main(void)
{
    struct block_device dev = {0};
    struct partition_entry part = {0};
    struct ext4_fs fs = {0};
    struct ext4_fs probe = {0};
    struct vfs_file file = {0};
    struct vfs_dirent ent = {0};
    uint32_t cursor = 0;
    uint8_t saved;
    uint8_t saved_hi;

    build_metadata_csum_image();
    if (!blockdev_ramdisk_init(&dev, "ext4-metadata-csum-test", disk, 128))
        return 1;
    part.first_lba = 0;
    part.sector_count = 128;
    part.scheme = PARTITION_SCHEME_MBR;

    if (!ext4_self_test()) {
        puts("CRC32C known-vector test FAIL");
        return 1;
    }
    puts("CRC32C known-vector test PASS");

    /* Superblock checksum must be checked before geometry is trusted. */
    saved = disk[1024U + 100U];
    disk[1024U + 100U] ^= 0x01U;
    if (ext4_mount(&probe, &dev, &part) ||
        !strstr(ext4_last_error(), "superblock metadata checksum mismatch")) {
        puts("superblock checksum rejection FAIL");
        return 1;
    }
    disk[1024U + 100U] = saved;
    puts("superblock checksum rejection PASS");

    /* The GDT checksum must be checked before using its inode-table pointer. */
    saved = disk[2U * TEST_BLOCK_SIZE + 30U];
    disk[2U * TEST_BLOCK_SIZE + 30U] ^= 0x01U;
    if (ext4_mount(&probe, &dev, &part)) {
        puts("group descriptor checksum rejection FAIL");
        return 1;
    }
    disk[2U * TEST_BLOCK_SIZE + 30U] = saved;
    puts("group descriptor checksum rejection PASS");

    /* Root inode checksum must be checked before interpreting inode fields. */
    saved = disk[8U * TEST_BLOCK_SIZE + TEST_INODE_SIZE + 124U];
    disk[8U * TEST_BLOCK_SIZE + TEST_INODE_SIZE + 124U] ^= 0x01U;
    if (ext4_mount(&probe, &dev, &part)) {
        puts("inode checksum rejection FAIL");
        return 1;
    }
    disk[8U * TEST_BLOCK_SIZE + TEST_INODE_SIZE + 124U] = saved;
    puts("inode checksum low-field rejection PASS");

    saved_hi = disk[8U * TEST_BLOCK_SIZE + TEST_INODE_SIZE + 130U];
    disk[8U * TEST_BLOCK_SIZE + TEST_INODE_SIZE + 130U] ^= 0x01U;
    if (ext4_mount(&probe, &dev, &part)) {
        puts("inode checksum high-field rejection FAIL");
        return 1;
    }
    disk[8U * TEST_BLOCK_SIZE + TEST_INODE_SIZE + 130U] = saved_hi;
    puts("inode checksum high-field rejection PASS");

    if (!vfs_mount_ext4(&fs, &dev, &part)) {
        printf("metadata_csum ext4 mount FAIL: %s\n", ext4_last_error());
        return 1;
    }
    if (vfs_readdir("/", &cursor, &ent) != 1 ||
        ent.inode != 3 || strcmp(ent.name, "hello.txt") != 0) {
        puts("metadata_csum HTree readdir FAIL");
        return 1;
    }
    if (!vfs_open("/hello.txt", &file)) {
        puts("metadata_csum open FAIL");
        return 1;
    }
    {
        char out[6] = {0};
        if (vfs_read(&file, out, 5) != 5 || out[0] != 'h' ||
            out[1] != 'e' || out[2] != 'l' || out[3] != 'l' ||
            out[4] != 'o') {
            puts("metadata_csum read FAIL");
            return 1;
        }
    }
    vfs_close(&file);
    puts("metadata_csum + CSUM_SEED + HTree + extent-tree VFS read PASS");

    /* The external extent-tree checksum must be checked during file reads. */
    if (!vfs_open("/hello.txt", &file)) {
        puts("extent checksum test open FAIL");
        return 1;
    }
    disk[40U * TEST_BLOCK_SIZE + TEST_BLOCK_SIZE - 1U] ^= 0x80U;
    {
        char out[6] = {0};
        if (vfs_read(&file, out, 5) >= 0) {
            puts("extent checksum rejection FAIL");
            return 1;
        }
    }
    disk[40U * TEST_BLOCK_SIZE + TEST_BLOCK_SIZE - 1U] ^= 0x80U;
    vfs_close(&file);
    puts("extent-tree checksum rejection PASS");

    /* HTree root and directory leaf checksums are both mandatory. */
    disk[42U * TEST_BLOCK_SIZE + TEST_BLOCK_SIZE - 1U] ^= 0x80U;
    if (vfs_open("/hello.txt", &file)) {
        puts("HTree root checksum rejection FAIL");
        return 1;
    }
    disk[42U * TEST_BLOCK_SIZE + TEST_BLOCK_SIZE - 1U] ^= 0x80U;
    puts("HTree root checksum rejection PASS");

    disk[43U * TEST_BLOCK_SIZE + TEST_BLOCK_SIZE - 1U] ^= 0x80U;
    if (vfs_open("/hello.txt", &file)) {
        puts("directory leaf checksum rejection FAIL");
        return 1;
    }
    disk[43U * TEST_BLOCK_SIZE + TEST_BLOCK_SIZE - 1U] ^= 0x80U;
    puts("directory leaf checksum rejection PASS");
    return 0;
}
