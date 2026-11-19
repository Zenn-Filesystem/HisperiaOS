#include <stdio.h>
#include "blockdev.h"
#include "partition.h"
#include "fat32.h"

int main(void)
{
    int blockdev_ok = blockdev_ramdisk_self_test();
    int partition_ok = partition_self_test();
    int fat32_ok = fat32_bpb_self_test();

    printf("[HOST] Block device RAM-disk test: %s\n",
           blockdev_ok ? "PASS" : "FAIL");
    printf("[HOST] MBR/GPT partition parser: %s\n",
           partition_ok ? "PASS" : "FAIL");
    printf("[HOST] FAT32 BPB validation: %s\n",
           fat32_ok ? "PASS" : "FAIL");

    return (blockdev_ok && partition_ok && fat32_ok) ? 0 : 1;
}
