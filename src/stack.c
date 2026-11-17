#include <stdint.h>

#include "stack.h"
#include "gdt.h"
#include "pmm.h"
#include "vmm.h"

int kernel_guarded_ist_stack_init(uint64_t *guard_base_out,
                                  uint64_t *stack_bottom_out,
                                  uint64_t *stack_top_out)
{
    const uint64_t guard = VMM_KERNEL_STACK_GUARD_BASE;
    const uint64_t stack_bottom = guard + VMM_PAGE_SIZE;
    const uint64_t stack_top = stack_bottom +
                               VMM_KERNEL_STACK_PAGES * VMM_PAGE_SIZE;

    struct vmm_mapping_info info;
    if (vmm_query_mapping(guard, &info))
        return 0;

    uint64_t mapped = 0;
    for (uint64_t i = 0; i < VMM_KERNEL_STACK_PAGES; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys)
            goto rollback;

        volatile uint64_t *page =
            (volatile uint64_t *)pmm_phys_to_virt(phys);
        if (!page) {
            (void)pmm_free(phys);
            goto rollback;
        }
        for (uint64_t j = 0; j < VMM_PAGE_SIZE / sizeof(uint64_t); j++)
            page[j] = 0;

        if (!vmm_map_page(stack_bottom + i * VMM_PAGE_SIZE,
                          phys, VMM_PROT_WRITE)) {
            (void)pmm_free(phys);
            goto rollback;
        }
        mapped++;
    }

    if (vmm_query_mapping(guard, &info))
        goto rollback;

    if (!gdt_ist_set_stack((uintptr_t)stack_bottom,
                           (uintptr_t)stack_top))
        goto rollback;

    if (guard_base_out) *guard_base_out = guard;
    if (stack_bottom_out) *stack_bottom_out = stack_bottom;
    if (stack_top_out) *stack_top_out = stack_top;
    return 1;

rollback:
    for (uint64_t i = 0; i < mapped; i++) {
        uint64_t phys = 0;
        if (vmm_unmap_page(stack_bottom + i * VMM_PAGE_SIZE, &phys))
            (void)pmm_free(phys);
    }
    return 0;
}
