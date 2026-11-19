#include <stdint.h>
#include <stddef.h>

#include "gdt.h"
#include "tss.h"

/* =========================================
 * GDT
 * ========================================= */

static uint64_t gdt[7] __attribute__((aligned(16))) = {
    0x0000000000000000ULL, // 0x00: Null
    0x00AF9A000000FFFFULL, // 0x08: Kernel Code, DPL 0
    0x00CF92000000FFFFULL, // 0x10: Kernel Data, DPL 0
    0,                     // 0x18: TSS low
    0,                     // 0x20: TSS high
    0x00CFF2000000FFFFULL, // 0x28: User Data, DPL 3
    0x00AFFA000000FFFFULL  // 0x30: User Code, DPL 3, 64-bit
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

static uintptr_t ist1_stack_bottom;
static uintptr_t ist1_stack_top;

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
 * USER SEGMENTS AND RING-0 ENTRY STACK
 * ========================================= */

int gdt_user_segments_verify(void)
{
    return gdt[5] == 0x00CFF2000000FFFFULL &&
           gdt[6] == 0x00AFFA000000FFFFULL;
}

int gdt_set_rsp0(uintptr_t stack_top)
{
    if (!stack_top || (stack_top & 0xFULL))
        return 0;

    tss.rsp0 = (uint64_t)stack_top;
    __asm__ volatile ("mfence" ::: "memory");
    return tss.rsp0 == (uint64_t)stack_top;
}

int gdt_rsp0_verify(uintptr_t expected)
{
    return expected && !(expected & 0xFULL) &&
           tss.rsp0 == (uint64_t)expected;
}

/* =========================================
 * IST INITIALIZATION
 * ========================================= */

void gdt_ist_init(void)
{
    uintptr_t stack_top =
        (uintptr_t)(df_ist_stack + sizeof(df_ist_stack));

    tss.ist1 = (uint64_t)stack_top;
    ist1_stack_bottom = (uintptr_t)df_ist_stack;
    ist1_stack_top = stack_top;
}

int gdt_ist_set_stack(uintptr_t stack_bottom, uintptr_t stack_top)
{
    if (!stack_bottom || stack_top <= stack_bottom || (stack_top & 0xFULL))
        return 0;

    tss.ist1 = (uint64_t)stack_top;
    ist1_stack_bottom = stack_bottom;
    ist1_stack_top = stack_top;
    return 1;
}

int gdt_ist1_contains(uintptr_t address)
{
    return address >= ist1_stack_bottom && address < ist1_stack_top;
}

/* =========================================
 * IST VERIFICATION
 * ========================================= */

int gdt_ist1_verify(uint64_t *stack_top_out)
{
    uint64_t expected = (uint64_t)ist1_stack_top;

    uint64_t actual = tss.ist1;

    if (stack_top_out)
        *stack_top_out = actual;

    return ist1_stack_bottom < ist1_stack_top &&
           actual == expected && (actual & 0xF) == 0;
}