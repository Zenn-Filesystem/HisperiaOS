#ifndef HISPERIA_VMM_H
#define HISPERIA_VMM_H

#include <stdint.h>

#define VMM_PAGE_SIZE   4096ULL
#define VMM_PROT_WRITE  (1U << 0)
#define VMM_PROT_EXEC   (1U << 1)
#define VMM_PROT_USER   (1U << 2)
#define VMM_PROT_MASK   (VMM_PROT_WRITE | VMM_PROT_EXEC)

#define VMM_KERNEL_STACK_GUARD_BASE 0xFFFFC00000000000ULL
#define VMM_KERNEL_HEAP_BASE        0xFFFFC08000000000ULL
#define VMM_KERNEL_STACK_PAGES      4ULL

struct vmm_address_space {
    uint64_t root_phys;
    uint8_t valid;
    uint8_t reserved[7];
};

struct vmm_mapping_info {
    uint64_t physical_address;
    uint64_t page_size;
    uint64_t raw_entry;
    uint8_t writable;
    uint8_t user;
    uint8_t executable;
};

/* Inspect the paging context installed by Limine. */
int vmm_bootstrap_init(void);
uint64_t vmm_root_phys(void);
uint8_t vmm_paging_levels(void);
uint64_t vmm_cr3_value(void);

/* Read-only page-table inspection. */
int vmm_inspect_root(uint64_t *present_count);
int vmm_read_root_entry(uint16_t index, uint64_t *entry_out);
int vmm_read_table_entry(uint64_t table_phys,
                         uint16_t index,
                         uint64_t *entry_out);
int vmm_query_mapping(uint64_t virtual_address,
                      struct vmm_mapping_info *info);

/* Enable architectural write/NX protection. */
int vmm_enable_protection(void);
int vmm_nx_enabled(void);

/* Protect existing mappings. No new virtual address is created. */
int vmm_protect_page(uint64_t virtual_address, uint32_t protection);
int vmm_protect_range(uint64_t start, uint64_t end, uint32_t protection);
int vmm_protect_range_and_hhdm_alias(uint64_t start,
                                     uint64_t end,
                                     uint32_t protection,
                                     uint32_t alias_protection);

/* 4 KiB supervisor mapping primitives in the currently active root. */
int vmm_map_page(uint64_t virtual_address, uint64_t physical_address,
                 uint32_t protection);
/* Map device MMIO in a dedicated virtual window using uncached PCD+PWT pages. */
void *vmm_map_mmio(uint64_t physical_address, uint64_t length);
int vmm_unmap_page(uint64_t virtual_address, uint64_t *physical_out);

/* Isolated lower-half user address spaces; high-half kernel mappings are shared. */
int vmm_address_space_create(struct vmm_address_space *space);
int vmm_address_space_destroy(struct vmm_address_space *space);
int vmm_address_space_activate(struct vmm_address_space *space);
int vmm_activate_kernel_address_space(void);
int vmm_address_space_map_user_page(struct vmm_address_space *space,
                                    uint64_t virtual_address,
                                    uint32_t protection,
                                    uint64_t *physical_out);
int vmm_address_space_unmap_user_page(struct vmm_address_space *space,
                                      uint64_t virtual_address);
int vmm_address_space_query(struct vmm_address_space *space,
                            uint64_t virtual_address,
                            struct vmm_mapping_info *info);

#endif
