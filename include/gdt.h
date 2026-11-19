#ifndef HISPERIA_GDT_H
#define HISPERIA_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE_SELECTOR 0x08U
#define GDT_KERNEL_DATA_SELECTOR 0x10U
#define GDT_TSS_SELECTOR         0x18U
#define GDT_USER_DATA_SELECTOR   0x2BU
#define GDT_USER_CODE_SELECTOR   0x33U

void gdt_init(void);
int gdt_user_segments_verify(void);
int gdt_set_rsp0(uintptr_t stack_top);
int gdt_rsp0_verify(uintptr_t expected);

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