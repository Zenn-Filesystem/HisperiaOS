#include <stdint.h>
#include <stddef.h>

#include "vmm.h"
#include "pmm.h"

#define VMM_CR4_LA57             (1ULL << 12)
#define VMM_CR0_WP               (1ULL << 16)
#define VMM_EFER_MSR             0xC0000080U
#define VMM_EFER_NXE             (1ULL << 11)
#define VMM_PHYS_MASK_LIMIT      0x000FFFFFFFFFF000ULL

#define PTE_PRESENT              (1ULL << 0)
#define PTE_WRITABLE             (1ULL << 1)
#define PTE_USER                 (1ULL << 2)
#define PTE_PWT                  (1ULL << 3)
#define PTE_PCD                  (1ULL << 4)
#define PTE_ACCESSED             (1ULL << 5)
#define PTE_DIRTY                (1ULL << 6)
#define PTE_PS                   (1ULL << 7)
#define PTE_GLOBAL               (1ULL << 8)
#define PTE_AVL                  (7ULL << 9)
#define PTE_PAT_LARGE            (1ULL << 12)
#define PTE_NX                   (1ULL << 63)
#define PTE_PAT_4K               (1ULL << 7)

#define PAGE_2M                  (1ULL << 21)
#define PAGE_1G                  (1ULL << 30)
#define TABLE_ENTRIES            512U

static uint64_t active_cr3;
static uint64_t active_root_phys;
static uint64_t kernel_root_phys;
static uint64_t physical_address_mask;
static uint8_t active_paging_levels;
static uint8_t physical_address_bits;
static int nx_supported;
static int nx_is_enabled;
static int vmm_ready;
static int protection_ready;

/* Dedicated supervisor-only virtual window for device MMIO mappings. */
#define VMM_MMIO_BASE  0xFFFFC10000000000ULL
#define VMM_MMIO_LIMIT 0xFFFFC11000000000ULL
static uint64_t next_mmio_virtual = VMM_MMIO_BASE;

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

static uint64_t read_cr0(void)
{
    uint64_t value;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(value));
    return value;
}

static void write_cr0(uint64_t value)
{
    __asm__ volatile ("mov %0, %%cr0" :: "r"(value) : "memory");
}

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b,
                  uint32_t *c, uint32_t *d)
{
    uint32_t ra, rb, rc, rd;
    __asm__ volatile (
        "cpuid"
        : "=a"(ra), "=b"(rb), "=c"(rc), "=d"(rd)
        : "a"(leaf), "c"(subleaf)
    );
    if (a) *a = ra;
    if (b) *b = rb;
    if (c) *c = rc;
    if (d) *d = rd;
}

static uint64_t read_msr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile (
        "rdmsr"
        : "=a"(lo), "=d"(hi)
        : "c"(msr)
    );
    return ((uint64_t)hi << 32) | lo;
}

static void write_msr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile (
        "wrmsr"
        :: "c"(msr), "a"(lo), "d"(hi)
        : "memory"
    );
}

static void invalidate_page(uint64_t address)
{
    __asm__ volatile ("invlpg (%0)" :: "r"((uintptr_t)address) : "memory");
}

static void detect_cpu_features(void)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    uint32_t max_extended = 0;

    cpuid(0x80000000U, 0, &max_extended, 0, 0, 0);

    physical_address_bits = 36;
    nx_supported = 0;

    if (max_extended >= 0x80000001U) {
        cpuid(0x80000001U, 0, &a, &b, &c, &d);
        nx_supported = (d & (1U << 20)) != 0;
    }

    if (max_extended >= 0x80000008U) {
        cpuid(0x80000008U, 0, &a, &b, &c, &d);
        uint8_t bits = (uint8_t)(a & 0xFFU);
        if (bits >= 32 && bits <= 52)
            physical_address_bits = bits;
    }

    physical_address_mask =
        (((1ULL << physical_address_bits) - 1ULL) & VMM_PHYS_MASK_LIMIT);
}

static int is_canonical_4level(uint64_t address)
{
    uint64_t upper = address >> 48;
    uint64_t sign = (address >> 47) & 1ULL;
    return sign ? upper == 0xFFFFULL : upper == 0;
}

static volatile uint64_t *table_from_phys(uint64_t phys)
{
    if (phys == 0 || (phys & (VMM_PAGE_SIZE - 1ULL)) != 0)
        return 0;

    if (phys >= PMM_MAX_PHYS ||
        PMM_MAX_PHYS - phys < VMM_PAGE_SIZE)
        return 0;

    return (volatile uint64_t *)pmm_phys_to_virt(phys);
}

