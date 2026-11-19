#ifndef HISPERIA_AHCI_H
#define HISPERIA_AHCI_H
#include "blockdev.h"
/* Experimental polling AHCI backend: SATA, READ DMA EXT, read-only. */
int ahci_init(struct block_device *device);
const char *ahci_last_error(void);
#endif
