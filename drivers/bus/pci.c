#include <stdint.h>
#include "pci.h"
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static uint32_t cfg_read(uint8_t b,uint8_t d,uint8_t f,uint8_t o) {
    uint32_t a=0x80000000U|((uint32_t)b<<16)|((uint32_t)d<<11)|((uint32_t)f<<8)|(o&0xFCU);
    outl(PCI_CONFIG_ADDRESS,a); return inl(PCI_CONFIG_DATA);
}
static void cfg_write(uint8_t b,uint8_t d,uint8_t f,uint8_t o,uint32_t v) {
    uint32_t a=0x80000000U|((uint32_t)b<<16)|((uint32_t)d<<11)|((uint32_t)f<<8)|(o&0xFCU);
    outl(PCI_CONFIG_ADDRESS,a); outl(PCI_CONFIG_DATA,v);
}
uint32_t pci_read32(const struct pci_device *d,uint8_t o) { if(!d)return 0xFFFFFFFFU; return cfg_read(d->bus,d->device,d->function,o); }
static int inspect(uint8_t b,uint8_t d,uint8_t f,struct pci_device *o) {
    uint32_t id=cfg_read(b,d,f,0); if((id&0xFFFFU)==0xFFFFU)return 0;
    uint32_t c=cfg_read(b,d,f,8), h=cfg_read(b,d,f,0x0C);
    o->bus=b;o->device=d;o->function=f;o->vendor_id=(uint16_t)id;o->device_id=(uint16_t)(id>>16);
    o->revision=(uint8_t)c;o->prog_if=(uint8_t)(c>>8);o->subclass=(uint8_t)(c>>16);o->class_code=(uint8_t)(c>>24);o->header_type=(uint8_t)(h>>16);
    return 1;
}
int pci_find_class(uint8_t cc,uint8_t sc,uint8_t pi,struct pci_device *out) {
    if(!out)return 0;
    for(uint16_t b=0;b<256;b++)for(uint8_t d=0;d<32;d++){
        struct pci_device x; if(!inspect((uint8_t)b,d,0,&x))continue;
        uint8_t funcs=(x.header_type&0x80U)?8:1;
        for(uint8_t f=0;f<funcs;f++){
            if(f && !inspect((uint8_t)b,d,f,&x))continue;
            if(x.class_code==cc&&x.subclass==sc&&x.prog_if==pi){*out=x;return 1;}
        }
    }
    return 0;
}
int pci_enable_memory_busmaster(const struct pci_device *d) {
    if(!d)return 0; uint32_t v=cfg_read(d->bus,d->device,d->function,4);
    v|=(1U<<1)|(1U<<2); cfg_write(d->bus,d->device,d->function,4,v);
    uint32_t r=cfg_read(d->bus,d->device,d->function,4); return (r&6U)==6U;
}
