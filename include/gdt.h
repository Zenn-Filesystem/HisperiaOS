#ifndef HISPERIA_GDT_H
#define HISPERIA_GDT_H

#include <stdint.h>

void gdt_init(void);

int gdt_verify(
    uint16_t *limit_out,
    uint64_t *base_out
);

int gdt_tss_verify(uint16_t *selector_out);

/* Interrupt Stack Table */
void gdt_ist_init(void);

int gdt_ist1_contains(uintptr_t address);

int gdt_ist1_verify(uint64_t *stack_top_out);
int gdt_ist_set_stack(uintptr_t stack_bottom, uintptr_t stack_top);

#endif