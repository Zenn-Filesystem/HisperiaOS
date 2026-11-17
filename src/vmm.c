#include <stdint.h>
#include "vmm.h"
#include "pmm.h"

#define VMM_CR3_ADDRESS_MASK 0x000FFFFFFFFFF000ULL
#define VMM_CR4_LA57         (1ULL << 12)
#define VMM_PAGE_PRESENT (1ULL << 0)
#define VMM_TABLE_ENTRIES 512

static uint64_t active_cr3;
static uint64_t active_root_phys;
static uint8_t active_paging_levels;
static int vmm_ready;

static uint64_t read_cr3(void)
{
    uint64_t value;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(value));
    return value;
}

static uint64_t read_cr4(void)
{
    uint64_t value;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}

int vmm_inspect_root(uint64_t *present_count)
{
    if (!vmm_ready || !present_count)
        return 0;

    volatile uint64_t *root =
        (volatile uint64_t *)pmm_phys_to_virt(
            active_root_phys
        );

    if (!root)
        return 0;

    uint64_t count = 0;

    for (uint64_t i = 0; i < VMM_TABLE_ENTRIES; i++) {
        if (root[i] & VMM_PAGE_PRESENT)
            count++;
    }

    *present_count = count;

    return 1;
}

int vmm_bootstrap_init(void)
{
    active_cr3 = read_cr3();
    active_root_phys = active_cr3 & VMM_CR3_ADDRESS_MASK;

    /* CR4.LA57 selects five-level paging; otherwise x86-64 uses four. */
    active_paging_levels = (read_cr4() & VMM_CR4_LA57) ? 5 : 4;

    /* CR3's page-table base must be 4 KiB aligned by architectural design. */
    if (active_root_phys == 0 || (active_root_phys & 0xFFFULL) != 0)
        return 0;

    vmm_ready = 1;
    return 1;
}

uint64_t vmm_root_phys(void)
{
    return vmm_ready ? active_root_phys : 0;
}

uint8_t vmm_paging_levels(void)
{
    return vmm_ready ? active_paging_levels : 0;
}

uint64_t vmm_cr3_value(void)
{
    return vmm_ready ? active_cr3 : 0;
}
