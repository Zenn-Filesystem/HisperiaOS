#include <stdint.h>
#include <stddef.h>
#include "ext4.h"
#include "vfs.h"
#include "heap.h"

#define EXT4_SUPER_MAGIC 0xEF53U
#define EXT4_EXTENTS_FL  0x00080000U
#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define EXT4_FEATURE_INCOMPAT_EXTENTS  0x0040U
#define EXT4_FEATURE_INCOMPAT_64BIT    0x0080U
#define EXT4_FEATURE_INCOMPAT_FLEX_BG  0x0200U
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED 0x2000U
#define EXT4_FEATURE_INCOMPAT_SUPPORTED (EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_CSUM_SEED)
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM 0x0400U
#define EXT4_INDEX_FL 0x00001000U
#define EXT4_CRC32C_CHKSUM 1U
#define EXT4_EXTENT_MAGIC 0xF30AU
#define EXT4_ROOT_INO 2U
#define EXT4_MAX_BLOCK_SIZE 4096U
#define EXT4_MAX_EXTENT_DEPTH 5U

static const char *ext4_error_text = "not attempted";
static uint16_t ext4_probe_magic;
static uint32_t ext4_probe_incompat;
static uint32_t ext4_probe_ro_compat;

uint16_t ext4_last_magic(void) { return ext4_probe_magic; }
uint32_t ext4_last_incompat_features(void) { return ext4_probe_incompat; }
uint32_t ext4_last_ro_compat_features(void) { return ext4_probe_ro_compat; }

const char *ext4_last_error(void)
{
    return ext4_error_text;
}

struct ext4_inode {
    uint32_t number;
    uint32_t generation;
    uint16_t mode;
    uint32_t flags;
    uint64_t size;
    uint8_t block[60];
};

struct ext4_extent_header {
    uint16_t magic, entries, max, depth;
    uint32_t generation;
} __attribute__((packed));
struct ext4_extent {
    uint32_t logical;
    uint16_t length, start_hi;
    uint32_t start_lo;
} __attribute__((packed));
struct ext4_extent_idx {
    uint32_t logical, leaf_lo;
    uint16_t leaf_hi, unused;
} __attribute__((packed));

static int ext4_read_block(struct ext4_fs *fs, uint64_t block, void *buffer);
static int ext4_inode_block(struct ext4_fs *fs, const struct ext4_inode *inode,
                            uint32_t logical, uint64_t *physical,
                            uint32_t *available);

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void bytes_copy(void *dst, const void *src, size_t n) { uint8_t *d=dst; const uint8_t *s=src; for(size_t i=0;i<n;i++) d[i]=s[i]; }
static int bytes_equal(const char *a, const char *b, size_t n) { for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static size_t str_len(const char *s) { size_t n=0; if(!s) return 0; while(s[n]) n++; return n; }

/* Castagnoli CRC, reflected form. The ext4 format uses the raw Linux
 * crc32c accumulator (no final complement). */
static uint32_t ext4_crc32c(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *p = (const uint8_t *)data;
    while (length--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0x82F63B78U & (0U - (crc & 1U)));
    }
    return crc;
}

