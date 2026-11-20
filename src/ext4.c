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
#define EXT4_FEATURE_INCOMPAT_SUPPORTED (EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG)
#define EXT4_EXTENT_MAGIC 0xF30AU
#define EXT4_ROOT_INO 2U
#define EXT4_MAX_BLOCK_SIZE 4096U
#define EXT4_MAX_EXTENT_DEPTH 5U

struct ext4_inode {
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

static struct ext4_fs *mounted_fs;

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void bytes_copy(void *dst, const void *src, size_t n) { uint8_t *d=dst; const uint8_t *s=src; for(size_t i=0;i<n;i++) d[i]=s[i]; }
static int bytes_equal(const char *a, const char *b, size_t n) { for(size_t i=0;i<n;i++) if(a[i]!=b[i]) return 0; return 1; }
static size_t str_len(const char *s) { size_t n=0; if(!s) return 0; while(s[n]) n++; return n; }

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
    uint64_t desc_block=fs->first_data_block+1ULL;
    uint64_t desc_size=fs->descriptor_size;
    uint64_t desc_off=desc_block*(uint64_t)fs->block_size+(uint64_t)group*desc_size;
    uint8_t desc[64];
    if(desc_size>sizeof(desc) || !ext4_read_bytes(fs,desc_off,desc,(size_t)desc_size)) return 0;
    *table=(uint64_t)le32(desc+8);
    if(desc_size>=64) *table|=(uint64_t)le32(desc+40)<<32;
    return *table<fs->block_count;
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
    if(fs->inode_size>sizeof(raw) || !ext4_read_bytes(fs,off,raw,fs->inode_size)) return 0;
    out->mode=le16(raw);
    out->size=le32(raw+4);
    if(fs->inode_size>=112) out->size|=(uint64_t)le32(raw+108)<<32;
    out->flags=le32(raw+32);
    bytes_copy(out->block,raw+40,60);
    return 1;
}

static int ext4_extent_lookup_node(struct ext4_fs *fs, const uint8_t *node,
                                   uint32_t logical, uint64_t *physical,
                                   uint32_t *available, unsigned depth)
{
    const struct ext4_extent_header *h=(const struct ext4_extent_header *)node;
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
    int ok=ext4_read_block(fs,child,buf) &&
           ext4_extent_lookup_node(fs,buf,logical,physical,available,depth+1U);
    kfree(buf);
    return ok;
}

static int ext4_inode_block(struct ext4_fs *fs, const struct ext4_inode *inode,
                            uint32_t logical, uint64_t *physical, uint32_t *available)
{
    if(!(inode->flags&EXT4_EXTENTS_FL)) return 0;
    if (le16(inode->block + 4) > 4U || le16(inode->block + 2) > 4U) return 0;
    return ext4_extent_lookup_node(fs,inode->block,logical,physical,available,0);
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
    if(!ext4_read_inode(fs,dir_ino,&dir) || mode_type(dir.mode)!=VFS_TYPE_DIRECTORY) return 0;
    size_t wanted=str_len(name);
    if(!wanted || wanted>VFS_NAME_MAX) return 0;
    uint8_t *buf=(uint8_t *)kmalloc(fs->block_size);
    if(!buf) return 0;
    uint64_t offset=0;
    while(offset<dir.size) {
        uint32_t logical=(uint32_t)(offset/fs->block_size);
        uint64_t physical; uint32_t available;
        if(!ext4_inode_block(fs,&dir,logical,&physical,&available)) { kfree(buf); return 0; }
        if(!physical) { offset+=fs->block_size; continue; }
        if(!ext4_read_block(fs,physical,buf)) { kfree(buf); return 0; }
        size_t pos=0;
        while(pos+8<=fs->block_size && offset+pos<dir.size) {
            uint32_t ino=le32(buf+pos); uint16_t rec=le16(buf+pos+4);
            uint8_t nlen=buf[pos+6];
            if(rec<8 || (rec&3U) || pos+rec>fs->block_size || nlen>rec-8U) { kfree(buf); return 0; }
            if(ino && nlen==wanted && bytes_equal((const char *)(buf+pos+8),name,wanted)) {
                *found=ino; kfree(buf); return 1;
            }
            pos+=rec;
        }
        offset+=fs->block_size;
    }
    kfree(buf); return 0;
}

