#ifndef HISPERIA_INTERRUPT_H
#define HISPERIA_INTERRUPT_H

#include <stdint.h>
#include <stddef.h>

/* =========================================
 * INTERRUPT FRAME
 *
 * Layout dari alamat RENDAH ke TINGGI, persis seperti
 * yang terlihat di stack setelah isr_common selesai
 * menyimpan register (RSP menunjuk ke r15).
 *
 *   [ disimpan software: isr_common ]
 *     r15 ... rax          15 qword
 *     vector                1 qword  (di-push stub ISR)
 *     error_code            1 qword  (asli dari CPU, atau 0 palsu)
 *   [ di-push hardware saat interrupt ]
 *     rip, cs, rflags, rsp, ss   5 qword
 *
 * Di long mode CPU SELALU mem-push SS dan RSP, baik ada
 * pergantian privilege maupun tidak. Jadi layout hardware
 * frame identik untuk stack normal dan stack IST. Yang
 * berbeda hanya DI MANA frame itu berada.
 * ========================================= */

struct interrupt_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    uint64_t vector;
    uint64_t error_code;

    /* Hardware frame */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;

    /*
     * Hanya valid jika CPU melakukan stack switch:
     * - perubahan privilege level
     * - penggunaan IST
     */
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

/*
 * Offset ini dipakai langsung oleh isr.S. Bila struct diubah
 * tanpa memperbarui assembly, build akan gagal di sini.
 */
_Static_assert(offsetof(struct interrupt_frame, r15)        ==   0, "frame: r15");
_Static_assert(offsetof(struct interrupt_frame, rax)        == 112, "frame: rax");
_Static_assert(offsetof(struct interrupt_frame, vector)     == 120, "frame: vector");
_Static_assert(offsetof(struct interrupt_frame, error_code) == 128, "frame: error_code");
_Static_assert(offsetof(struct interrupt_frame, rip)        == 136, "frame: rip");
_Static_assert(offsetof(struct interrupt_frame, cs)         == 144, "frame: cs");
_Static_assert(offsetof(struct interrupt_frame, rflags)     == 152, "frame: rflags");
_Static_assert(offsetof(struct interrupt_frame, rsp)        == 160, "frame: rsp");
_Static_assert(offsetof(struct interrupt_frame, ss)         == 168, "frame: ss");
_Static_assert(sizeof(struct interrupt_frame)               == 176, "frame: size");

/* =========================================
 * EXCEPTION ERROR CODE
 *
 * Vector yang dipush error code oleh CPU:
 * 8 (#DF), 10 (#TS), 11 (#NP), 12 (#SS), 13 (#GP),
 * 14 (#PF), 17 (#AC), 21 (#CP), 29 (#VC), 30 (#SX).
 *
 * Harus sinkron dengan makro ISR_ERR di isr.S.
 * ========================================= */

#define EXCEPTION_ERRCODE_MASK  0x60227D00UL

static inline int exception_has_error_code(uint64_t vector)
{
    return vector < 32 && ((EXCEPTION_ERRCODE_MASK >> vector) & 1UL);
}

/* =========================================
 * ISR API
 * ========================================= */

/* Dipanggil dari isr_common. Tidak return untuk exception fatal. */
void isr_dispatch(struct interrupt_frame *frame);

/* Diimplementasikan di main.c (console masih static di sana). */
void isr_report(
    const struct interrupt_frame *frame,
    int stack_switched
);
void isr_ist_report(int passed);

#endif