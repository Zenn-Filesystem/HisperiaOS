#include "vfs.h"

static const struct vfs_operations *root_operations;
static void *root_filesystem;

int vfs_mount(const struct vfs_operations *operations, void *filesystem)
{
    if (vfs_is_mounted() || !operations || !filesystem ||
        !operations->open || !operations->read || !operations->close ||
        !operations->stat || !operations->readdir)
        return 0;

    root_operations = operations;
    root_filesystem = filesystem;
    return 1;
}

int vfs_is_mounted(void)
{
    return root_operations != 0 && root_filesystem != 0;
}

int vfs_open(const char *path, struct vfs_file *file)
{
    if (!vfs_is_mounted() || !path || !file || path[0] != '/')
        return 0;

    *file = (struct vfs_file){0};
    if (!root_operations->open(root_filesystem, path, file)) {
        *file = (struct vfs_file){0};
        return 0;
    }

    file->operations = root_operations;
    file->filesystem = root_filesystem;
    file->opened = 1;
    return 1;
}

int64_t vfs_read(struct vfs_file *file, void *buffer, size_t count)
{
    if (!file || !file->opened || !file->operations ||
        !file->operations->read || (count != 0 && !buffer))
        return -1;
    if (count == 0)
        return 0;
    return file->operations->read(file->filesystem, file, buffer, count);
}

int vfs_seek(struct vfs_file *file, int64_t offset, int whence,
             uint64_t *new_offset)
{
    uint64_t base;
    uint64_t target;

    if (!file || !file->opened)
        return 0;

    switch (whence) {
    case VFS_SEEK_SET:
        base = 0;
        break;
    case VFS_SEEK_CUR:
        base = file->offset;
        break;
    case VFS_SEEK_END:
        base = file->size;
        break;
    default:
        return 0;
    }

    if (offset < 0) {
        uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1ULL;
        if (magnitude > base)
            return 0;
        target = base - magnitude;
    } else {
        uint64_t magnitude = (uint64_t)offset;
        if (magnitude > file->size || base > file->size - magnitude)
            return 0;
        target = base + magnitude;
    }

    if (target > file->size)
        return 0;
    file->offset = target;
    if (new_offset)
        *new_offset = target;
    return 1;
}

void vfs_close(struct vfs_file *file)
{
    if (!file || !file->opened)
        return;

    if (file->operations && file->operations->close)
        file->operations->close(file->filesystem, file);

    file->opened = 0;
    file->offset = 0;
    file->operations = 0;
    file->filesystem = 0;
}

int vfs_readdir(const char *path, uint32_t *cursor,
                struct vfs_dirent *entry)
{
    if (!vfs_is_mounted() || !path || !cursor || !entry ||
        !root_operations->readdir)
        return -1;
    volatile uint8_t *entry_bytes = (volatile uint8_t *)entry;
    for (size_t i = 0; i < sizeof(*entry); i++)
        entry_bytes[i] = 0;
    return root_operations->readdir(root_filesystem, path, cursor, entry);
}

int vfs_stat(const char *path, uint64_t *size, uint8_t *type)
{
    if (!vfs_is_mounted() || !path || !root_operations->stat)
        return 0;
    return root_operations->stat(root_filesystem, path, size, type);
}