static void ext4_store_le32(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static uint32_t ext4_inode_checksum_seed(const struct ext4_fs *fs,
                                         const struct ext4_inode *inode)
{
    uint8_t number[4], generation[4];
    ext4_store_le32(number, inode->number);
    ext4_store_le32(generation, inode->generation);
    uint32_t crc = ext4_crc32c(fs->checksum_seed, number, sizeof(number));
    return ext4_crc32c(crc, generation, sizeof(generation));
}

static int ext4_verify_inode_checksum(const struct ext4_fs *fs,
                                      uint32_t ino, const uint8_t *raw,
                                      uint32_t inode_size)
{
    uint8_t zero[2] = {0, 0};
    uint16_t extra_isize = inode_size > 128U ? le16(raw + 128) : 0;
    uint32_t crc;
    uint32_t expected;
    uint32_t offset;
    uint8_t ino_le[4], generation_le[4];

    if (!fs->metadata_csum || fs->creator_os != 0U)
        return 1;

    ext4_store_le32(ino_le, ino);
    ext4_store_le32(generation_le, le32(raw + 100));
    crc = ext4_crc32c(fs->checksum_seed, ino_le, sizeof(ino_le));
    crc = ext4_crc32c(crc, generation_le, sizeof(generation_le));
    crc = ext4_crc32c(crc, raw, 124U);
    crc = ext4_crc32c(crc, zero, sizeof(zero));
    crc = ext4_crc32c(crc, raw + 126U, 2U);

    if (inode_size > 128U) {
        /* i_extra_isize occupies 0x80..0x81; i_checksum_hi is at 0x82. */
        crc = ext4_crc32c(crc, raw + 128U, 2U);
        offset = 130U;
        if (extra_isize >= 4U && inode_size >= 132U) {
            crc = ext4_crc32c(crc, zero, sizeof(zero));
            offset = 132U;
        }
        if (inode_size > offset)
            crc = ext4_crc32c(crc, raw + offset, inode_size - offset);
    }

    expected = le16(raw + 124U);
    if (extra_isize >= 4U && inode_size >= 132U)
        expected |= (uint32_t)le16(raw + 130U) << 16;
    else
        crc &= 0xFFFFU;

    return crc == expected;
}

static int ext4_verify_group_desc(struct ext4_fs *fs, uint32_t group,
                                  uint8_t *desc)
{
    uint8_t group_le[4], zero[2] = {0, 0};
    uint32_t crc;
    uint16_t expected;

    if (!fs->metadata_csum)
        return 1;
    if (fs->descriptor_size < 32U)
        return 0;

    ext4_store_le32(group_le, group);
    crc = ext4_crc32c(fs->checksum_seed, group_le, sizeof(group_le));
    crc = ext4_crc32c(crc, desc, 0x1EU);
    crc = ext4_crc32c(crc, zero, sizeof(zero));
    if (fs->descriptor_size > 0x20U)
        crc = ext4_crc32c(crc, desc + 0x20U,
                          fs->descriptor_size - 0x20U);
    expected = le16(desc + 0x1EU);
    return (uint16_t)crc == expected;
}

static int ext4_verify_extent_block(struct ext4_fs *fs,
                                    const struct ext4_inode *inode,
                                    const uint8_t *block)
{
    uint32_t expected;
    uint32_t actual;
    uint32_t seed;

    if (!fs->metadata_csum)
        return 1;
    if (fs->block_size < 4U)
        return 0;
    expected = le32(block + fs->block_size - 4U);
    seed = ext4_inode_checksum_seed(fs, inode);
    actual = ext4_crc32c(seed, block, fs->block_size - 4U);
    return actual == expected;
}

static int ext4_verify_dir_leaf(struct ext4_fs *fs,
                                const struct ext4_inode *inode,
                                const uint8_t *block)
{
    const uint8_t *tail;
    uint32_t expected, actual;

    if (!fs->metadata_csum)
        return 1;
    if (fs->block_size < 12U)
        return 0;
    tail = block + fs->block_size - 12U;
    if (le32(tail) != 0U || le16(tail + 4U) != 12U ||
        tail[6] != 0U || tail[7] != 0xDEU)
        return 0;
    expected = le32(tail + 8U);
    actual = ext4_crc32c(ext4_inode_checksum_seed(fs, inode),
                         block, fs->block_size - 12U);
    return actual == expected;
}

/* Verify an HTree root (count_offset=32) or interior node (count_offset=8).
 * We scan leaf blocks linearly, but still validate the index metadata before
 * skipping it so a malformed index cannot be mistaken for directory data. */
static int ext4_verify_dx_block(struct ext4_fs *fs,
                                const struct ext4_inode *inode,
                                const uint8_t *block, int root)
{
    uint32_t count_offset;
    uint16_t limit, count;
    const uint8_t *tail;
    uint8_t zero[4] = {0, 0, 0, 0};
    uint32_t crc, expected;

    if (root) {
        if (le32(block) != inode->number || le16(block + 4U) != 12U ||
            le32(block + 12U) == 0U ||
            le16(block + 16U) != fs->block_size - 12U ||
            le32(block + 24U) != 0U || block[29] != 8U)
            return 0;
        count_offset = 32U;
    } else {
        if (le32(block) != 0U || le16(block + 4U) != fs->block_size)
            return 0;
        count_offset = 8U;
    }

    if (count_offset + 4U > fs->block_size)
        return 0;
    limit = le16(block + count_offset);
    count = le16(block + count_offset + 2U);
    if (!limit || !count || count > limit)
        return 0;
    if (fs->metadata_csum) {
        if (count_offset + (uint32_t)limit * 8U > fs->block_size - 8U)
            return 0;
    } else if (count_offset + (uint32_t)limit * 8U > fs->block_size) {
        return 0;
    }

    if (!fs->metadata_csum)
        return 1;
    tail = block + count_offset + (uint32_t)limit * 8U;
    if (le32(tail) != 0U)
        return 0;

    crc = ext4_crc32c(ext4_inode_checksum_seed(fs, inode), block,
                      count_offset + (uint32_t)count * 8U);
    crc = ext4_crc32c(crc, tail, 4U);
    crc = ext4_crc32c(crc, zero, sizeof(zero));
    expected = le32(tail + 4U);
    return crc == expected;
}

static int ext4_read_directory_block(struct ext4_fs *fs,
                                     const struct ext4_inode *inode,
                                     uint32_t logical, uint8_t *buffer,
                                     int *is_index)
{
    uint64_t physical;
    uint32_t available;

    *is_index = 0;
    if (!ext4_inode_block(fs, inode, logical, &physical, &available) ||
        !physical || !ext4_read_block(fs, physical, buffer))
        return 0;

    if ((inode->flags & EXT4_INDEX_FL) && logical == 0U) {
        if (!ext4_verify_dx_block(fs, inode, buffer, 1))
            return 0;
        *is_index = 1;
        return 1;
    }
    if (le32(buffer) == 0U && le16(buffer + 4U) == fs->block_size) {
        if (!ext4_verify_dx_block(fs, inode, buffer, 0))
            return 0;
        *is_index = 1;
        return 1;
    }
    return ext4_verify_dir_leaf(fs, inode, buffer);
}

static int ext4_read_bytes(struct ext4_fs *fs, uint64_t offset, void *out, size_t length)
{
    uint8_t sector[BLOCKDEV_SECTOR_SIZE];
    uint8_t *dst=(uint8_t *)out;
    uint64_t volume_bytes=fs->partition.sector_count * (uint64_t)BLOCKDEV_SECTOR_SIZE;
    if(offset > volume_bytes || (uint64_t)length > volume_bytes-offset) return 0;
    while(length) {
        uint64_t lba=fs->partition.first_lba + offset/BLOCKDEV_SECTOR_SIZE;
        size_t in=(size_t)(offset%BLOCKDEV_SECTOR_SIZE);
        size_t take=BLOCKDEV_SECTOR_SIZE-in;
        if(take>length) take=length;
        if(!blockdev_read(fs->device,lba,1,sector)) return 0;
        bytes_copy(dst,sector+in,take);
        dst+=take; offset+=take; length-=take;
    }
    return 1;
}

static int ext4_read_block(struct ext4_fs *fs, uint64_t block, void *buffer)
{
    if(block>=fs->block_count) return 0;
    uint64_t sector=block*(fs->block_size/BLOCKDEV_SECTOR_SIZE);
    uint64_t volume_sectors=fs->partition.sector_count;
    uint32_t count=fs->block_size/BLOCKDEV_SECTOR_SIZE;
    if(sector>volume_sectors || count>volume_sectors-sector) return 0;
    return blockdev_read(fs->device,fs->partition.first_lba+sector,count,buffer);
}

static int ext4_group_inode_table(struct ext4_fs *fs, uint32_t group, uint64_t *table)
{
    uint64_t desc_block = fs->first_data_block + 1ULL;
    uint64_t desc_size = fs->descriptor_size;
    uint64_t desc_off = desc_block * (uint64_t)fs->block_size +
                        (uint64_t)group * desc_size;
    uint8_t desc[64];

    if (group >= fs->groups_count || desc_size > sizeof(desc) ||
        !ext4_read_bytes(fs, desc_off, desc, (size_t)desc_size))
        return 0;
    if (!ext4_verify_group_desc(fs, group, desc))
        return 0;

    *table = (uint64_t)le32(desc + 8);
    if (desc_size >= 64U)
        *table |= (uint64_t)le32(desc + 40) << 32;
    return *table < fs->block_count;
}

static int ext4_read_inode(struct ext4_fs *fs, uint32_t ino, struct ext4_inode *out)
{
    if(!ino || ino>fs->groups_count*fs->inodes_per_group) return 0;
    uint32_t group=(ino-1U)/fs->inodes_per_group;
    uint32_t index=(ino-1U)%fs->inodes_per_group;
    uint64_t table;
    if(!ext4_group_inode_table(fs,group,&table)) return 0;
    uint64_t off=table*(uint64_t)fs->block_size+(uint64_t)index*fs->inode_size;
    uint8_t raw[256];
    if (fs->inode_size > sizeof(raw) ||
        !ext4_read_bytes(fs, off, raw, fs->inode_size))
        return 0;
    if (!ext4_verify_inode_checksum(fs, ino, raw, fs->inode_size))
        return 0;

    out->number = ino;
    out->generation = le32(raw + 100);
    out->mode = le16(raw);
    out->size = le32(raw + 4);
    if (fs->inode_size >= 112U)
        out->size |= (uint64_t)le32(raw + 108) << 32;
    out->flags = le32(raw + 32);
    bytes_copy(out->block, raw + 40, 60);
    return 1;
}

static int ext4_extent_lookup_node(struct ext4_fs *fs,
                                   const struct ext4_inode *inode,
                                   const uint8_t *node, uint32_t logical,
                                   uint64_t *physical, uint32_t *available,
                                   unsigned depth, int external)
{
    const struct ext4_extent_header *h=(const struct ext4_extent_header *)node;
    if (external && !ext4_verify_extent_block(fs, inode, node))
        return 0;
    if(depth>EXT4_MAX_EXTENT_DEPTH || le16((const uint8_t *)&h->magic)!=EXT4_EXTENT_MAGIC) return 0;
    uint16_t entries=le16((const uint8_t *)&h->entries);
    uint16_t tree_depth=le16((const uint8_t *)&h->depth);
    if(entries>le16((const uint8_t *)&h->max) || tree_depth>EXT4_MAX_EXTENT_DEPTH ||
       entries>(fs->block_size-sizeof(*h))/12U) return 0;
    if(tree_depth==0) {
        const struct ext4_extent *e=(const struct ext4_extent *)(node+sizeof(*h));
        for(uint16_t i=0;i<entries;i++) {
            uint32_t start=le32((const uint8_t *)&e[i].logical);
            uint16_t rawlen=le16((const uint8_t *)&e[i].length);
            uint32_t len=(rawlen>0x8000U) ? (uint32_t)(rawlen-0x8000U) : rawlen;
            uint64_t p=(uint64_t)le32((const uint8_t *)&e[i].start_lo) |
                       ((uint64_t)le16((const uint8_t *)&e[i].start_hi)<<32);
            if(!len) continue;
            if(logical>=start && logical-start<len) {
                if(rawlen>0x8000U) return 0; /* unwritten extents are not exposed as file data */
                *physical=p+(logical-start); *available=len-(logical-start); return 1;
            }
        }
        *physical=0; *available=1; return 1; /* sparse hole */
    }
    const struct ext4_extent_idx *ix=(const struct ext4_extent_idx *)(node+sizeof(*h));
    int selected=-1;
    for(uint16_t i=0;i<entries;i++) {
        uint32_t key=le32((const uint8_t *)&ix[i].logical);
        if(logical<key) break;
        selected=(int)i;
    }
    if(selected<0) { *physical=0; *available=1; return 1; }
    uint64_t child=(uint64_t)le32((const uint8_t *)&ix[selected].leaf_lo) |
                   ((uint64_t)le16((const uint8_t *)&ix[selected].leaf_hi)<<32);
    uint8_t *buf=(uint8_t *)kmalloc(fs->block_size);
    if(!buf) return 0;
    int ok = ext4_read_block(fs, child, buf) &&
             ext4_extent_lookup_node(fs, inode, buf, logical, physical,
                                     available, depth + 1U, 1);
    kfree(buf);
    return ok;
}

static int ext4_inode_block(struct ext4_fs *fs, const struct ext4_inode *inode,
                            uint32_t logical, uint64_t *physical, uint32_t *available)
{
    if(!(inode->flags&EXT4_EXTENTS_FL)) return 0;
    if (le16(inode->block + 4) > 4U || le16(inode->block + 2) > 4U) return 0;
    return ext4_extent_lookup_node(fs, inode, inode->block, logical,
                                   physical, available, 0, 0);
}

static int ext4_read_inode_data(struct ext4_fs *fs, const struct ext4_inode *inode,
                                uint64_t offset, void *buffer, size_t length)
{
    uint8_t *dst=(uint8_t *)buffer;
    uint8_t *blockbuf=(uint8_t *)kmalloc(fs->block_size);
    if(!blockbuf) return 0;
    if(offset>inode->size || (uint64_t)length>inode->size-offset) { kfree(blockbuf); return 0; }
    while(length) {
        uint64_t logical64=offset/fs->block_size;
        if(logical64>0xFFFFFFFFULL) { kfree(blockbuf); return 0; }
        uint32_t logical=(uint32_t)logical64;
        size_t in=(size_t)(offset%fs->block_size);
        size_t take=fs->block_size-in;
        uint64_t physical; uint32_t available;
        if(take>length) take=length;
        if(!ext4_inode_block(fs,inode,logical,&physical,&available)) { kfree(blockbuf); return 0; }
        if(physical==0) { for(size_t i=0;i<take;i++) dst[i]=0; }
        else if(!ext4_read_block(fs,physical,blockbuf)) { kfree(blockbuf); return 0; }
        else bytes_copy(dst,blockbuf+in,take);
        dst+=take; offset+=take; length-=take;
    }
    kfree(blockbuf);
    return 1;
}

static uint8_t mode_type(uint16_t mode)
{
    switch(mode&0xF000U) { case 0x8000: return VFS_TYPE_REGULAR; case 0x4000: return VFS_TYPE_DIRECTORY; case 0xA000: return VFS_TYPE_SYMLINK; default: return VFS_TYPE_UNKNOWN; }
}

static int ext4_lookup(struct ext4_fs *fs, uint32_t dir_ino, const char *name,
                       uint32_t *found)
{
    struct ext4_inode dir;
    size_t wanted = str_len(name);
    uint8_t *buf;
    uint64_t blocks;

    if (!ext4_read_inode(fs, dir_ino, &dir) ||
        mode_type(dir.mode) != VFS_TYPE_DIRECTORY ||
        !wanted || wanted > VFS_NAME_MAX)
        return 0;

    buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf)
        return 0;
    blocks = (dir.size + fs->block_size - 1U) / fs->block_size;

    for (uint64_t logical64 = 0; logical64 < blocks; logical64++) {
        int is_index = 0;
        if (logical64 > 0xFFFFFFFFULL ||
            !ext4_read_directory_block(fs, &dir, (uint32_t)logical64,
                                       buf, &is_index)) {
            kfree(buf);
            return 0;
        }
        if (is_index)
            continue;

        size_t data_end = fs->metadata_csum ? fs->block_size - 12U : fs->block_size;
        size_t pos = 0;
        while (pos + 8U <= data_end) {
            uint32_t ino = le32(buf + pos);
            uint16_t rec = le16(buf + pos + 4U);
            uint8_t nlen = buf[pos + 6U];
            if (rec < 8U || (rec & 3U) || pos + rec > data_end ||
                nlen > rec - 8U) {
                kfree(buf);
                return 0;
            }
            if (ino && nlen == wanted &&
                bytes_equal((const char *)(buf + pos + 8U), name, wanted)) {
                *found = ino;
                kfree(buf);
                return 1;
            }
            pos += rec;
        }
    }

    kfree(buf);
    return 0;
}

