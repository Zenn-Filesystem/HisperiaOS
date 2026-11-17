#ifndef HISPERIA_VMM_H
#define HISPERIA_VMM_H

#include <stdint.h>

/*
 * VMM bootstrap: inspect the paging context installed by Limine.
 * This stage does not modify CR3 or any page-table entry.
 */
int vmm_bootstrap_init(void);

/* Physical address of the currently active top-level page table. */
uint64_t vmm_root_phys(void);

/* Current paging depth: 4 or 5 levels. */
uint8_t vmm_paging_levels(void);

/* CR3 value captured during bootstrap (includes low control/PCID bits). */
uint64_t vmm_cr3_value(void);

int vmm_inspect_root(uint64_t *present_count);

#endif
