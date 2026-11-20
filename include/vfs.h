#ifndef HISPERIA_VFS_H
#define HISPERIA_VFS_H

#include <stdint.h>
#include <stddef.h>
#include "blockdev.h"
#include "partition.h"

#define VFS_NAME_MAX 255U
#define VFS_TYPE_UNKNOWN 0U
#define VFS_TYPE_REGULAR 1U
#define VFS_TYPE_DIRECTORY 2U
#define VFS_TYPE_SYMLINK 3U

struct ext4_fs;
struct vfs_file {
    uint32_t inode;
    uint64_t size;
    uint64_t offset;
    uint8_t type;
    uint8_t opened;
};
struct vfs_dirent {
    uint32_t inode;
    uint8_t type;
    char name[VFS_NAME_MAX + 1U];
};

int vfs_mount_ext4(struct ext4_fs *fs, struct block_device *device,
                   const struct partition_entry *partition);
int vfs_is_mounted(void);
int vfs_open(const char *path, struct vfs_file *file);
int64_t vfs_read(struct vfs_file *file, void *buffer, size_t count);
void vfs_close(struct vfs_file *file);
int vfs_readdir(const char *path, uint32_t *cursor, struct vfs_dirent *entry);
int vfs_stat(const char *path, uint64_t *size, uint8_t *type);

#endif
