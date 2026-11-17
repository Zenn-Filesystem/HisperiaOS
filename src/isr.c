
#include <stdint.h>

#include "gdt.h"
#include "idt.h"
#include "interrupt.h"

#ifndef HISPERIA_RUN_MPROTECT_TEST
#define HISPERIA_RUN_MPROTECT_TEST 0
#endif

extern const volatile uint64_t vmm_rodata_probe;
extern volatile uint64_t vmm_data_probe;
void isr_test_report(int passed);

void isr_dispatch(struct interrupt_frame *frame)
{
    __asm__ volatile ("cli");

    /*
     * Stack berpindah jika:
     * 1. Gate menggunakan IST.
     * 2. Exception datang dari privilege level berbeda.
     */
    int stack_switched =
        idt_vector_uses_ist((uint8_t)frame->vector) ||
        ((frame->cs & 0x03) != 0);

    isr_report(frame, stack_switched);

#if HISPERIA_RUN_MPROTECT_TEST == 1 || HISPERIA_RUN_MPROTECT_TEST == 2
    if (frame->vector == 14) {
        uint64_t fault_address = 0;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
        int expected = 0;
#if HISPERIA_RUN_MPROTECT_TEST == 1
        uint64_t expected_page =
            (uint64_t)(uintptr_t)&vmm_rodata_probe & ~(4095ULL);
        expected = ((frame->error_code & 0x1FULL) == 0x03ULL) &&
                   ((fault_address & ~(4095ULL)) == expected_page);
#elif HISPERIA_RUN_MPROTECT_TEST == 2
        uint64_t expected_page =
            (uint64_t)(uintptr_t)&vmm_data_probe & ~(4095ULL);
        expected = ((frame->error_code & 0x1FULL) == 0x11ULL) &&
                   ((fault_address & ~(4095ULL)) == expected_page);
#endif
        isr_test_report(expected);
    }
#endif

    if (idt_vector_uses_ist((uint8_t)frame->vector)) {
        int passed = gdt_ist1_contains((uintptr_t)frame);
        isr_ist_report(passed);
    }

    /* Exception fatal: berhenti dengan aman. */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
