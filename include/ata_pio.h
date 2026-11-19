#ifndef HISPERIA_ATA_PIO_H
#define HISPERIA_ATA_PIO_H

#include "blockdev.h"

/* QEMU legacy primary-master IDE, polling PIO, LBA28, read-only. */
int ata_pio_init_primary_master(struct block_device *device);

#endif
