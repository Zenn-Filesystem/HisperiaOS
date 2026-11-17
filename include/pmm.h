#ifndef HISPERIA_PMM_H
#define HISPERIA_PMM_H

#include <stdint.h>
#include <stddef.h>

struct limine_memmap_response;

/* =========================================
 * PHYSICAL MEMORY MANAGER (bitmap, 4 KiB frame)
 *
 * Port dari PMM Heinx (core.c), diadaptasi ke:
 *   - x86_64 + Limine (memmap + HHDM), bukan Multiboot 32-bit
 *   - alamat fisik 64-bit (uint64_t)
 * ========================================= */

#define PMM_PAGE_SIZE    4096ULL
#define PMM_PAGE_SHIFT   12

/* Frame 0..255 (1 MiB pertama) tidak pernah dibagikan. */
#define PMM_FIRST_FRAME  256ULL

/*
 * Batas memori fisik yang dilacak. Bitmap bersifat statis di .bss
 * (2 x PMM_MAX_FRAMES/8 byte). Default 16 GiB = 1 MiB .bss.
 * RAM di atas batas ini diabaikan (lihat pmm_ignored_bytes()).
 */
#define PMM_MAX_FRAMES   (1ULL << 22)
#define PMM_MAX_PHYS     (PMM_MAX_FRAMES * PMM_PAGE_SIZE)

/*
 * Inisialisasi dari memory map Limine. hhdm_offset adalah
 * limine_hhdm_response->offset.
 * Return 1 jika sukses, 0 jika memmap tidak valid.
 */
int pmm_init(const struct limine_memmap_response *memmap,
             uint64_t hhdm_offset);

/* Alokasi satu frame. Return alamat fisik (rata 4 KiB), 0 jika habis. */
uint64_t pmm_alloc(void);

/*
 * Bebaskan frame hasil pmm_alloc(). Return 1 jika sukses.
 * Return 0 (tanpa efek) untuk: alamat 0, tidak rata, di luar
 * jangkauan, frame yang bukan hasil pmm_alloc (mis. kernel),
 * dan double-free.
 */
int pmm_free(uint64_t addr);

/* Jumlah frame bebas saat ini. */
uint64_t pmm_free_frames(void);

/* Byte RAM USABLE di atas PMM_MAX_PHYS yang tidak dilacak. */
uint64_t pmm_ignored_bytes(void);

/* Alamat fisik -> alamat virtual via HHDM. */
void *pmm_phys_to_virt(uint64_t phys);

#endif