static int ext4_resolve(const char *path, struct ext4_inode *inode, uint32_t *ino_out)
{
    if(!mounted_fs || !mounted_fs->mounted || !path || path[0]!='/') return 0;
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
        if(!ext4_lookup(mounted_fs,current,component,&current)) return 0;
    }
    if(!ext4_read_inode(mounted_fs,current,inode)) return 0;
    if(ino_out) *ino_out=current;
    return 1;
}

int ext4_mount(struct ext4_fs *fs, struct block_device *device,
               const struct partition_entry *partition)
{
    uint8_t sb[1024];
    if(!fs || !device || !partition || device->sector_size!=512 ||
       partition->sector_count<4 || partition->first_lba>=device->sector_count ||
       partition->sector_count>device->sector_count-partition->first_lba) return 0;
    fs->mounted=0;
    if(!ext4_read_bytes((struct ext4_fs *)&(struct ext4_fs){.device=device,.partition=*partition},1024,sb,sizeof(sb))) return 0;
    if(le16(sb+56)!=EXT4_SUPER_MAGIC) return 0;
    uint32_t log=le32(sb+24);
    if(log>2U) return 0;
    uint32_t block_size=1024U<<log;
    uint32_t incompat=le32(sb+96);
    uint32_t ro_compat=le32(sb+100);
    if(ro_compat & 0x0200U) return 0; /* BIGALLOC cluster allocation is unsupported. */
    if(incompat&~EXT4_FEATURE_INCOMPAT_SUPPORTED) return 0;
    if(!(incompat&EXT4_FEATURE_INCOMPAT_EXTENTS)) return 0;
    uint32_t blocks_lo=le32(sb+4), blocks_hi=le32(sb+0x150);
    uint64_t blocks=(uint64_t)blocks_lo | ((uint64_t)blocks_hi<<32);
    uint32_t bpg=le32(sb+32), ipg=le32(sb+40), inode_size=le16(sb+88);
    uint16_t desc_size=le16(sb+254);
    if(!desc_size) desc_size=32;
    if(desc_size<32 || desc_size>64 || (desc_size&7U) || !bpg || !ipg ||
       inode_size<128 || inode_size>256 || (inode_size&3U) || !blocks) return 0;
    uint32_t first_data=le32(sb+20);
    uint64_t groups_b=(blocks-first_data+bpg-1U)/bpg;
    uint32_t inodes=le32(sb+0);
    uint64_t groups=(inodes+ (uint64_t)ipg-1U)/ipg;
    if(groups_b>0xFFFFFFFFULL || groups==0 || groups>groups_b) return 0;
    uint64_t volume_blocks=(partition->sector_count*512ULL)/block_size;
    if(blocks>volume_blocks || first_data>=blocks) return 0;
    fs->device=device; fs->partition=*partition; fs->block_count=blocks;
    fs->block_size=block_size; fs->blocks_per_group=bpg; fs->inodes_per_group=ipg;
    fs->inode_size=inode_size; fs->first_data_block=first_data;
    fs->descriptor_size=desc_size; fs->feature_incompat=incompat;
    fs->feature_ro_compat=ro_compat; fs->groups_count=(uint32_t)groups;
    struct ext4_inode root;
    if(!ext4_read_inode(fs,EXT4_ROOT_INO,&root) || mode_type(root.mode)!=VFS_TYPE_DIRECTORY) return 0;
    fs->mounted=1;
    return 1;
}

int vfs_mount_ext4(struct ext4_fs *fs, struct block_device *device,
                   const struct partition_entry *partition)
{
    if(!ext4_mount(fs,device,partition)) return 0;
    mounted_fs=fs;
    return 1;
}

