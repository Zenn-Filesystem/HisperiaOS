#ifndef HISPERIA_VFS_H
#define HISPERIA_VFS_H

#include <stdint.h>
#include <stddef.h>
#include "blockdev.h"
#include "partition.h"

#define VFS_NAME_MAX 255U
#define VFS_PATH_MAX 255U
#define VFS_TYPE_UNKNOWN 0U
#define VFS_TYPE_REGULAR 1U
#define VFS_TYPE_DIRECTORY 2U
#define VFS_TYPE_SYMLINK 3U

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

struct ext4_fs;
struct vfs_file;
struct vfs_dirent;

/* Filesystem-specific operations. VFS owns dispatch and file lifetime. */
struct vfs_operations {
    int (*open)(void *context, const char *path, struct vfs_file *file);
    int64_t (*read)(void *context, struct vfs_file *file,
                    void *buffer, size_t count);
    void (*close)(void *context, struct vfs_file *file);
    int (*stat)(void *context, const char *path,
                uint64_t *size, uint8_t *type);
    int (*readdir)(void *context, const char *path, uint32_t *cursor,
                   struct vfs_dirent *entry);
};

struct vfs_file {
    uint32_t inode;
    uint64_t size;
    uint64_t offset;
    uint8_t type;
    uint8_t opened;
    const struct vfs_operations *operations;
    void *filesystem;
};

struct vfs_dirent {
    uint32_t inode;
    uint8_t type;
    uint8_t reserved[3];
    char name[VFS_NAME_MAX + 1U];
};

int vfs_mount(const struct vfs_operations *operations, void *filesystem);
int vfs_mount_ext4(struct ext4_fs *fs, struct block_device *device,
                   const struct partition_entry *partition);
int vfs_is_mounted(void);
int vfs_open(const char *path, struct vfs_file *file);
int64_t vfs_read(struct vfs_file *file, void *buffer, size_t count);
int vfs_seek(struct vfs_file *file, int64_t offset, int whence,
             uint64_t *new_offset);
void vfs_close(struct vfs_file *file);
int vfs_readdir(const char *path, uint32_t *cursor, struct vfs_dirent *entry);
int vfs_stat(const char *path, uint64_t *size, uint8_t *type);

#endif
