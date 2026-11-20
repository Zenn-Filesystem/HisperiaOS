#include <stdint.h>
#include <stdio.h>
#include "vfs.h"

static const char test_data[] = "hello";
static int close_called;

static int mock_open(void *context, const char *path, struct vfs_file *file)
{
    (void)context;
    if (!path || !file || path[0] != '/' || path[1] != 'h')
        return 0;
    file->inode = 42;
    file->size = sizeof(test_data) - 1U;
    file->offset = 0;
    file->type = VFS_TYPE_REGULAR;
    return 1;
}

static int64_t mock_read(void *context, struct vfs_file *file,
                         void *buffer, size_t count)
{
    (void)context;
    if (!file || !buffer || file->offset > sizeof(test_data) - 1U)
        return -1;
    size_t remaining = (sizeof(test_data) - 1U) - (size_t)file->offset;
    if (count > remaining)
        count = remaining;
    for (size_t i = 0; i < count; i++)
        ((char *)buffer)[i] = test_data[file->offset + i];
    file->offset += count;
    return (int64_t)count;
}

static void mock_close(void *context, struct vfs_file *file)
{
    (void)context;
    (void)file;
    close_called++;
}

static int mock_stat(void *context, const char *path,
                     uint64_t *size, uint8_t *type)
{
    (void)context;
    if (!path || path[0] != '/' || path[1] != 'h')
        return 0;
    if (size)
        *size = sizeof(test_data) - 1U;
    if (type)
        *type = VFS_TYPE_REGULAR;
    return 1;
}

static int mock_readdir(void *context, const char *path, uint32_t *cursor,
                        struct vfs_dirent *entry)
{
    (void)context;
    if (!path || path[0] != '/' || !cursor || !entry)
        return -1;
    if (*cursor != 0)
        return 0;
    entry->inode = 42;
    entry->type = VFS_TYPE_REGULAR;
    entry->name[0] = 'h';
    entry->name[1] = 'e';
    entry->name[2] = 'l';
    entry->name[3] = 'l';
    entry->name[4] = 'o';
    entry->name[5] = '\0';
    *cursor = 1;
    return 1;
}

static const struct vfs_operations mock_operations = {
    .open = mock_open,
    .read = mock_read,
    .close = mock_close,
    .stat = mock_stat,
    .readdir = mock_readdir
};

int main(void)
{
    int filesystem_cookie = 1;
    struct vfs_file file = {0};
    struct vfs_dirent entry = {0};
    uint32_t cursor = 0;
    uint64_t size = 0;
    uint8_t type = VFS_TYPE_UNKNOWN;
    char output[6] = {0};

    if (!vfs_mount(&mock_operations, &filesystem_cookie) || !vfs_is_mounted())
        return puts("VFS mount FAIL"), 1;
    if (!vfs_stat("/hello", &size, &type) || size != 5 ||
        type != VFS_TYPE_REGULAR)
        return puts("VFS stat FAIL"), 1;
    if (vfs_readdir("/", &cursor, &entry) != 1 || entry.inode != 42 ||
        entry.name[0] != 'h' || cursor != 1)
        return puts("VFS readdir FAIL"), 1;
    if (!vfs_open("/hello", &file) || !file.opened ||
        vfs_read(&file, output, 2) != 2 || output[0] != 'h' || output[1] != 'e')
        return puts("VFS open/read FAIL"), 1;
    if (!vfs_seek(&file, 0, VFS_SEEK_END, &size) || size != 5 ||
        vfs_read(&file, output, 1) != 0)
        return puts("VFS seek/end FAIL"), 1;
    if (!vfs_seek(&file, -2, VFS_SEEK_CUR, &size) || size != 3 ||
        vfs_read(&file, output, 2) != 2 || output[0] != 'l' || output[1] != 'o')
        return puts("VFS relative seek FAIL"), 1;
    if (vfs_seek(&file, -9, VFS_SEEK_CUR, 0))
        return puts("VFS invalid seek FAIL"), 1;

    vfs_close(&file);
    if (file.opened || close_called != 1)
        return puts("VFS close FAIL"), 1;

    puts("generic VFS mount/stat/readdir/open/read/seek/close PASS");
    return 0;
}