static uint64_t entry_table_phys(uint64_t entry)
{
    return entry & physical_address_mask;
}

int vmm_bootstrap_init(void)
{
    vmm_ready = 0;
    protection_ready = 0;
    nx_is_enabled = 0;

    detect_cpu_features();

    active_cr3 = read_cr3();
    active_root_phys = active_cr3 & physical_address_mask;
    kernel_root_phys = active_root_phys;
    active_paging_levels = (read_cr4() & VMM_CR4_LA57) ? 5 : 4;

    if (active_root_phys == 0 ||
        (active_root_phys & (VMM_PAGE_SIZE - 1ULL)) != 0)
        return 0;

    if (!table_from_phys(active_root_phys))
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

int vmm_read_table_entry(uint64_t table_phys,
                         uint16_t index,
                         uint64_t *entry_out)
{
    if (!vmm_ready || !entry_out || index >= TABLE_ENTRIES)
        return 0;

    volatile uint64_t *table = table_from_phys(table_phys);
    if (!table)
        return 0;

    *entry_out = table[index];
    return 1;
}

int vmm_read_root_entry(uint16_t index, uint64_t *entry_out)
{
    if (!vmm_ready)
        return 0;
    return vmm_read_table_entry(active_root_phys, index, entry_out);
}

int vmm_inspect_root(uint64_t *present_count)
{
    if (!vmm_ready || !present_count)
        return 0;

    volatile uint64_t *root = table_from_phys(active_root_phys);
    if (!root)
        return 0;

    uint64_t count = 0;
    for (uint16_t i = 0; i < TABLE_ENTRIES; i++) {
        if (root[i] & PTE_PRESENT)
            count++;
    }

    *present_count = count;
    return 1;
}

struct walk_result {
    volatile uint64_t *leaf_entry;
    uint64_t raw_entry;
    uint64_t physical_address;
    uint64_t page_size;
    int effective_writable;
    int effective_user;
    int effective_nx;
    int ancestors_writable;
    int ancestors_nx;
};

static uint64_t pointer_flags_from_huge(uint64_t huge_entry)
{
    /* Flags meaningful on a non-leaf table pointer. */
    const uint64_t pointer_mask = PTE_PRESENT | PTE_WRITABLE |
        PTE_USER | PTE_ACCESSED | PTE_AVL | PTE_NX;
    return huge_entry & pointer_mask;
}

static int split_1g_page(volatile uint64_t *pdpt,
                         uint16_t index,
                         uint64_t old_entry,
                         uint64_t faulting_va)
{
    uint64_t new_pd_phys = pmm_alloc();
    if (!new_pd_phys)
        return 0;

    volatile uint64_t *new_pd = table_from_phys(new_pd_phys);
    if (!new_pd) {
        (void)pmm_free(new_pd_phys);
        return 0;
    }

    for (uint16_t i = 0; i < TABLE_ENTRIES; i++)
        new_pd[i] = 0;

    uint64_t base = old_entry &
        (physical_address_mask & ~(PAGE_1G - 1ULL));

    const uint64_t leaf_flags = old_entry &
        (PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PWT |
         PTE_PCD | PTE_ACCESSED | PTE_DIRTY | PTE_GLOBAL |
         PTE_AVL | PTE_NX | PTE_PAT_LARGE);

    for (uint16_t i = 0; i < TABLE_ENTRIES; i++) {
        uint64_t child_phys = base + ((uint64_t)i * PAGE_2M);
        uint64_t child_base = child_phys &
            (physical_address_mask & ~(PAGE_2M - 1ULL));
        new_pd[i] = child_base | leaf_flags | PTE_PS;
    }

    pdpt[index] = new_pd_phys | pointer_flags_from_huge(old_entry);
    invalidate_page(faulting_va);
    return 1;
}

static int split_2m_page(volatile uint64_t *pd,
                         uint16_t index,
                         uint64_t old_entry,
                         uint64_t faulting_va)
{
    uint64_t new_pt_phys = pmm_alloc();
    if (!new_pt_phys)
        return 0;

    volatile uint64_t *new_pt = table_from_phys(new_pt_phys);
    if (!new_pt) {
        (void)pmm_free(new_pt_phys);
        return 0;
    }

    for (uint16_t i = 0; i < TABLE_ENTRIES; i++)
        new_pt[i] = 0;

    uint64_t base = old_entry &
        (physical_address_mask & ~(PAGE_2M - 1ULL));

    const uint64_t child_flags = old_entry &
        (PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PWT |
         PTE_PCD | PTE_ACCESSED | PTE_DIRTY | PTE_GLOBAL |
         PTE_AVL | PTE_NX);
    const uint64_t pat_flag = (old_entry & PTE_PAT_LARGE) ? PTE_PAT_4K : 0;

    for (uint16_t i = 0; i < TABLE_ENTRIES; i++) {
        uint64_t child_phys = base + ((uint64_t)i * VMM_PAGE_SIZE);
        new_pt[i] = (child_phys & physical_address_mask) |
                    child_flags | pat_flag;
    }

    pd[index] = new_pt_phys | pointer_flags_from_huge(old_entry);
    invalidate_page(faulting_va);
    return 1;
}

static int walk_mapping(uint64_t va,
                        struct walk_result *result,
                        int split_huge_pages)
{
    if (!vmm_ready || !result || active_paging_levels != 4 ||
        !is_canonical_4level(va))
        return 0;

    volatile uint64_t *pml4 = table_from_phys(active_root_phys);
    if (!pml4)
        return 0;

    const uint16_t i4 = (uint16_t)((va >> 39) & 0x1FFU);
    const uint16_t i3 = (uint16_t)((va >> 30) & 0x1FFU);
    const uint16_t i2 = (uint16_t)((va >> 21) & 0x1FFU);
    const uint16_t i1 = (uint16_t)((va >> 12) & 0x1FFU);

    uint64_t e4 = pml4[i4];
    if (!(e4 & PTE_PRESENT))
        return 0;

    int effective_rw = (e4 & PTE_WRITABLE) != 0;
    int effective_user = (e4 & PTE_USER) != 0;
    int effective_nx = nx_is_enabled && ((e4 & PTE_NX) != 0);

    volatile uint64_t *pdpt = table_from_phys(entry_table_phys(e4));
    if (!pdpt)
        return 0;

    uint64_t e3 = pdpt[i3];
    if (!(e3 & PTE_PRESENT))
        return 0;

    effective_rw &= (e3 & PTE_WRITABLE) != 0;
    effective_user &= (e3 & PTE_USER) != 0;
    effective_nx |= nx_is_enabled && ((e3 & PTE_NX) != 0);

    if (e3 & PTE_PS) {
        if (split_huge_pages) {
            if (!split_1g_page(pdpt, i3, e3, va))
                return 0;
            return walk_mapping(va, result, split_huge_pages);
        }

        uint64_t base = e3 &
            (physical_address_mask & ~(PAGE_1G - 1ULL));
        result->leaf_entry = 0;
        result->raw_entry = e3;
        result->physical_address = base | (va & (PAGE_1G - 1ULL));
        result->page_size = PAGE_1G;
        result->effective_writable = effective_rw;
        result->effective_user = effective_user;
        result->effective_nx = effective_nx ||
                               (nx_is_enabled && (e3 & PTE_NX));
        result->ancestors_writable = effective_rw;
        result->ancestors_nx = effective_nx;
        return 1;
    }

    volatile uint64_t *pd = table_from_phys(entry_table_phys(e3));
    if (!pd)
        return 0;

    uint64_t e2 = pd[i2];
    if (!(e2 & PTE_PRESENT))
        return 0;

    effective_rw &= (e2 & PTE_WRITABLE) != 0;
    effective_user &= (e2 & PTE_USER) != 0;
    effective_nx |= nx_is_enabled && ((e2 & PTE_NX) != 0);

    if (e2 & PTE_PS) {
        if (split_huge_pages) {
            if (!split_2m_page(pd, i2, e2, va))
                return 0;
            return walk_mapping(va, result, split_huge_pages);
        }

        uint64_t base = e2 &
            (physical_address_mask & ~(PAGE_2M - 1ULL));
        result->leaf_entry = 0;
        result->raw_entry = e2;
        result->physical_address = base | (va & (PAGE_2M - 1ULL));
        result->page_size = PAGE_2M;
        result->effective_writable = effective_rw;
        result->effective_user = effective_user;
        result->effective_nx = effective_nx ||
                               (nx_is_enabled && (e2 & PTE_NX));
        result->ancestors_writable = effective_rw;
        result->ancestors_nx = effective_nx;
        return 1;
    }

    volatile uint64_t *pt = table_from_phys(entry_table_phys(e2));
    if (!pt)
        return 0;

    uint64_t e1 = pt[i1];
    if (!(e1 & PTE_PRESENT))
        return 0;

    result->leaf_entry = &pt[i1];
    result->raw_entry = e1;
    result->physical_address =
        (e1 & physical_address_mask) | (va & (VMM_PAGE_SIZE - 1ULL));
    result->page_size = VMM_PAGE_SIZE;
    result->effective_writable = effective_rw &&
                                 ((e1 & PTE_WRITABLE) != 0);
    result->effective_user = effective_user &&
                             ((e1 & PTE_USER) != 0);
    result->effective_nx = effective_nx ||
                           (nx_is_enabled && ((e1 & PTE_NX) != 0));
    result->ancestors_writable = effective_rw;
    result->ancestors_nx = effective_nx;
    return 1;
}

int vmm_query_mapping(uint64_t virtual_address,
                      struct vmm_mapping_info *info)
{
    if (!info)
        return 0;

    struct walk_result result;
    if (!walk_mapping(virtual_address, &result, 0))
        return 0;

    info->physical_address = result.physical_address;
    info->page_size = result.page_size;
    info->raw_entry = result.raw_entry;
    info->writable = (uint8_t)result.effective_writable;
    info->user = (uint8_t)result.effective_user;
    info->executable = (uint8_t)!result.effective_nx;
    return 1;
}

int vmm_nx_enabled(void)
{
    return nx_is_enabled;
}

int vmm_enable_protection(void)
{
    if (!vmm_ready || active_paging_levels != 4)
        return 0;

    if (!nx_supported)
        return 0;

    /* NXE must be enabled before bit 63 is interpreted as the NX flag. */
    uint64_t efer = read_msr(VMM_EFER_MSR);
    if (!(efer & VMM_EFER_NXE)) {
        write_msr(VMM_EFER_MSR, efer | VMM_EFER_NXE);
        efer = read_msr(VMM_EFER_MSR);
    }

    if (!(efer & VMM_EFER_NXE))
        return 0;

    nx_is_enabled = 1;

    /* CR0.WP makes supervisor writes respect read-only PTEs. */
    uint64_t cr0 = read_cr0();
    if (!(cr0 & VMM_CR0_WP)) {
        write_cr0(cr0 | VMM_CR0_WP);
        cr0 = read_cr0();
    }

    if (!(cr0 & VMM_CR0_WP))
        return 0;

    protection_ready = 1;
    return 1;
}

int vmm_protect_page(uint64_t virtual_address, uint32_t protection)
{
    if (!protection_ready || (protection & ~VMM_PROT_MASK))
        return 0;

    /* Enforce W^X: a page must never be writable and executable. */
    if ((protection & VMM_PROT_WRITE) &&
        (protection & VMM_PROT_EXEC))
        return 0;

    if (!(protection & VMM_PROT_EXEC) && !nx_is_enabled)
        return 0;

    if (virtual_address & (VMM_PAGE_SIZE - 1ULL))
        return 0;

    struct walk_result result;
    if (!walk_mapping(virtual_address, &result, 1))
        return 0;

    if (result.page_size != VMM_PAGE_SIZE || !result.leaf_entry)
        return 0;

    if ((protection & VMM_PROT_WRITE) && !result.ancestors_writable)
        return 0;

    if ((protection & VMM_PROT_EXEC) && result.ancestors_nx)
        return 0;

    uint64_t entry = *result.leaf_entry;
    entry &= ~(PTE_WRITABLE | PTE_USER | PTE_NX);

    if (protection & VMM_PROT_WRITE)
        entry |= PTE_WRITABLE;

    if (!(protection & VMM_PROT_EXEC))
        entry |= PTE_NX;

    *result.leaf_entry = entry;
    __asm__ volatile ("mfence" ::: "memory");
    invalidate_page(virtual_address);
    return 1;
}

int vmm_protect_range(uint64_t start,
                      uint64_t end,
                      uint32_t protection)
{
    if (start > end || (protection & ~VMM_PROT_MASK))
        return 0;

    if (start == end)
        return 1;

    if (end > UINT64_MAX - (VMM_PAGE_SIZE - 1ULL))
        return 0;

    uint64_t page = start & ~(VMM_PAGE_SIZE - 1ULL);
    uint64_t limit = (end + VMM_PAGE_SIZE - 1ULL) &
                     ~(VMM_PAGE_SIZE - 1ULL);

    for (; page < limit; page += VMM_PAGE_SIZE) {
        if (!vmm_protect_page(page, protection))
            return 0;
    }

    return 1;
}

int vmm_protect_range_and_hhdm_alias(uint64_t start,
                                     uint64_t end,
                                     uint32_t protection,
                                     uint32_t alias_protection)
{
    if (start > end || start == 0 ||
        (protection & ~VMM_PROT_MASK) ||
        (alias_protection & ~VMM_PROT_MASK))
        return 0;

    if ((protection & VMM_PROT_WRITE) && (protection & VMM_PROT_EXEC))
        return 0;
    if ((alias_protection & VMM_PROT_WRITE) &&
        (alias_protection & VMM_PROT_EXEC))
        return 0;

    if (start == end)
        return 1;
    if (end > UINT64_MAX - (VMM_PAGE_SIZE - 1ULL))
        return 0;

    uint64_t page = start & ~(VMM_PAGE_SIZE - 1ULL);
    uint64_t limit = (end + VMM_PAGE_SIZE - 1ULL) &
                     ~(VMM_PAGE_SIZE - 1ULL);

    for (; page < limit; page += VMM_PAGE_SIZE) {
        struct vmm_mapping_info info;
        if (!vmm_query_mapping(page, &info))
            return 0;

        uint64_t phys_page = info.physical_address &
                             ~(VMM_PAGE_SIZE - 1ULL);
        uintptr_t alias = (uintptr_t)pmm_phys_to_virt(phys_page);
        if (!alias)
            return 0;

        if (!vmm_protect_page(page, protection))
            return 0;

        if ((uint64_t)alias != page &&
            !vmm_protect_page((uint64_t)alias, alias_protection))
            return 0;
    }

    return 1;
}

/* ========================================================================== 
 * PAGE MAPPING AND ADDRESS-SPACE MANAGEMENT
 *
 * All page-table memory is allocated from PMM and accessed through the HHDM.
 * User address spaces share supervisor-only high-half root entries while
 * keeping their lower-half PML4 entries private.
 * ========================================================================== */

#define PTE_USER_ADDRESS  (1ULL << 2)
#define PTE_LARGE_PAGE    (1ULL << 7)
#define PTE_EXEC_DISABLE  (1ULL << 63)
#define USER_CANONICAL_TOP 0x0000800000000000ULL

static void zero_page_table(volatile uint64_t *table)
{
    for (uint16_t i = 0; i < TABLE_ENTRIES; i++)
        table[i] = 0;
}

static volatile uint64_t *allocate_table(uint64_t *phys_out)
{
    uint64_t phys = pmm_alloc();
    if (!phys)
        return 0;

    volatile uint64_t *table = table_from_phys(phys);
    if (!table) {
        (void)pmm_free(phys);
        return 0;
    }

    zero_page_table(table);
    *phys_out = phys;
    return table;
}

/* Ensure a non-leaf entry points to a page table. */
static volatile uint64_t *ensure_child_table(volatile uint64_t *parent,
                                             uint16_t index,
                                             int user_access)
{
    uint64_t entry = parent[index];
    if (!(entry & PTE_PRESENT)) {
        uint64_t child_phys = 0;
        if (!allocate_table(&child_phys))
            return 0;

        entry = child_phys | PTE_PRESENT | PTE_WRITABLE;
        if (user_access)
            entry |= PTE_USER_ADDRESS;
        parent[index] = entry;
        return table_from_phys(child_phys);
    }

    if (entry & PTE_LARGE_PAGE)
        return 0;

    if (!(entry & PTE_WRITABLE)) {
        entry |= PTE_WRITABLE;
        parent[index] = entry;
    }
    if (user_access && !(entry & PTE_USER_ADDRESS)) {
        entry |= PTE_USER_ADDRESS;
        parent[index] = entry;
    }

    return table_from_phys(entry_table_phys(entry));
}

static int map_page_in_root(uint64_t root_phys,
                            uint64_t virtual_address,
                            uint64_t physical_address,
                            uint32_t protection,
                            int user_mapping,
                            int uncached)
{
    if (active_paging_levels != 4 ||
        (virtual_address & (VMM_PAGE_SIZE - 1ULL)) ||
        (physical_address & (VMM_PAGE_SIZE - 1ULL)) ||
        physical_address == 0 || physical_address > physical_address_mask ||
        (!uncached && physical_address >= PMM_MAX_PHYS) ||
        (protection & ~(VMM_PROT_MASK | VMM_PROT_USER)) ||
        ((protection & VMM_PROT_WRITE) &&
         (protection & VMM_PROT_EXEC)))
        return 0;

    if (user_mapping) {
        if (!(protection & VMM_PROT_USER) ||
            virtual_address < VMM_PAGE_SIZE ||
            virtual_address >= USER_CANONICAL_TOP)
            return 0;
    } else {
        if ((protection & VMM_PROT_USER) ||
            !is_canonical_4level(virtual_address) ||
            ((virtual_address >> 47) & 1ULL) == 0)
            return 0;
    }

    volatile uint64_t *pml4 = table_from_phys(root_phys);
    if (!pml4)
        return 0;

    uint16_t i4 = (uint16_t)((virtual_address >> 39) & 0x1FFU);
    uint16_t i3 = (uint16_t)((virtual_address >> 30) & 0x1FFU);
    uint16_t i2 = (uint16_t)((virtual_address >> 21) & 0x1FFU);
    uint16_t i1 = (uint16_t)((virtual_address >> 12) & 0x1FFU);

    volatile uint64_t *pdpt = ensure_child_table(pml4, i4, user_mapping);
    if (!pdpt)
        return 0;
    volatile uint64_t *pd = ensure_child_table(pdpt, i3, user_mapping);
    if (!pd)
        return 0;
    volatile uint64_t *pt = ensure_child_table(pd, i2, user_mapping);
    if (!pt)
        return 0;

    if (pt[i1] & PTE_PRESENT)
        return 0;

    uint64_t leaf = (physical_address & physical_address_mask) |
                    PTE_PRESENT;
    if (protection & VMM_PROT_WRITE)
        leaf |= PTE_WRITABLE;
    if (user_mapping)
        leaf |= PTE_USER_ADDRESS;
    if (!(protection & VMM_PROT_EXEC))
        leaf |= PTE_EXEC_DISABLE;
    if (uncached)
        leaf |= PTE_PCD | PTE_PWT;

    pt[i1] = leaf;
    __asm__ volatile ("mfence" ::: "memory");
    if (root_phys == active_root_phys)
        invalidate_page(virtual_address);
    return 1;
}

int vmm_map_page(uint64_t virtual_address,
                 uint64_t physical_address,
                 uint32_t protection)
{
    if (!vmm_ready || !protection_ready)
        return 0;
    return map_page_in_root(active_root_phys, virtual_address,
                            physical_address, protection, 0, 0);
}

void *vmm_map_mmio(uint64_t physical_address, uint64_t length)
{
    if (!vmm_ready || !protection_ready || active_paging_levels != 4 ||
        physical_address == 0 || length == 0 ||
        physical_address > physical_address_mask ||
        length - 1ULL > physical_address_mask - physical_address)
        return 0;

    const uint64_t page_mask = VMM_PAGE_SIZE - 1ULL;
    uint64_t offset = physical_address & page_mask;
    uint64_t physical_base = physical_address & ~page_mask;
    if (length > UINT64_MAX - offset ||
        length + offset > UINT64_MAX - page_mask)
        return 0;
    uint64_t span = (length + offset + page_mask) & ~page_mask;
    if (span == 0 || next_mmio_virtual > VMM_MMIO_LIMIT ||
        span > VMM_MMIO_LIMIT - next_mmio_virtual ||
        VMM_PAGE_SIZE > VMM_MMIO_LIMIT - next_mmio_virtual - span)
        return 0;

    uint64_t virtual_base = next_mmio_virtual;
    uint64_t mapped = 0;
    for (; mapped < span; mapped += VMM_PAGE_SIZE) {
        uint64_t va = virtual_base + mapped;
        uint64_t pa = physical_base + mapped;

        /* Never replace an existing mapping, even inside our MMIO window. */
        struct vmm_mapping_info existing;
        if (vmm_query_mapping(va, &existing) ||
            !map_page_in_root(active_root_phys, va, pa,
                              VMM_PROT_WRITE, 0, 1))
            break;
    }

    if (mapped != span) {
        while (mapped != 0) {
            uint64_t ignored = 0;
            mapped -= VMM_PAGE_SIZE;
            (void)vmm_unmap_page(virtual_base + mapped, &ignored);
        }
        return 0;
    }

    next_mmio_virtual = virtual_base + span + VMM_PAGE_SIZE;
    return (void *)(uintptr_t)(virtual_base + offset);
}

int vmm_unmap_page(uint64_t virtual_address, uint64_t *physical_out)
{
    if (!vmm_ready || !protection_ready ||
        (virtual_address & (VMM_PAGE_SIZE - 1ULL)) ||
        !is_canonical_4level(virtual_address))
        return 0;

    struct walk_result result;
    if (!walk_mapping(virtual_address, &result, 1) ||
        result.page_size != VMM_PAGE_SIZE || !result.leaf_entry)
        return 0;

    uint64_t phys = result.physical_address & ~(VMM_PAGE_SIZE - 1ULL);
    *result.leaf_entry = 0;
    __asm__ volatile ("mfence" ::: "memory");
    invalidate_page(virtual_address);
    if (physical_out)
        *physical_out = phys;
    return 1;
}

static int valid_address_space(const struct vmm_address_space *space)
{
    return space && space->valid && space->root_phys &&
           !(space->root_phys & (VMM_PAGE_SIZE - 1ULL)) &&
           table_from_phys(space->root_phys) != 0;
}

int vmm_address_space_create(struct vmm_address_space *space)
{
    if (!vmm_ready || !space || active_paging_levels != 4)
        return 0;

    space->valid = 0;
    space->root_phys = 0;
    for (size_t i = 0; i < sizeof(space->reserved); i++)
        space->reserved[i] = 0;

    uint64_t root_phys = 0;
    volatile uint64_t *new_root = allocate_table(&root_phys);
    if (!new_root)
        return 0;

    volatile uint64_t *kernel_root = table_from_phys(kernel_root_phys);
    if (!kernel_root) {
        (void)pmm_free(root_phys);
        return 0;
    }

    /* Copy upper canonical half, but clear U/S at its root entries. */
    for (uint16_t i = 256; i < TABLE_ENTRIES; i++) {
        uint64_t entry = kernel_root[i];
        if (entry & PTE_PRESENT)
            new_root[i] = entry & ~PTE_USER_ADDRESS;
    }

    space->root_phys = root_phys;
    space->valid = 1;
    return 1;
}

static int address_space_walk_4k(struct vmm_address_space *space,
                                 uint64_t virtual_address,
                                 volatile uint64_t **leaf_out,
                                 uint64_t *leaf_value_out)
{
    if (!valid_address_space(space) ||
        virtual_address < VMM_PAGE_SIZE ||
        virtual_address >= USER_CANONICAL_TOP ||
        (virtual_address & (VMM_PAGE_SIZE - 1ULL)))
        return 0;

    volatile uint64_t *pml4 = table_from_phys(space->root_phys);
    if (!pml4)
        return 0;

    uint16_t i4 = (uint16_t)((virtual_address >> 39) & 0x1FFU);
    uint16_t i3 = (uint16_t)((virtual_address >> 30) & 0x1FFU);
    uint16_t i2 = (uint16_t)((virtual_address >> 21) & 0x1FFU);
    uint16_t i1 = (uint16_t)((virtual_address >> 12) & 0x1FFU);

    uint64_t e4 = pml4[i4];
    if (!(e4 & PTE_PRESENT) || (e4 & PTE_LARGE_PAGE))
        return 0;
    volatile uint64_t *pdpt = table_from_phys(entry_table_phys(e4));
    if (!pdpt)
        return 0;

    uint64_t e3 = pdpt[i3];
    if (!(e3 & PTE_PRESENT) || (e3 & PTE_LARGE_PAGE))
        return 0;
    volatile uint64_t *pd = table_from_phys(entry_table_phys(e3));
    if (!pd)
        return 0;

    uint64_t e2 = pd[i2];
    if (!(e2 & PTE_PRESENT) || (e2 & PTE_LARGE_PAGE))
        return 0;
    volatile uint64_t *pt = table_from_phys(entry_table_phys(e2));
    if (!pt)
        return 0;

    if (!(pt[i1] & PTE_PRESENT))
        return 0;
    if (leaf_out)
        *leaf_out = &pt[i1];
    if (leaf_value_out)
        *leaf_value_out = pt[i1];
    return 1;
}

int vmm_address_space_map_user_page(struct vmm_address_space *space,
                                    uint64_t virtual_address,
                                    uint32_t protection,
                                    uint64_t *physical_out)
{
    if (!valid_address_space(space) || !(protection & VMM_PROT_USER))
        return 0;

    uint64_t phys = pmm_alloc();
    if (!phys)
        return 0;

    volatile uint64_t *page =
        (volatile uint64_t *)pmm_phys_to_virt(phys);
    if (!page) {
        (void)pmm_free(phys);
        return 0;
    }
    for (uint64_t i = 0; i < VMM_PAGE_SIZE / sizeof(uint64_t); i++)
        page[i] = 0;

    if (!map_page_in_root(space->root_phys, virtual_address,
                          phys, protection, 1, 0)) {
        (void)pmm_free(phys);
        return 0;
    }

    if (physical_out)
        *physical_out = phys;
    return 1;
}

int vmm_address_space_query(struct vmm_address_space *space,
                            uint64_t virtual_address,
                            struct vmm_mapping_info *info)
{
    uint64_t entry = 0;
    if (!info ||
        !address_space_walk_4k(space, virtual_address, 0, &entry))
        return 0;

    info->physical_address = (entry & physical_address_mask) |
                             (virtual_address & (VMM_PAGE_SIZE - 1ULL));
    info->page_size = VMM_PAGE_SIZE;
    info->raw_entry = entry;
    info->writable = (entry & PTE_WRITABLE) != 0;
    info->user = (entry & PTE_USER_ADDRESS) != 0;
    info->executable = (entry & PTE_EXEC_DISABLE) == 0;
    return 1;
}

int vmm_address_space_unmap_user_page(struct vmm_address_space *space,
                                      uint64_t virtual_address)
{
    volatile uint64_t *leaf = 0;
    uint64_t entry = 0;
    if (!address_space_walk_4k(space, virtual_address, &leaf, &entry) ||
        !(entry & PTE_USER_ADDRESS) || !leaf)
        return 0;

    *leaf = 0;
    __asm__ volatile ("mfence" ::: "memory");
    if (space->root_phys == active_root_phys)
        invalidate_page(virtual_address);

    uint64_t phys = entry & physical_address_mask;
    return pmm_free(phys);
}

int vmm_address_space_activate(struct vmm_address_space *space)
{
    if (!valid_address_space(space))
        return 0;

    __asm__ volatile ("mov %0, %%cr3" :: "r"(space->root_phys) : "memory");
    active_root_phys = space->root_phys;
    active_cr3 = space->root_phys;
    return read_cr3() == space->root_phys;
}

int vmm_activate_kernel_address_space(void)
{
    if (!vmm_ready || !kernel_root_phys || !table_from_phys(kernel_root_phys))
        return 0;

    __asm__ volatile ("mov %0, %%cr3" :: "r"(kernel_root_phys) : "memory");
    active_root_phys = kernel_root_phys;
    active_cr3 = kernel_root_phys;
    return read_cr3() == kernel_root_phys;
}

static int address_space_tree_has_huge_pages(struct vmm_address_space *space)
{
    volatile uint64_t *root = table_from_phys(space->root_phys);
    if (!root)
        return 1;

    for (uint16_t i4 = 0; i4 < 256; i4++) {
        if (!(root[i4] & PTE_PRESENT))
            continue;
        volatile uint64_t *pdpt = table_from_phys(entry_table_phys(root[i4]));
        if (!pdpt) return 1;
        for (uint16_t i3 = 0; i3 < TABLE_ENTRIES; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            if (pdpt[i3] & PTE_LARGE_PAGE) return 1;
            volatile uint64_t *pd = table_from_phys(entry_table_phys(pdpt[i3]));
            if (!pd) return 1;
            for (uint16_t i2 = 0; i2 < TABLE_ENTRIES; i2++) {
                if (!(pd[i2] & PTE_PRESENT)) continue;
                if (pd[i2] & PTE_LARGE_PAGE) return 1;
                if (!table_from_phys(entry_table_phys(pd[i2]))) return 1;
            }
        }
    }
    return 0;
}

int vmm_address_space_destroy(struct vmm_address_space *space)
{
    if (!valid_address_space(space) ||
        space->root_phys == active_root_phys ||
        address_space_tree_has_huge_pages(space))
        return 0;

    volatile uint64_t *root = table_from_phys(space->root_phys);
    if (!root)
        return 0;

    for (uint16_t i4 = 0; i4 < 256; i4++) {
        if (!(root[i4] & PTE_PRESENT)) continue;
        uint64_t pdpt_phys = entry_table_phys(root[i4]);
        volatile uint64_t *pdpt = table_from_phys(pdpt_phys);
        for (uint16_t i3 = 0; i3 < TABLE_ENTRIES; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            uint64_t pd_phys = entry_table_phys(pdpt[i3]);
            volatile uint64_t *pd = table_from_phys(pd_phys);
            for (uint16_t i2 = 0; i2 < TABLE_ENTRIES; i2++) {
                if (!(pd[i2] & PTE_PRESENT)) continue;
                uint64_t pt_phys = entry_table_phys(pd[i2]);
                volatile uint64_t *pt = table_from_phys(pt_phys);
                for (uint16_t i1 = 0; i1 < TABLE_ENTRIES; i1++) {
                    if (!(pt[i1] & PTE_PRESENT)) continue;
                    uint64_t page_phys = pt[i1] & physical_address_mask;
                    (void)pmm_free(page_phys);
                }
                (void)pmm_free(pt_phys);
            }
            (void)pmm_free(pd_phys);
        }
        (void)pmm_free(pdpt_phys);
    }

    uint64_t root_phys = space->root_phys;
    space->root_phys = 0;
    space->valid = 0;
    for (size_t i = 0; i < sizeof(space->reserved); i++)
        space->reserved[i] = 0;
    return pmm_free(root_phys);
}

