#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include "pmm.h"

/* =========================================
 * STATE
 *
 * bitmap           : bit 1 = frame terpakai / tidak tersedia
 * allocated_bitmap : bit 1 = frame berasal dari pmm_alloc()
 *
 * Dua bitmap ini yang membuat pmm_free() bisa menolak frame
 * kernel, memori reserved, dan double-free.
 * ========================================= */

#define PMM_BITMAP_BYTES (PMM_MAX_FRAMES / 8U)

static uint8_t  bitmap[PMM_BITMAP_BYTES];
static uint8_t  allocated_bitmap[PMM_BITMAP_BYTES];
static uint64_t free_frames;
static uint64_t pmm_cursor = PMM_FIRST_FRAME;
static uint64_t hhdm_base;
static uint64_t ignored_bytes;

/* =========================================
 * BITMAP HELPERS
 * ========================================= */

static int frame_used(uint64_t f)
{
    return (bitmap[f >> 3] & (1U << (f & 7))) != 0;
}

static int frame_allocated(uint64_t f)
{
    return (allocated_bitmap[f >> 3] & (1U << (f & 7))) != 0;
}

static void set_used(uint64_t f)
{
    bitmap[f >> 3] |= (uint8_t)(1U << (f & 7));
}

static void set_free(uint64_t f)
{
    bitmap[f >> 3] &= (uint8_t)~(1U << (f & 7));
}

static void set_allocated(uint64_t f)
{
    allocated_bitmap[f >> 3] |= (uint8_t)(1U << (f & 7));
}

static void clear_allocated(uint64_t f)
{
    allocated_bitmap[f >> 3] &= (uint8_t)~(1U << (f & 7));
}

static uint64_t align_up(uint64_t n)
{
    return (n + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
}

static uint64_t align_down(uint64_t n)
{
    return n & ~(PMM_PAGE_SIZE - 1);
}

/* =========================================
 * RANGE OPERATIONS
 *
 * Kedua fungsi dijamin tidak overflow: panjang dipotong
 * ke PMM_MAX_PHYS (yang rata 4 KiB) sebelum dihitung.
 * ========================================= */

/* Tandai [base, base+len) sebagai bebas. Hanya frame PENUH. */
static void expose_available(uint64_t base, uint64_t len)
{
    if (!len)
        return;

    if (base >= PMM_MAX_PHYS) {
        ignored_bytes += len;
        return;
    }

    uint64_t remaining = PMM_MAX_PHYS - base;

    if (len > remaining) {
        ignored_bytes += len - remaining;
        len = remaining;
    }

    uint64_t start = align_up(base);
    uint64_t stop  = align_down(base + len);

    if (start >= stop)
        return;

    for (uint64_t f = start >> PMM_PAGE_SHIFT;
         f < (stop >> PMM_PAGE_SHIFT); f++) {
        if (frame_used(f)) {
            set_free(f);
            free_frames++;
        }
    }
}

/* Tandai [base, base+len) sebagai terpakai. Membulat KE LUAR. */
static void reserve_range(uint64_t base, uint64_t len)
{
    if (!len || base >= PMM_MAX_PHYS)
        return;

    uint64_t remaining = PMM_MAX_PHYS - base;

    if (len > remaining)
        len = remaining;

    uint64_t start = align_down(base);
    uint64_t stop  = align_up(base + len);

    for (uint64_t f = start >> PMM_PAGE_SHIFT;
         f < (stop >> PMM_PAGE_SHIFT); f++) {
        if (!frame_used(f)) {
            set_used(f);

            if (free_frames)
                free_frames--;
        }
    }
}

/* =========================================
 * PUBLIC API
 * ========================================= */

int pmm_init(const struct limine_memmap_response *memmap,
             uint64_t hhdm_offset)
{
    for (uint64_t i = 0; i < PMM_BITMAP_BYTES; i++) {
        bitmap[i] = 0xFF;
        allocated_bitmap[i] = 0;
    }

    free_frames   = 0;
    pmm_cursor    = PMM_FIRST_FRAME;
    ignored_bytes = 0;
    hhdm_base     = hhdm_offset;

    if (!memmap || !memmap->entries || memmap->entry_count == 0)
        return 0;

    /*
     * Hanya LIMINE_MEMMAP_USABLE yang dibagikan.
     *
     * Tidak perlu reserve_range() khusus untuk kernel, memmap,
     * atau struktur boot seperti di Heinx (Multiboot): Limine
     * menaruhnya di EXECUTABLE_AND_MODULES / BOOTLOADER_RECLAIMABLE,
     * yang bukan USABLE, sehingga tidak pernah ikut di-expose.
     */
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        const struct limine_memmap_entry *e = memmap->entries[i];

        if (e && e->type == LIMINE_MEMMAP_USABLE)
            expose_available(e->base, e->length);
    }

    /* 1 MiB pertama (legacy/BIOS/DMA) tetap dilarang, seperti Heinx. */
    reserve_range(0, 0x100000);

    return 1;
}

uint64_t pmm_alloc(void)
{
    if (!free_frames)
        return 0;

    for (uint64_t scanned = 0;
         scanned < PMM_MAX_FRAMES - PMM_FIRST_FRAME; scanned++) {
        uint64_t f = pmm_cursor++;

        if (pmm_cursor >= PMM_MAX_FRAMES)
            pmm_cursor = PMM_FIRST_FRAME;

        if (!frame_used(f)) {
            set_used(f);
            set_allocated(f);
            free_frames--;

            return f << PMM_PAGE_SHIFT;
        }
    }

    return 0;
}

int pmm_free(uint64_t addr)
{
    if (!addr || (addr & (PMM_PAGE_SIZE - 1)))
        return 0;

    uint64_t f = addr >> PMM_PAGE_SHIFT;

    if (f >= PMM_MAX_FRAMES || !frame_allocated(f) || !frame_used(f))
        return 0;

    clear_allocated(f);
    set_free(f);
    free_frames++;

    if (f < pmm_cursor)
        pmm_cursor = f;

    return 1;
}

uint64_t pmm_free_frames(void)
{
    return free_frames;
}

uint64_t pmm_ignored_bytes(void)
{
    return ignored_bytes;
}

void *pmm_phys_to_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + hhdm_base);
}
