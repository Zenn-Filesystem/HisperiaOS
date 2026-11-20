#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "ext4.h"
#include "vfs.h"
#include "blockdev.h"

static uint8_t disk[64U * 1024U];
void *kmalloc(size_t n) { return __builtin_malloc(n); }
void kfree(void *p) { __builtin_free(p); }
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static void put_extent(uint8_t *p,uint32_t logical,uint16_t len,uint32_t physical){
    put16(p,0xF30A);put16(p+2,1);put16(p+4,4);put16(p+6,0);put32(p+8,0);
    put32(p+12,logical);put16(p+16,len);put16(p+18,0);put32(p+20,physical);
}
static void make_inode(uint8_t *p,uint16_t mode,uint32_t size,uint32_t block){
    put16(p,mode);put32(p+4,size);put32(p+32,0x00080000U);put_extent(p+40,0,1,block);
}
int main(void){
    struct block_device dev={0}; struct partition_entry part={0}; struct ext4_fs fs={0};
    struct vfs_file file={0}; struct vfs_dirent ent={0}; uint32_t cursor=0;
    uint8_t *sb=disk+1024; put32(sb,128); put32(sb+4,64); put32(sb+20,1);
    put32(sb+24,0); put32(sb+32,63); put32(sb+40,128); put16(sb+56,0xEF53);
    put16(sb+88,128); put16(sb+254,32); put32(sb+96,0x42); /* FILETYPE + EXTENTS */
    put32(disk+2U*1024U+8,3); /* group 0 inode table block */
    make_inode(disk+3U*1024U+128,0x41ED,1024,4); /* inode 2: root */
    make_inode(disk+3U*1024U+256,0x81A4,5,5);    /* inode 3: hello.txt */
    uint8_t *d=disk+4U*1024U; put32(d,2);put16(d+4,12);d[6]=1;d[7]=2;d[8]='.';
    put32(d+12,2);put16(d+16,12);d[18]=2;d[19]=2;d[20]='.';d[21]='.';
    put32(d+24,3);put16(d+28,1000);d[30]=9;d[31]=1;for(unsigned i=0;i<9;i++)d[32+i]=(uint8_t)"hello.txt"[i];
    for(unsigned i=0;i<5;i++)disk[5U*1024U+i]=(uint8_t)"hello"[i];
    if(!blockdev_ramdisk_init(&dev,"ext4-test",disk,128)) return 1;
    part.first_lba=0;part.sector_count=128;part.scheme=PARTITION_SCHEME_MBR;
    if(!vfs_mount_ext4(&fs,&dev,&part)){puts("ext4 mount FAIL");return 1;}
    int rr=vfs_readdir("/",&cursor,&ent); if(rr!=1 || ent.inode!=2 || ent.name[0]!='.') { printf("readdir FAIL rr=%d ino=%u name=%s cursor=%u\n",rr,ent.inode,ent.name,cursor);return 1; }
    if(!vfs_open("/hello.txt",&file)){puts("open FAIL");return 1;}
    char out[6]={0}; if(vfs_read(&file,out,5)!=5 || out[0]!='h'||out[1]!='e'||out[2]!='l'||out[3]!='l'||out[4]!='o'){puts("read FAIL");return 1;}
    vfs_close(&file); puts("ext4 mount + VFS readdir/open/read PASS"); return 0;
}