int vfs_is_mounted(void) { return mounted_fs && mounted_fs->mounted; }

int vfs_open(const char *path, struct vfs_file *file)
{
    struct ext4_inode inode; uint32_t ino;
    if(!file || !ext4_resolve(path,&inode,&ino)) return 0;
    file->inode=ino; file->size=inode.size; file->offset=0;
    file->type=mode_type(inode.mode); file->opened=(file->type==VFS_TYPE_REGULAR);
    return file->opened;
}

int64_t vfs_read(struct vfs_file *file, void *buffer, size_t count)
{
    struct ext4_inode inode;
    if(!file || !file->opened || !buffer || !mounted_fs ||
       !ext4_read_inode(mounted_fs,file->inode,&inode)) return -1;
    if(file->offset>=file->size || count==0) return 0;
    uint64_t remaining=file->size-file->offset;
    if((uint64_t)count>remaining) count=(size_t)remaining;
    if(!ext4_read_inode_data(mounted_fs,&inode,file->offset,buffer,count)) return -1;
    file->offset+=count;
    return (int64_t)count;
}

void vfs_close(struct vfs_file *file) { if(file) { file->opened=0; file->offset=0; } }

int vfs_stat(const char *path, uint64_t *size, uint8_t *type)
{
    struct ext4_inode inode;
    if(!ext4_resolve(path,&inode,0)) return 0;
    if(size) *size=inode.size;
    if(type) *type=mode_type(inode.mode);
    return 1;
}

int vfs_readdir(const char *path, uint32_t *cursor, struct vfs_dirent *entry)
{
    struct ext4_inode dir;
    if(!cursor || !entry || !ext4_resolve(path,&dir,0) || mode_type(dir.mode)!=VFS_TYPE_DIRECTORY) return -1;
    uint8_t *buf=(uint8_t *)kmalloc(mounted_fs->block_size);
    if(!buf) return -1;
    uint64_t offset=(uint64_t)*cursor;
    while(offset<dir.size) {
        uint32_t logical=(uint32_t)(offset/mounted_fs->block_size);
        uint64_t physical; uint32_t available;
        if(!ext4_inode_block(mounted_fs,&dir,logical,&physical,&available)) { kfree(buf); return -1; }
        if(!physical) { offset=(offset/mounted_fs->block_size+1U)*mounted_fs->block_size; continue; }
        if(!ext4_read_block(mounted_fs,physical,buf)) { kfree(buf); return -1; }
        size_t pos=(size_t)(offset%mounted_fs->block_size);
        while(pos+8<=mounted_fs->block_size && offset<dir.size) {
            uint32_t ino=le32(buf+pos); uint16_t rec=le16(buf+pos+4); uint8_t nlen=buf[pos+6];
            if(rec<8 || (rec&3U) || pos+rec>mounted_fs->block_size || nlen>rec-8U) { kfree(buf); return -1; }
            offset+=rec; pos+=rec;
            if(ino && nlen) {
                entry->inode=ino;
                { uint8_t ft=buf[pos-rec+7]; entry->type=(ft==1)?VFS_TYPE_REGULAR:(ft==2)?VFS_TYPE_DIRECTORY:(ft==7)?VFS_TYPE_SYMLINK:VFS_TYPE_UNKNOWN; }
                for(uint8_t j=0;j<nlen;j++) entry->name[j]=(char)buf[pos-rec+8+j];
                entry->name[nlen]='\0'; *cursor=(uint32_t)offset; kfree(buf); return 1;
            }
        }
    }
    *cursor=(uint32_t)offset; kfree(buf); return 0;
}

int ext4_self_test(void)
{
    /* On-disk parser is validated against a real ext4 image in tools/test-ext4.sh. */
    return sizeof(struct ext4_extent_header)==12 && sizeof(struct ext4_extent)==12 && sizeof(struct ext4_extent_idx)==12;
}
