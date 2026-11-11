#include <stdint.h>
#include <stddef.h>

#include "gdt.h"
#include "tss.h"

/* =========================================
 * GDT
 * ========================================= */

static uint64_t gdt[5] __attribute__((aligned(16))) = {
    0x0000000000000000ULL, // Null
    0x00AF9A000000FFFFULL, // Kernel Code
    0x00CF92000000FFFFULL, // Kernel Data
    0,                     // TSS low
    0                      // TSS high
};

/* =========================================
 * TSS
 * ========================================= */

static struct tss tss = {
    .iomap_base = sizeof(struct tss)
};

/* =========================================
 * INTERRUPT STACK TABLE
 * ========================================= */

#define DF_IST_STACK_SIZE (16 * 1024)

static uint8_t df_ist_stack[DF_IST_STACK_SIZE]
    __attribute__((aligned(16)));

/* =========================================
 * GDTR
 * ========================================= */

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_pointer gdtr = {
    .limit = sizeof(gdt) - 1,
    .base = (uint64_t)(uintptr_t)&gdt
};

/* =========================================
 * TSS DESCRIPTOR ENCODING
 * ========================================= */

static void tss_descriptor_set(uint64_t base, uint32_t limit)
{
    uint64_t low = 0;
    uint64_t high = 0;

    low |= (uint64_t)(limit & 0xFFFF);
    low |= (base & 0xFFFFFF) << 16;

    // Present + DPL 0 + 64-bit available TSS
    low |= 0x89ULL << 40;

    low |= (uint64_t)((limit >> 16) & 0x0F) << 48;
    low |= ((base >> 24) & 0xFF) << 56;

    high = base >> 32;

    gdt[3] = low;
    gdt[4] = high;
}

/* =========================================
 * ASSEMBLY INTERFACE
 * ========================================= */

extern void gdt_load(const struct gdt_pointer *ptr);

/* =========================================
 * INITIALIZATION
 * ========================================= */

void gdt_init(void)
{
    tss_descriptor_set(
        (uint64_t)(uintptr_t)&tss,
        sizeof(tss) - 1
    );

    gdt_load(&gdtr);
}

/* =========================================
 * GDTR VERIFICATION
 * ========================================= */

struct gdtr_snapshot {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

int gdt_verify(uint16_t *limit_out, uint64_t *base_out)
{
    struct gdtr_snapshot current;

    __asm__ volatile (
        "sgdt %0"
        : "=m"(current)
        :
        : "memory"
    );

    if (limit_out)
        *limit_out = current.limit;

    if (base_out)
        *base_out = current.base;

    return current.limit == sizeof(gdt) - 1 &&
           current.base == (uint64_t)(uintptr_t)&gdt;
}

/* =========================================
 * TASK REGISTER VERIFICATION
 * ========================================= */

int gdt_tss_verify(uint16_t *selector_out)
{
    uint16_t selector;

    __asm__ volatile (
        "str %0"
        : "=r"(selector)
    );

    if (selector_out)
        *selector_out = selector;

    return selector == 0x18;
}

/* =========================================
 * IST INITIALIZATION
 * ========================================= */

void gdt_ist_init(void)
{
    uintptr_t stack_top =
        (uintptr_t)(df_ist_stack + sizeof(df_ist_stack));

    tss.ist1 = (uint64_t)stack_top;
}

int gdt_ist1_contains(uintptr_t address)
{
    uintptr_t bottom = (uintptr_t)df_ist_stack;
    uintptr_t top = bottom + sizeof(df_ist_stack);

    return address >= bottom && address < top;
}

/* =========================================
 * IST VERIFICATION
 * ========================================= */

int gdt_ist1_verify(uint64_t *stack_top_out)
{
    uint64_t expected =
        (uint64_t)(uintptr_t)(
            df_ist_stack + sizeof(df_ist_stack)
        );

    uint64_t actual = tss.ist1;

    if (stack_top_out)
        *stack_top_out = actual;

    return actual == expected &&
           (actual & 0xF) == 0;
}