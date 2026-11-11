
#include <stdint.h>

#include "gdt.h"
#include "idt.h"
#include "interrupt.h"

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

    if (idt_vector_uses_ist((uint8_t)frame->vector)) {
        int passed = gdt_ist1_contains((uintptr_t)frame);
        isr_ist_report(passed);
    }

    /* Exception fatal: berhenti dengan aman. */
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
