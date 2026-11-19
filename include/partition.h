#ifndef HISPERIA_PARTITION_H
#define HISPERIA_PARTITION_H

#include <stdint.h>
#include <stddef.h>
#include "blockdev.h"

enum partition_scheme {
    PARTITION_SCHEME_MBR = 1,
    PARTITION_SCHEME_GPT = 2
};

struct partition_entry {
    uint64_t first_lba;
    uint64_t sector_count;
    uint8_t scheme;
    uint8_t mbr_type;
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
};

int partition_scan(struct block_device *device,
                   struct partition_entry *entries,
                   size_t entry_capacity,
                   size_t *entry_count);
int partition_self_test(void);

#endif