static int ext4_resolve(struct ext4_fs *fs, const char *path,
                        struct ext4_inode *inode, uint32_t *ino_out)
{
    if(!fs || !fs->mounted || !path || path[0]!='/') return 0;
    uint32_t current=EXT4_ROOT_INO;
    size_t i=1, len=str_len(path);
    while(i<len) {
        while(i<len && path[i]=='/') i++;
        if(i>=len) break;
        size_t start=i;
        while(i<len && path[i]!='/') i++;
        size_t n=i-start;
        if(!n || n>VFS_NAME_MAX) return 0;
        char component[VFS_NAME_MAX+1U];
        for(size_t j=0;j<n;j++) component[j]=path[start+j];
        component[n]='\0';
        if(!ext4_lookup(fs,current,component,&current)) return 0;
    }
    if(!ext4_read_inode(fs,current,inode)) return 0;
    if(ino_out) *ino_out=current;
    return 1;
}

int ext4_mount(struct ext4_fs *fs, struct block_device *device,
               const struct partition_entry *partition)
{
    uint8_t sb[1024];

    ext4_error_text = "invalid mount arguments or partition bounds";
    ext4_probe_magic = 0;
    ext4_probe_incompat = 0;
    ext4_probe_ro_compat = 0;
    if (!fs || !device || !partition ||
        device->sector_size != BLOCKDEV_SECTOR_SIZE ||
        partition->sector_count < 4 ||
        partition->first_lba >= device->sector_count ||
        partition->sector_count > device->sector_count - partition->first_lba)
        return 0;

    fs->mounted = 0;
    if (!ext4_read_bytes(
            (struct ext4_fs *)&(struct ext4_fs){
                .device = device,
                .partition = *partition
            },
            1024, sb, sizeof(sb))) {
        ext4_error_text = "could not read superblock at partition offset 1024";
        return 0;
    }

    ext4_probe_magic = le16(sb + 56);
    if (ext4_probe_magic != EXT4_SUPER_MAGIC) {
        ext4_error_text = "superblock magic mismatch (not ext4 at this partition offset)";
        return 0;
    }

    uint32_t log = le32(sb + 24);
    uint32_t block_size;
    uint32_t incompat = le32(sb + 96);
    uint32_t ro_compat = le32(sb + 100);
    int metadata_csum = (ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
    ext4_probe_incompat = incompat;
    ext4_probe_ro_compat = ro_compat;

    if (log > 2U) {
        ext4_error_text = "unsupported ext4 block-size exponent";
        return 0;
    }
    block_size = 1024U << log;

    if (ro_compat & 0x0200U) {
        ext4_error_text = "BIGALLOC feature is unsupported";
        return 0;
    }
    if (incompat & ~EXT4_FEATURE_INCOMPAT_SUPPORTED) {
        ext4_error_text = "unsupported EXT4 incompat feature flags";
        return 0;
    }
    if (!(incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) {
        ext4_error_text = "EXT4 extents feature is not enabled";
        return 0;
    }
    if ((incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED) && !metadata_csum) {
        ext4_error_text = "CSUM_SEED requires METADATA_CSUM";
        return 0;
    }
    if (metadata_csum) {
        uint32_t actual = ext4_crc32c(0xFFFFFFFFU, sb, 0x3FCU);
        uint32_t expected = le32(sb + 0x3FCU);
        if (sb[0x175U] != EXT4_CRC32C_CHKSUM) {
            ext4_error_text = "unsupported ext4 metadata checksum algorithm";
            return 0;
        }
        if (actual != expected) {
            ext4_error_text = "ext4 superblock metadata checksum mismatch";
            return 0;
        }
        if (le32(sb + 0x48U) != 0U) {
            ext4_error_text = "metadata checksum inode format from non-Linux creator is unsupported";
            return 0;
        }
    }

    uint32_t blocks_lo = le32(sb + 4);
    uint32_t blocks_hi = le32(sb + 0x150);
    uint64_t blocks = (uint64_t)blocks_lo | ((uint64_t)blocks_hi << 32);
    uint32_t bpg = le32(sb + 32);
    uint32_t ipg = le32(sb + 40);
    uint32_t inode_size = le16(sb + 88);
    uint16_t desc_size = le16(sb + 254);
    if (!desc_size)
        desc_size = 32;

    if (desc_size < 32U || desc_size > 64U || (desc_size & 7U) ||
        ((incompat & EXT4_FEATURE_INCOMPAT_64BIT) && desc_size < 64U) ||
        !bpg || !ipg || inode_size < 128U || inode_size > 256U ||
        (inode_size & 3U) || !blocks) {
        ext4_error_text = "invalid ext4 geometry or unsupported inode/descriptor size";
        return 0;
    }

    uint32_t first_data = le32(sb + 20);
    if (blocks <= first_data) {
        ext4_error_text = "invalid ext4 first-data-block or block count";
        return 0;
    }

    uint64_t groups_b = (blocks - first_data + bpg - 1U) / bpg;
    uint32_t inodes = le32(sb + 0);
    uint64_t groups = ((uint64_t)inodes + ipg - 1U) / ipg;
    if (groups_b > 0xFFFFFFFFULL || groups == 0 || groups > groups_b) {
        ext4_error_text = "invalid ext4 block-group geometry";
        return 0;
    }

    uint64_t volume_blocks =
        (partition->sector_count * (uint64_t)BLOCKDEV_SECTOR_SIZE) / block_size;
    if (blocks > volume_blocks) {
        ext4_error_text = "ext4 filesystem size exceeds partition bounds";
        return 0;
    }

    fs->device = device;
    fs->partition = *partition;
    fs->block_count = blocks;
    fs->block_size = block_size;
    fs->blocks_per_group = bpg;
    fs->inodes_per_group = ipg;
    fs->inode_size = inode_size;
    fs->first_data_block = first_data;
    fs->descriptor_size = desc_size;
    fs->feature_incompat = incompat;
    fs->feature_ro_compat = ro_compat;
    fs->metadata_csum = (uint8_t)metadata_csum;
    fs->creator_os = le32(sb + 0x48U);
    fs->checksum_seed = (incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
        ? le32(sb + 0x270U)
        : ext4_crc32c(0xFFFFFFFFU, sb + 0x68U, 16U);
    fs->groups_count = (uint32_t)groups;

    struct ext4_inode root;
    if (!ext4_read_inode(fs, EXT4_ROOT_INO, &root)) {
        ext4_error_text = metadata_csum
            ? "root inode, group descriptor, or inode checksum verification failed"
            : "could not read ext4 root inode/inode table";
        return 0;
    }
    if (mode_type(root.mode) != VFS_TYPE_DIRECTORY) {
        ext4_error_text = "ext4 root inode is not a directory";
        return 0;
    }

    fs->mounted = 1;
    ext4_error_text = "ok";
    return 1;
}

static int ext4_vfs_open(void *context, const char *path,
                         struct vfs_file *file)
{
    struct ext4_fs *fs = (struct ext4_fs *)context;
    struct ext4_inode inode;
    uint32_t ino;

    if (!file || !ext4_resolve(fs, path, &inode, &ino))
        return 0;

    file->inode = ino;
    file->size = inode.size;
    file->offset = 0;
    file->type = mode_type(inode.mode);
    return file->type == VFS_TYPE_REGULAR;
}

static int64_t ext4_vfs_read(void *context, struct vfs_file *file,
                             void *buffer, size_t count)
{
    struct ext4_fs *fs = (struct ext4_fs *)context;
    struct ext4_inode inode;

    if (!fs || !fs->mounted || !file || !buffer ||
        !ext4_read_inode(fs, file->inode, &inode))
        return -1;
    if (file->offset >= file->size || count == 0)
        return 0;

    uint64_t remaining = file->size - file->offset;
    if ((uint64_t)count > remaining)
        count = (size_t)remaining;
    if (!ext4_read_inode_data(fs, &inode, file->offset, buffer, count))
        return -1;

    file->offset += count;
    return (int64_t)count;
}

static void ext4_vfs_close(void *context, struct vfs_file *file)
{
    (void)context;
    if (file)
        file->offset = 0;
}

static int ext4_vfs_stat(void *context, const char *path,
                         uint64_t *size, uint8_t *type)
{
    struct ext4_fs *fs = (struct ext4_fs *)context;
    struct ext4_inode inode;

    if (!ext4_resolve(fs, path, &inode, 0))
        return 0;
    if (size)
        *size = inode.size;
    if (type)
        *type = mode_type(inode.mode);
    return 1;
}

static int ext4_vfs_readdir(void *context, const char *path,
                            uint32_t *cursor, struct vfs_dirent *entry)
{
    struct ext4_fs *fs = (struct ext4_fs *)context;
    struct ext4_inode dir;
    uint8_t *buf;
    uint64_t offset;

    if (!fs || !cursor || !entry ||
        !ext4_resolve(fs, path, &dir, 0) ||
        mode_type(dir.mode) != VFS_TYPE_DIRECTORY)
        return -1;

    buf = (uint8_t *)kmalloc(fs->block_size);
    if (!buf)
        return -1;

    offset = *cursor;
    while (offset < dir.size) {
        uint64_t logical64 = offset / fs->block_size;
        uint64_t block_start = logical64 * fs->block_size;
        size_t pos = (size_t)(offset % fs->block_size);
        size_t data_end;
        int is_index = 0;

        if (logical64 > 0xFFFFFFFFULL ||
            !ext4_read_directory_block(fs, &dir, (uint32_t)logical64,
                                       buf, &is_index)) {
            kfree(buf);
            return -1;
        }
        if (is_index) {
            offset = block_start + fs->block_size;
            continue;
        }

        data_end = fs->metadata_csum ? fs->block_size - 12U : fs->block_size;
        if (pos >= data_end) {
            offset = block_start + fs->block_size;
            continue;
        }

        while (pos + 8U <= data_end && offset < dir.size) {
            uint32_t ino = le32(buf + pos);
            uint16_t rec = le16(buf + pos + 4U);
            uint8_t nlen = buf[pos + 6U];
            uint8_t file_type = buf[pos + 7U];

            if (rec < 8U || (rec & 3U) || pos + rec > data_end ||
                nlen > rec - 8U) {
                kfree(buf);
                return -1;
            }

            offset = block_start + pos + rec;
            pos += rec;
            if (ino && nlen) {
                entry->inode = ino;
                entry->type = (file_type == 1U) ? VFS_TYPE_REGULAR :
                              (file_type == 2U) ? VFS_TYPE_DIRECTORY :
                              (file_type == 7U) ? VFS_TYPE_SYMLINK :
                              VFS_TYPE_UNKNOWN;
                for (uint8_t j = 0; j < nlen; j++)
                    entry->name[j] = (char)buf[pos - rec + 8U + j];
                entry->name[nlen] = '\0';
                *cursor = (uint32_t)offset;
                kfree(buf);
                return 1;
            }
        }
        offset = block_start + fs->block_size;
    }

    *cursor = (uint32_t)offset;
    kfree(buf);
    return 0;
}

static const struct vfs_operations ext4_vfs_operations = {
    .open = ext4_vfs_open,
    .read = ext4_vfs_read,
    .close = ext4_vfs_close,
    .stat = ext4_vfs_stat,
    .readdir = ext4_vfs_readdir
};

int vfs_mount_ext4(struct ext4_fs *fs, struct block_device *device,
                   const struct partition_entry *partition)
{
    if (!ext4_mount(fs, device, partition))
        return 0;
    return vfs_mount(&ext4_vfs_operations, fs);
}

int ext4_self_test(void)
{
    static const char vector[] = "123456789";
    /* Raw CRC32C accumulator used by Linux/ext4: no final complement. */
    return ext4_crc32c(0xFFFFFFFFU, vector, 9U) == 0x1CF96D7CU &&
           sizeof(struct ext4_extent_header) == 12U &&
           sizeof(struct ext4_extent) == 12U &&
           sizeof(struct ext4_extent_idx) == 12U;
}
