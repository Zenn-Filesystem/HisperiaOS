#include <stdint.h>
#include <stddef.h>
#include "ahci.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"

#define AHCI_CLASS 0x01U
#define AHCI_SUBCLASS 0x06U
#define AHCI_PROGIF 0x01U
#define HBA_CAP 0x00U
#define HBA_GHC 0x04U
#define HBA_PI  0x0CU
#define HBA_PORT_BASE 0x100U
#define HBA_PORT_SIZE 0x80U
#define GHC_AE (1U<<31)
#define GHC_HR (1U<<0)
#define PX_CLB 0x00U
#define PX_CLBU 0x04U
#define PX_FB 0x08U
#define PX_FBU 0x0CU
#define PX_IS 0x10U
#define PX_IE 0x14U
#define PX_CMD 0x18U
#define PX_TFD 0x20U
#define PX_SIG 0x24U
#define PX_SSTS 0x28U
#define PX_SERR 0x30U
#define PX_CI 0x38U
#define CMD_ST (1U<<0)
#define CMD_FRE (1U<<4)
#define CMD_FR (1U<<14)
#define CMD_CR (1U<<15)
#define TFD_BSY 0x80U
#define TFD_DRQ 0x08U
#define TFD_ERR 0x01U
#define IS_TFES (1U<<30)
#define SATA_SIG_ATA 0x00000101U
#define POLL_LIMIT 5000000U
#define MAX_SECTORS_PER_COMMAND 8U
#define PAGE_SIZE 4096U

struct hba_port { volatile uint32_t r[32]; };
struct hba_mem { volatile uint32_t cap,ghc,is,pi,vs,ccc_ctl,ccc_ports,em_loc,em_ctl,cap2,bohc; uint8_t reserved[0x100-44]; struct hba_port ports[32]; };
struct fis_reg_h2d { uint8_t type,flags,command,featurel,lba0,lba1,lba2,device,lba3,lba4,lba5,featureh,countl,counth,icc,control; } __attribute__((packed));
struct prdt { uint32_t dba,dbau,reserved,dbc; } __attribute__((packed));
struct cmd_header { uint16_t flags,prdtl; volatile uint32_t prdbc; uint32_t ctba,ctbau; uint32_t reserved[4]; } __attribute__((packed));
struct cmd_table { uint8_t cfis[64]; uint8_t acmd[16]; uint8_t reserved[48]; struct prdt prdt[1]; } __attribute__((packed));
struct ahci_state { struct hba_mem *hba; struct hba_port *port; uint64_t sectors; uint64_t clb_phys,fb_phys,ct_phys,bounce_phys; struct cmd_header *headers; struct cmd_table *table; uint8_t *bounce; uint8_t port_no; };
static struct ahci_state state;
static const char *ahci_error = "not initialized";

