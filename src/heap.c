#include <stddef.h>
#include <stdint.h>

#include "heap.h"
#include "pmm.h"
#include "vmm.h"

#define HEAP_MAGIC       0x48535048U
#define HEAP_ALIGNMENT   16ULL
#define HEAP_MAX_BYTES   (64ULL * 1024ULL * 1024ULL)

struct heap_block {
    size_t size;
    struct heap_block *previous;
    struct heap_block *next;
    uint32_t magic;
    uint32_t is_free;
};

static struct heap_block *heap_first;
static struct heap_block *heap_last;
static uint64_t heap_end;
static uint64_t heap_mapped;
static int heap_ready;

static size_t align_size(size_t value)
{
    if (value > SIZE_MAX - (HEAP_ALIGNMENT - 1ULL))
        return 0;
    return (value + HEAP_ALIGNMENT - 1ULL) & ~(HEAP_ALIGNMENT - 1ULL);
}

static int heap_map_one_page(void)
{
    if (!heap_ready || heap_mapped > HEAP_MAX_BYTES - VMM_PAGE_SIZE)
        return 0;

    uint64_t phys = pmm_alloc();
    if (!phys)
        return 0;

    volatile uint64_t *page =
        (volatile uint64_t *)pmm_phys_to_virt(phys);
    if (!page) {
        (void)pmm_free(phys);
        return 0;
    }
    for (uint64_t i = 0; i < VMM_PAGE_SIZE / sizeof(uint64_t); i++)
        page[i] = 0;

    if (!vmm_map_page(heap_end, phys, VMM_PROT_WRITE)) {
        (void)pmm_free(phys);
        return 0;
    }

    if (heap_last && heap_last->is_free &&
        (uint64_t)(uintptr_t)heap_last + sizeof(*heap_last) +
            heap_last->size == heap_end) {
        heap_last->size += (size_t)VMM_PAGE_SIZE;
    } else {
        struct heap_block *block = (struct heap_block *)(uintptr_t)heap_end;
        block->size = (size_t)VMM_PAGE_SIZE - sizeof(*block);
        block->previous = heap_last;
        block->next = 0;
        block->magic = HEAP_MAGIC;
        block->is_free = 1;
        if (heap_last)
            heap_last->next = block;
        else
            heap_first = block;
        heap_last = block;
    }

    heap_end += VMM_PAGE_SIZE;
    heap_mapped += VMM_PAGE_SIZE;
    return 1;
}

int kheap_init(void)
{
    if (heap_ready)
        return 1;

    heap_first = 0;
    heap_last = 0;
    heap_end = VMM_KERNEL_HEAP_BASE;
    heap_mapped = 0;
    heap_ready = 1;

    if (!heap_map_one_page()) {
        heap_ready = 0;
        return 0;
    }
    return 1;
}

void *kmalloc(size_t size)
{
    if (!heap_ready || size == 0)
        return 0;

    size = align_size(size);
    if (!size || size > HEAP_MAX_BYTES - sizeof(struct heap_block))
        return 0;

    for (;;) {
        struct heap_block *block = heap_first;
        while (block) {
            if (block->magic != HEAP_MAGIC)
                return 0;
            if (block->is_free && block->size >= size) {
                size_t remainder = block->size - size;
                if (remainder >= sizeof(struct heap_block) + HEAP_ALIGNMENT) {
                    struct heap_block *split =
                        (struct heap_block *)((uintptr_t)(block + 1) + size);
                    split->size = remainder - sizeof(*split);
                    split->previous = block;
                    split->next = block->next;
                    split->magic = HEAP_MAGIC;
                    split->is_free = 1;
                    if (split->next)
                        split->next->previous = split;
                    else
                        heap_last = split;
                    block->next = split;
                    block->size = size;
                }
                block->is_free = 0;
                return (void *)(block + 1);
            }
            block = block->next;
        }

        if (!heap_map_one_page())
            return 0;
    }
}

void kfree(void *pointer)
{
    if (!heap_ready || !pointer)
        return;

    uintptr_t address = (uintptr_t)pointer;
    if (address < VMM_KERNEL_HEAP_BASE + sizeof(struct heap_block) ||
        address >= heap_end || (address & (HEAP_ALIGNMENT - 1ULL)))
        return;

    /* Validate against the block list before dereferencing a candidate header. */
    struct heap_block *block = heap_first;
    while (block && (void *)(block + 1) != pointer) {
        if (block->magic != HEAP_MAGIC)
            return;
        block = block->next;
    }
    if (!block || block->magic != HEAP_MAGIC || block->is_free)
        return;

    block->is_free = 1;

    if (block->next && block->next->magic == HEAP_MAGIC &&
        block->next->is_free &&
        (uint8_t *)(block + 1) + block->size == (uint8_t *)block->next) {
        struct heap_block *next = block->next;
        block->size += sizeof(*next) + next->size;
        block->next = next->next;
        if (block->next)
            block->next->previous = block;
        else
            heap_last = block;
    }

    if (block->previous && block->previous->magic == HEAP_MAGIC &&
        block->previous->is_free &&
        (uint8_t *)(block->previous + 1) + block->previous->size ==
            (uint8_t *)block) {
        struct heap_block *previous = block->previous;
        previous->size += sizeof(*block) + block->size;
        previous->next = block->next;
        if (previous->next)
            previous->next->previous = previous;
        else
            heap_last = previous;
    }
}

size_t kheap_bytes_mapped(void)
{
    return (size_t)heap_mapped;
}

int kheap_self_test(void)
{
    if (!heap_ready)
        return 0;

    uint8_t *a = (uint8_t *)kmalloc(37);
    uint64_t *b = (uint64_t *)kmalloc(1024);
    uint8_t *c = (uint8_t *)kmalloc(8192);
    if (!a || !b || !c) {
        kfree(a);
        kfree(b);
        kfree(c);
        return 0;
    }

    for (size_t i = 0; i < 37; i++) a[i] = (uint8_t)(i ^ 0xA5U);
    for (size_t i = 0; i < 128; i++) b[i] = 0x4853505249410000ULL | i;
    for (size_t i = 0; i < 8192; i++) c[i] = (uint8_t)(i * 13U);

    int ok = (((uintptr_t)a & (HEAP_ALIGNMENT - 1ULL)) == 0) &&
             (((uintptr_t)b & (HEAP_ALIGNMENT - 1ULL)) == 0) &&
             (((uintptr_t)c & (HEAP_ALIGNMENT - 1ULL)) == 0);
    for (size_t i = 0; i < 37; i++)
        if (a[i] != (uint8_t)(i ^ 0xA5U)) ok = 0;
    for (size_t i = 0; i < 128; i++)
        if (b[i] != (0x4853505249410000ULL | i)) ok = 0;
    for (size_t i = 0; i < 8192; i++)
        if (c[i] != (uint8_t)(i * 13U)) ok = 0;

    kfree(b);
    kfree(a);
    kfree(c);
    kfree(a); /* double-free must be ignored */
    return ok;
}
