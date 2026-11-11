#include <stdint.h>
#include <stddef.h>

#include "idt.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256]
    __attribute__((aligned(16)));

static struct idt_pointer idtr = {
    .limit = sizeof(idt) - 1,
    .base = (uint64_t)(uintptr_t)&idt
};

extern void *isr_stub_table[32];

static void idt_set_gate(
    uint8_t vector,
    uintptr_t handler,
    uint8_t ist
) {
    struct idt_entry *entry = &idt[vector];

    entry->offset_low  = handler & 0xFFFF;
    entry->selector    = 0x08;
    entry->ist         = ist & 0x07;
    entry->type_attr   = 0x8E;
    entry->offset_mid  = (handler >> 16) & 0xFFFF;
    entry->offset_high = (handler >> 32) & 0xFFFFFFFF;
    entry->reserved    = 0;
}

void idt_init(void)
{
    __asm__ volatile ("cli");

    for (uint16_t i = 0; i < 256; i++) {
        idt[i] = (struct idt_entry){0};
    }

    for (uint8_t i = 0; i < 32; i++) {
        uint8_t ist = (i == 3 || i == 8) ? 1 : 0;

        idt_set_gate(
            i,
            (uintptr_t)isr_stub_table[i],
            ist
        );
    }

    __asm__ volatile (
        "lidt %0"
        :
        : "m"(idtr)
        : "memory"
    );
}

struct idtr_snapshot {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

int idt_verify(
    uint16_t *limit_out,
    uint64_t *base_out
) {
    struct idtr_snapshot current;

    __asm__ volatile (
        "sidt %0"
        : "=m"(current)
        :
        : "memory"
    );

    if (limit_out)
        *limit_out = current.limit;

    if (base_out)
        *base_out = current.base;

    return current.limit == sizeof(idt) - 1 &&
           current.base == (uint64_t)(uintptr_t)&idt;
}

/* =========================================
 * DOUBLE FAULT IST VERIFICATION
 * ========================================= */

int idt_df_ist_verify(uint8_t *ist_out)
{
    uint8_t ist = idt[8].ist & 0x07;

    if (ist_out)
        *ist_out = ist;

    return ist == 1 &&
           idt[8].selector == 0x08 &&
           idt[8].type_attr == 0x8E;
}

int idt_vector_uses_ist(uint8_t vector)
{
    return (idt[vector].ist & 0x07) != 0;
}