const char *ahci_last_error(void)
{
    return ahci_error;
}
static uint64_t alloc_page(void **virt) { uint64_t p=pmm_alloc(); if(!p)return 0; *virt=pmm_phys_to_virt(p); if(!*virt){(void)pmm_free(p);return 0;} for(size_t i=0;i<PAGE_SIZE;i++)((uint8_t*)*virt)[i]=0; return p; }
static int wait_clear(volatile uint32_t *reg,uint32_t mask) { for(uint32_t i=0;i<POLL_LIMIT;i++){if(!(*reg&mask))return 1;__asm__ volatile("pause");}return 0; }
static int stop_port(struct hba_port *p) { p->r[PX_CMD/4]&=~CMD_ST; if(!wait_clear(&p->r[PX_CMD/4],CMD_CR))return 0; p->r[PX_CMD/4]&=~CMD_FRE; return wait_clear(&p->r[PX_CMD/4],CMD_FR); }
static int start_port(struct hba_port *p) { p->r[PX_CMD/4]|=CMD_FRE; for(uint32_t i=0;i<POLL_LIMIT;i++){if(p->r[PX_CMD/4]&CMD_FR)break;if(i+1U==POLL_LIMIT)return 0;__asm__ volatile("pause");} p->r[PX_CMD/4]|=CMD_ST; for(uint32_t i=0;i<POLL_LIMIT;i++){if(p->r[PX_CMD/4]&CMD_CR)return 1;if(i+1U==POLL_LIMIT)return 0;__asm__ volatile("pause");} return 0; }
static int port_read(struct block_device *dev,uint64_t lba,uint32_t count,void *buffer) {
    (void)dev; if(!buffer||!count||lba>=state.sectors||(uint64_t)count>state.sectors-lba)return 0;
    uint8_t *dst=buffer;
    while(count){uint32_t n=count>MAX_SECTORS_PER_COMMAND?MAX_SECTORS_PER_COMMAND:count;uint32_t bytes=n*512U;
        if(!wait_clear(&state.port->r[PX_TFD/4],TFD_BSY|TFD_DRQ))return 0;
        state.port->r[PX_IS/4]=0xFFFFFFFFU; state.port->r[PX_SERR/4]=0xFFFFFFFFU;
        for(size_t i=0;i<sizeof(*state.table);i++)((uint8_t*)state.table)[i]=0;
        struct fis_reg_h2d *fis=(struct fis_reg_h2d*)state.table->cfis;
        fis->type=0x27; fis->flags=0x80; fis->command=0x25; fis->device=1U<<6;
        fis->lba0=(uint8_t)lba;fis->lba1=(uint8_t)(lba>>8);fis->lba2=(uint8_t)(lba>>16);fis->lba3=(uint8_t)(lba>>24);fis->lba4=(uint8_t)(lba>>32);fis->lba5=(uint8_t)(lba>>40);
        fis->countl=(uint8_t)n;fis->counth=(uint8_t)(n>>8);
        state.table->prdt[0].dba=(uint32_t)state.bounce_phys;state.table->prdt[0].dbau=(uint32_t)(state.bounce_phys>>32);state.table->prdt[0].dbc=(bytes-1U)|(1U<<31);
        struct cmd_header *h=&state.headers[0];h->flags=5U;h->prdtl=1;h->prdbc=0;h->ctba=(uint32_t)state.ct_phys;h->ctbau=(uint32_t)(state.ct_phys>>32);
        __asm__ volatile("mfence" ::: "memory"); state.port->r[PX_CI/4]=1U;
        int completed=0; for(uint32_t i=0;i<POLL_LIMIT;i++){if(!(state.port->r[PX_CI/4]&1U)){completed=1;break;}__asm__ volatile("pause");}
        if(!completed||(state.port->r[PX_IS/4]&IS_TFES)||(state.port->r[PX_TFD/4]&TFD_ERR))return 0;
        for(uint32_t i=0;i<bytes;i++)dst[i]=state.bounce[i];dst+=bytes;lba+=n;count-=n;
    }
    return 1;
}
int ahci_init(struct block_device *device)
{
    ahci_error = "initialization did not complete";
    if (!device) {
        ahci_error = "invalid block-device output";
        return 0;
    }

    for (size_t i = 0; i < sizeof(state); i++)
        ((uint8_t *)&state)[i] = 0;

    struct pci_device pci;
    if (!pci_find_class(AHCI_CLASS, AHCI_SUBCLASS, AHCI_PROGIF, &pci)) {
        ahci_error = "no PCI AHCI controller found";
        return 0;
    }
    if (!pci_enable_memory_busmaster(&pci)) {
        ahci_error = "could not enable PCI memory space/bus mastering";
        return 0;
    }

    uint32_t bar5 = pci_read32(&pci, 0x24);
    if ((bar5 & 1U) || bar5 == 0 || bar5 == 0xFFFFFFFFU) {
        ahci_error = "BAR5 is not a valid memory BAR";
        return 0;
    }
    uint64_t abar = (uint64_t)(bar5 & ~0xFU);
    if (!abar) {
        ahci_error = "AHCI ABAR has a zero physical base";
        return 0;
    }

    /* ABAR is device MMIO, not RAM: never translate it through the HHDM. */
    state.hba = (struct hba_mem *)vmm_map_mmio(abar, sizeof(struct hba_mem));
    if (!state.hba) {
        ahci_error = "could not create uncached ABAR MMIO mapping";
        return 0;
    }

    state.hba->ghc |= GHC_AE;
    uint32_t pi = state.hba->pi;
    for (uint8_t i = 0; i < 32; i++) {
        if (!(pi & (1U << i)))
            continue;
        struct hba_port *port = (struct hba_port *)(
            (uintptr_t)state.hba + HBA_PORT_BASE +
            (uintptr_t)i * HBA_PORT_SIZE);
        uint32_t ssts = port->r[PX_SSTS / 4];
        uint32_t sig = port->r[PX_SIG / 4];
        if ((ssts & 0x0FU) != 3U ||
            ((ssts >> 8) & 0x0FU) != 1U || sig != SATA_SIG_ATA)
            continue;
        state.port = port;
        state.port_no = i;
        break;
    }
    if (!state.port) {
        ahci_error = "no active SATA ATA port detected";
        return 0;
    }

    if (!stop_port(state.port)) {
        ahci_error = "SATA port command engine did not stop";
        return 0;
    }

    void *v = 0;
    state.clb_phys = alloc_page(&v);
    state.headers = (struct cmd_header *)v;
    state.fb_phys = alloc_page(&v);
    state.ct_phys = alloc_page(&v);
    state.table = (struct cmd_table *)v;
    state.bounce_phys = alloc_page((void **)&state.bounce);
    if (!state.clb_phys || !state.fb_phys || !state.ct_phys ||
        !state.bounce_phys) {
        ahci_error = "could not allocate AHCI DMA structures";
        return 0;
    }

    if (!(state.hba->cap & (1U << 31)) &&
        (state.clb_phys > 0xFFFFFFFFULL ||
         state.fb_phys > 0xFFFFFFFFULL ||
         state.ct_phys > 0xFFFFFFFFULL ||
         state.bounce_phys > 0xFFFFFFFFULL)) {
        ahci_error = "controller lacks 64-bit DMA and buffers are above 4 GiB";
        return 0;
    }

    state.port->r[PX_CLB / 4] = (uint32_t)state.clb_phys;
    state.port->r[PX_CLBU / 4] = (uint32_t)(state.clb_phys >> 32);
    state.port->r[PX_FB / 4] = (uint32_t)state.fb_phys;
    state.port->r[PX_FBU / 4] = (uint32_t)(state.fb_phys >> 32);
    state.port->r[PX_IE / 4] = 0;
    state.port->r[PX_IS / 4] = 0xFFFFFFFFU;
    state.port->r[PX_SERR / 4] = 0xFFFFFFFFU;
    if (!start_port(state.port)) {
        ahci_error = "SATA port command engine did not start";
        return 0;
    }

    /* IDENTIFY DEVICE uses command slot 0 and a 512-byte DMA transfer. */
    if (!wait_clear(&state.port->r[PX_TFD / 4], TFD_BSY | TFD_DRQ)) {
        ahci_error = "device remained busy before IDENTIFY";
        return 0;
    }
    for (size_t i = 0; i < PAGE_SIZE; i++)
        state.bounce[i] = 0;
    for (size_t i = 0; i < sizeof(*state.table); i++)
        ((uint8_t *)state.table)[i] = 0;

    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)state.table->cfis;
    fis->type = 0x27;
    fis->flags = 0x80;
    fis->command = 0xEC;
    state.table->prdt[0].dba = (uint32_t)state.bounce_phys;
    state.table->prdt[0].dbau = (uint32_t)(state.bounce_phys >> 32);
    state.table->prdt[0].dbc = 511U | (1U << 31);
    state.headers[0].flags = 5;
    state.headers[0].prdtl = 1;
    state.headers[0].ctba = (uint32_t)state.ct_phys;
    state.headers[0].ctbau = (uint32_t)(state.ct_phys >> 32);

    __asm__ volatile ("mfence" ::: "memory");
    state.port->r[PX_CI / 4] = 1U;
    int identify_done = 0;
    for (uint32_t i = 0; i < POLL_LIMIT; i++) {
        if (!(state.port->r[PX_CI / 4] & 1U)) {
            identify_done = 1;
            break;
        }
        __asm__ volatile ("pause");
    }
    if (!identify_done || (state.port->r[PX_IS / 4] & IS_TFES) ||
        (state.port->r[PX_TFD / 4] & TFD_ERR)) {
        ahci_error = "IDENTIFY DMA command timed out or reported an error";
        return 0;
    }

    uint16_t *id = (uint16_t *)state.bounce;
    if (!(id[49] & (1U << 9)) || !(id[83] & (1U << 10))) {
        ahci_error = "SATA device lacks required LBA/LBA48 support";
        return 0;
    }
    state.sectors = (uint64_t)id[100] |
                    ((uint64_t)id[101] << 16) |
                    ((uint64_t)id[102] << 32) |
                    ((uint64_t)id[103] << 48);
    if (!state.sectors) {
        ahci_error = "IDENTIFY returned zero sectors";
        return 0;
    }

    device->name = "ahci-sata0";
    device->sector_count = state.sectors;
    device->sector_size = 512;
    device->read = port_read;
    device->write = 0;
    device->context = &state;
    ahci_error = "ok";
    return 1;
}
