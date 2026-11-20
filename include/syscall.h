#ifndef HISPERIA_SYSCALL_H
#define HISPERIA_SYSCALL_H

#include <stdint.h>

/* Ring 3 int 0x80 syscall ABI. */
#define HISPERIA_SYS_WRITE   1ULL
#define HISPERIA_SYS_EXIT    2ULL
#define HISPERIA_SYS_OPEN    3ULL
#define HISPERIA_SYS_READ    4ULL
#define HISPERIA_SYS_CLOSE   5ULL
#define HISPERIA_SYS_SEEK    6ULL
#define HISPERIA_SYS_STAT    7ULL
#define HISPERIA_SYS_READDIR 8ULL

#define HISPERIA_NAME_MAX 255U

struct hisperia_stat {
    uint64_t size;
    uint8_t type;
    uint8_t reserved[7];
};

struct hisperia_dirent {
    uint32_t inode;
    uint8_t type;
    uint8_t reserved[3];
    char name[HISPERIA_NAME_MAX + 1U];
};

#endif
