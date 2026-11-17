#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include "gdt.h"
#include "idt.h"
#include "interrupt.h"
#include "pmm.h"
#include "vmm.h"
#include "stack.h"
#include "heap.h"

#ifndef HISPERIA_RUN_EXCEPTION_TEST
#define HISPERIA_RUN_EXCEPTION_TEST 0
#endif
#ifndef HISPERIA_RUN_MPROTECT_TEST
#define HISPERIA_RUN_MPROTECT_TEST 0
#endif

/* Linker-defined page-aligned kernel section boundaries. */
extern char __kernel_requests_start[];
extern char __kernel_requests_end[];
extern char __kernel_text_start[];
extern char __kernel_text_end[];
extern char __kernel_rodata_start[];
extern char __kernel_rodata_end[];
extern char __kernel_data_start[];
extern char __kernel_bss_end[];

/* Dedicated probes used to verify section permissions. */
__attribute__((used, aligned(4096)))
const volatile uint64_t vmm_rodata_probe = 0x524F444154415052ULL;

__attribute__((used, aligned(4096)))
volatile uint64_t vmm_data_probe = 0x4441544150524F42ULL;

__attribute__((used, aligned(4096)))
static volatile uint64_t vmm_bss_probe;

/* =========================================
 * LIMINE COMPATIBILITY
 * ========================================= */

#if defined(LIMINE_FRAMEBUFFER_REQUEST_ID)
    #define HISPERIA_FB_REQUEST_ID LIMINE_FRAMEBUFFER_REQUEST_ID
#elif defined(LIMINE_FRAMEBUFFER_REQUEST)
    #define HISPERIA_FB_REQUEST_ID LIMINE_FRAMEBUFFER_REQUEST
#else
    #error "Limine framebuffer request ID not found"
#endif

#if defined(LIMINE_FRAMEBUFFER_RGB)
    #define HISPERIA_FB_RGB LIMINE_FRAMEBUFFER_RGB
#elif defined(LIMINE_FRAMEBUFFER_MEMORY_MODEL_RGB)
    #define HISPERIA_FB_RGB LIMINE_FRAMEBUFFER_MEMORY_MODEL_RGB
#else
    #error "Limine framebuffer RGB constant not found"
#endif

/* =========================================
 * LIMINE REQUESTS
 * ========================================= */

__attribute__((used, section(".limine_requests")))
static volatile uint64_t base_revision[] =
    LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t request_start[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t request_end[] =
    LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = HISPERIA_FB_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

/* =========================================
 * SERIAL COM1
 * ========================================= */

#define COM1       0x3F8
#define COM1_LSR   (COM1 + 5)

static int serial_available = 0;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;

    __asm__ volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);

    serial_available = 1;
}

static void serial_putchar(char c) {
    if (!serial_available)
        return;

    for (uint32_t timeout = 0; timeout < 1000000; timeout++) {
        uint8_t status = inb(COM1_LSR);

        if (status == 0xFF)
            return;

        if (status & 0x20) {
            outb(COM1, (uint8_t)c);
            return;
        }

        __asm__ volatile ("pause");
    }
}

/* =========================================
 * FRAMEBUFFER
 * ========================================= */

#define COLOR_BLACK 0x00000000u
#define COLOR_WHITE 0x00FFFFFFu

#define FONT_WIDTH   5
#define FONT_HEIGHT  7

#define CHAR_WIDTH   12
#define CHAR_HEIGHT  16

static volatile uint32_t *fb = NULL;

static uint64_t fb_width = 0;
static uint64_t fb_height = 0;
static uint64_t fb_pitch = 0;

static uint64_t cursor_x = 8;
static uint64_t cursor_y = 8;

static int fb_ready = 0;

static void framebuffer_init(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision))
        return;

    if (!framebuffer_request.response)
        return;

    struct limine_framebuffer_response *response =
        framebuffer_request.response;

    if (response->framebuffer_count == 0 ||
        !response->framebuffers)
        return;

    struct limine_framebuffer *framebuffer =
        response->framebuffers[0];

    if (!framebuffer || !framebuffer->address)
        return;

    if (framebuffer->bpp != 32 ||
        framebuffer->memory_model != HISPERIA_FB_RGB)
        return;

    if (framebuffer->width < CHAR_WIDTH ||
        framebuffer->height < CHAR_HEIGHT)
        return;

    if (framebuffer->pitch % 4 != 0 ||
        framebuffer->pitch < framebuffer->width * 4)
        return;

    fb = (volatile uint32_t *)framebuffer->address;

    fb_width = framebuffer->width;
    fb_height = framebuffer->height;
    fb_pitch = framebuffer->pitch / 4;

    fb_ready = 1;
}

/* =========================================
 * GRAPHICS PRIMITIVES
 * ========================================= */

static void put_pixel(
    uint64_t x,
    uint64_t y,
    uint32_t color
) {
    if (!fb_ready)
        return;

    if (x >= fb_width || y >= fb_height)
        return;

    fb[y * fb_pitch + x] = color;
}

static void clear_screen(uint32_t color) {
    if (!fb_ready)
        return;

    for (uint64_t y = 0; y < fb_height; y++) {
        for (uint64_t x = 0; x < fb_width; x++) {
            put_pixel(x, y, color);
        }
    }
}

/* =========================================
 * BITMAP FONT 5x7
 * ========================================= */

static const uint8_t font[36][7] = {
    {14,17,17,31,17,17,17}, // A
    {30,17,17,30,17,17,30}, // B
    {14,17,16,16,16,17,14}, // C
    {30,17,17,17,17,17,30}, // D
    {31,16,16,30,16,16,31}, // E
    {31,16,16,30,16,16,16}, // F
    {14,17,16,23,17,17,15}, // G
    {17,17,17,31,17,17,17}, // H
    {14,4,4,4,4,4,14},      // I
    {7,2,2,2,18,18,12},     // J
    {17,18,20,24,20,18,17}, // K
    {16,16,16,16,16,16,31}, // L
    {17,27,21,21,17,17,17}, // M
    {17,25,21,19,17,17,17}, // N
    {14,17,17,17,17,17,14}, // O
    {30,17,17,30,16,16,16}, // P
    {14,17,17,17,21,18,13}, // Q
    {30,17,17,30,20,18,17}, // R
    {15,16,16,14,1,1,30},   // S
    {31,4,4,4,4,4,4},       // T
    {17,17,17,17,17,17,14}, // U
    {17,17,17,17,17,10,4},  // V
    {17,17,17,21,21,21,10}, // W
    {17,17,10,4,10,17,17},  // X
    {17,17,10,4,4,4,4},     // Y
    {31,1,2,4,8,16,31},     // Z

    {14,17,19,21,25,17,14}, // 0
    {4,12,4,4,4,4,14},      // 1
    {14,17,1,2,4,8,31},     // 2
    {30,1,1,14,1,1,30},     // 3
    {2,6,10,18,31,2,2},     // 4
    {31,16,16,30,1,1,30},   // 5
    {14,16,16,30,17,17,14}, // 6
    {31,1,2,4,8,8,8},       // 7
    {14,17,17,14,17,17,14}, // 8
    {14,17,17,15,1,1,14}    // 9
};

static uint8_t glyph_row(char c, uint32_t row) {
    if (row >= FONT_HEIGHT)
        return 0;

    if (c >= 'a' && c <= 'z')
        c -= 32;

    if (c >= 'A' && c <= 'Z')
        return font[c - 'A'][row];

    if (c >= '0' && c <= '9')
        return font[26 + c - '0'][row];

    switch (c) {
        case '!':
            return row < 5 || row == 6 ? 4 : 0;

        case '.':
            return row == 6 ? 4 : 0;

        case '-':
            return row == 3 ? 14 : 0;

        case ':':
            return (row == 2 || row == 5) ? 4 : 0;

        case '=':
            return (row == 2 || row == 4) ? 14 : 0;

        case '_':
            return row == 6 ? 31 : 0;

        case '[':
            return (row == 0 || row == 6) ? 14 : 8;

        case ']':
            return (row == 0 || row == 6) ? 14 : 2;

        case '/':
            return (uint8_t)(1u << (row > 4 ? 0 : 4 - row));

        case '+':
            return row == 3 ? 31 :
                   (row >= 1 && row <= 5 ? 4 : 0);

        default:
            return 0;
    }
}

/* =========================================
 * TEXT RENDERER
 * ========================================= */

static void draw_char(char c, uint64_t x, uint64_t y) {
    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph_row(c, row);

        for (uint32_t col = 0; col < FONT_WIDTH; col++) {
            if (bits & (1u << (FONT_WIDTH - 1 - col))) {
                for (uint32_t dy = 0; dy < 2; dy++) {
                    for (uint32_t dx = 0; dx < 2; dx++) {
                        put_pixel(
                            x + col * 2 + dx,
                            y + row * 2 + dy,
                            COLOR_WHITE
                        );
                    }
                }
            }
        }
    }
}

/* =========================================
 * CONSOLE SCROLLING
 * ========================================= */

static void framebuffer_scroll(void) {
    if (!fb_ready)
        return;

    if (fb_height <= CHAR_HEIGHT) {
        clear_screen(COLOR_BLACK);
        cursor_y = 0;
        return;
    }

    for (uint64_t y = CHAR_HEIGHT; y < fb_height; y++) {
        for (uint64_t x = 0; x < fb_width; x++) {
            fb[(y - CHAR_HEIGHT) * fb_pitch + x] =
                fb[y * fb_pitch + x];
        }
    }

    for (uint64_t y = fb_height - CHAR_HEIGHT;
         y < fb_height; y++) {
        for (uint64_t x = 0; x < fb_width; x++) {
            fb[y * fb_pitch + x] = COLOR_BLACK;
        }
    }

    if (cursor_y >= CHAR_HEIGHT)
        cursor_y -= CHAR_HEIGHT;
    else
        cursor_y = 0;
}

static void screen_newline(void) {
    cursor_x = 8;
    cursor_y += CHAR_HEIGHT;

    if (cursor_y + CHAR_HEIGHT > fb_height)
        framebuffer_scroll();
}

static void screen_putchar(char c) {
    if (!fb_ready || c == '\r')
        return;

    if (c == '\n') {
        screen_newline();
        return;
    }

    if (cursor_x + CHAR_WIDTH > fb_width)
        screen_newline();

    draw_char(c, cursor_x, cursor_y);
    cursor_x += CHAR_WIDTH;
}

/* =========================================
 * UNIFIED CONSOLE
 * ========================================= */

static void console_putchar(char c) {
    if (c == '\n')
        serial_putchar('\r');

    serial_putchar(c);
    screen_putchar(c);
}

static void console_print(const char *str) {
    while (*str)
        console_putchar(*str++);
}

static void console_print_u64(uint64_t value) {
    char buffer[21];
    uint32_t index = 0;

    if (value == 0) {
        console_putchar('0');
        return;
    }

    while (value > 0 && index < sizeof(buffer)) {
        buffer[index++] = '0' + (value % 10);
        value /= 10;
    }

    while (index > 0)
        console_putchar(buffer[--index]);
}

static void console_print_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";

    console_print("0x");

    for (int shift = 60; shift >= 0; shift -= 4)
        console_putchar(digits[(value >> shift) & 0x0F]);
}

/* =========================================
 * ISR EXCEPTION REPORT
 * ========================================= */


static const char *exception_name(uint64_t vector)
{
    switch (vector) {
    case 0:  return "Divide Error";
    case 1:  return "Debug";
    case 2:  return "Non-Maskable Interrupt";
    case 3:  return "Breakpoint";
    case 4:  return "Overflow";
    case 5:  return "Bound Range Exceeded";
    case 6:  return "Invalid Opcode";
    case 7:  return "Device Not Available";
    case 8:  return "Double Fault";
    case 9:  return "Coprocessor Segment Overrun";
    case 10: return "Invalid TSS";
    case 11: return "Segment Not Present";
    case 12: return "Stack-Segment Fault";
    case 13: return "General Protection Fault";
    case 14: return "Page Fault";
    case 15: return "Reserved";
    case 16: return "x87 Floating-Point Exception";
    case 17: return "Alignment Check";
    case 18: return "Machine Check";
    case 19: return "SIMD Floating-Point Exception";
    case 20: return "Virtualization Exception";
    case 21: return "Control Protection Exception";
    case 22: return "Reserved";
    case 23: return "Reserved";
    case 24: return "Reserved";
    case 25: return "Reserved";
    case 26: return "Reserved";
    case 27: return "Reserved";
    case 28: return "Hypervisor Injection Exception";
    case 29: return "VMM Communication Exception";
    case 30: return "Security Exception";
    case 31: return "Reserved";
    default: return "Unknown Exception";
    }
}

static void isr_report_error_details(
    uint64_t vector,
    uint64_t error
) {
    if (vector == 13 || vector == 10 ||
        vector == 11 || vector == 12) {

        uint64_t index = (error >> 3) & 0x1FFF;
        uint64_t table = (error >> 2) & 1;
        uint64_t external = error & 1;

        console_print("\n[EXCP] Selector index : ");
        console_print_u64(index);

        console_print("\n[EXCP] Selector table : ");
        console_print(table ? "IDT" : "GDT/LDT");

        if (vector == 13) {
            console_print("\n[EXCP] External event : ");
            console_print(external ? "yes" : "no");
        }
    }

    if (vector == 14) {
        console_print("\n[PF  ] Present       : ");
        console_print((error & 1) ? "yes" : "no");

        console_print("\n[PF  ] Write         : ");
        console_print((error & 2) ? "yes" : "no");

        console_print("\n[PF  ] User          : ");
        console_print((error & 4) ? "yes" : "no");

        console_print("\n[PF  ] Reserved bit  : ");
        console_print((error & 8) ? "yes" : "no");

        console_print("\n[PF  ] Instruction   : ");
        console_print((error & 16) ? "yes" : "no");
    }
}


void isr_report(
    const struct interrupt_frame *frame,
    int stack_switched
) {
    console_print("\n");
    console_print("[EXCP] CPU Exception detected!\n");

    console_print("[EXCP] Name       : ");
    console_print(exception_name(frame->vector));

    if (exception_has_error_code(frame->vector)) {
        isr_report_error_details(
            frame->vector,
            frame->error_code
        );
    }

    if (frame->vector == 14) {
        uint64_t fault_address;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
        console_print("\n[PF  ] Fault address : ");
        console_print_hex(fault_address);
    }

    console_print("\n[EXCP] Vector     : ");
    console_print_u64(frame->vector);

    console_print("\n[EXCP] Error Code : ");

    if (exception_has_error_code(frame->vector))
        console_print_hex(frame->error_code);
    else
        console_print("none");

    console_print("\n[EXCP] RIP        : ");
    console_print_hex(frame->rip);

    console_print("\n[EXCP] CS         : ");
    console_print_hex(frame->cs);

    console_print("\n[EXCP] RFLAGS     : ");
    console_print_hex(frame->rflags);

    console_print("\n[EXCP] Stack switch: ");
    console_print(stack_switched ? "yes" : "no");

    if (stack_switched) {
        console_print("\n[EXCP] RSP (saved): ");
        console_print_hex(frame->rsp);

        console_print("\n[EXCP] SS         : ");
        console_print_hex(frame->ss);
    }

    console_print("\n[EXCP] Frame @    : ");
    console_print_hex((uint64_t)(uintptr_t)frame);

    console_print("\n");
}

void isr_ist_report(int on_ist1)
{
    console_print(on_ist1 ? "[IST ] Frame stack: IST1\n"
                          : "[IST ] Frame stack: normal\n");
}

/* =========================================
 * CPU HALT
 * ========================================= */

static void halt_cpu(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void isr_test_report(int passed)
{
    if (passed) {
#if HISPERIA_RUN_MPROTECT_TEST == 1
        console_print("[TEST] Fault enforcement PASS: .rodata write trapped by #PF.\n");
#elif HISPERIA_RUN_MPROTECT_TEST == 2
        console_print("[TEST] Fault enforcement PASS: NX execution trapped by #PF.\n");
#else
        console_print("[TEST] Expected protection fault observed.\n");
#endif
    } else {
        console_print("[TEST] Fault enforcement FAILED: unexpected #PF details.\n");
    }
}


/* =========================================
 * EXCEPTION TESTS
 * ========================================= */

#if HISPERIA_RUN_EXCEPTION_TEST
#define EXCEPTION_TEST 2   /* 0 = int3, 1 = ud2, 2 = #GP */

static void run_exception_test(int which)
{
    switch (which) {
    case 0: /* #BP: tanpa error code, IST1, TRAP */
        __asm__ volatile ("int3");
        break;
    case 1: /* #UD: tanpa error code, stack normal, FAULT */
        __asm__ volatile ("ud2");
        break;
    case 2: /* #GP: dengan error code, stack normal */
        __asm__ volatile ("mov $0x28, %%ax\n\t"
                          "mov %%ax, %%ds" ::: "ax");
        break;
    }
}
#endif /* HISPERIA_RUN_EXCEPTION_TEST */

/* =========================================
 * KERNEL MEMORY PROTECTION SELF-CHECK
 * ========================================= */

static int verify_protected_range(uint64_t start,
                                  uint64_t end,
                                  int kernel_writable,
                                  int kernel_executable,
                                  int hhdm_writable)
{
    if (start > end || end > UINT64_MAX - (VMM_PAGE_SIZE - 1ULL))
        return 0;
    if (start == end)
        return 1;

    uint64_t page = start & ~(VMM_PAGE_SIZE - 1ULL);
    uint64_t limit = (end + VMM_PAGE_SIZE - 1ULL) &
                     ~(VMM_PAGE_SIZE - 1ULL);

    for (; page < limit; page += VMM_PAGE_SIZE) {
        struct vmm_mapping_info kernel_info;
        struct vmm_mapping_info alias_info;

        if (!vmm_query_mapping(page, &kernel_info))
            return 0;

        if (kernel_info.writable != (uint8_t)kernel_writable ||
            kernel_info.executable != (uint8_t)kernel_executable ||
            kernel_info.user)
            return 0;

        uint64_t physical_page = kernel_info.physical_address &
                                 ~(VMM_PAGE_SIZE - 1ULL);
        uintptr_t alias = (uintptr_t)pmm_phys_to_virt(physical_page);
        if (!alias ||
            !vmm_query_mapping((uint64_t)alias, &alias_info))
            return 0;

        if (alias_info.writable != (uint8_t)hhdm_writable ||
            alias_info.executable || alias_info.user)
            return 0;
    }

    return 1;
}

static int verify_kernel_section_permissions(void)
{
    return
        verify_protected_range(
            (uint64_t)(uintptr_t)__kernel_requests_start,
            (uint64_t)(uintptr_t)__kernel_requests_end,
            0, 0, 0) &&
        verify_protected_range(
            (uint64_t)(uintptr_t)__kernel_text_start,
            (uint64_t)(uintptr_t)__kernel_text_end,
            0, 1, 0) &&
        verify_protected_range(
            (uint64_t)(uintptr_t)__kernel_rodata_start,
            (uint64_t)(uintptr_t)__kernel_rodata_end,
            0, 0, 0) &&
        verify_protected_range(
            (uint64_t)(uintptr_t)__kernel_data_start,
            (uint64_t)(uintptr_t)__kernel_bss_end,
            1, 0, 1);
}


/* ========================================================================== 
 * ADDRESS-SPACE SELF-TEST
 *
 * Creates two independent lower-half user spaces, maps the same VA to
 * different physical frames, switches CR3 between them, then restores the
 * Limine/kernel root. This validates paging isolation primitives, not ring-3
 * execution, syscalls, scheduling, or a complete process implementation.
 * ========================================================================== */
static int address_space_self_test(void)
{
    struct vmm_address_space first = {0};
    struct vmm_address_space second = {0};
    const uint64_t test_va = 0x0000000000400000ULL;
    const uint64_t pattern_a = 0x4853505249414153ULL;
    const uint64_t pattern_b = 0x4853505249414253ULL;
    uint64_t phys_a = 0;
    uint64_t phys_b = 0;
    int ok = 0;

    if (!vmm_address_space_create(&first) ||
        !vmm_address_space_create(&second))
        goto cleanup;

    if (!vmm_address_space_map_user_page(
            &first, test_va, VMM_PROT_USER | VMM_PROT_WRITE, &phys_a) ||
        !vmm_address_space_map_user_page(
            &second, test_va, VMM_PROT_USER | VMM_PROT_WRITE, &phys_b))
        goto cleanup;

    if (!phys_a || !phys_b || phys_a == phys_b)
        goto cleanup;

    struct vmm_mapping_info map_a;
    struct vmm_mapping_info map_b;
    if (!vmm_address_space_query(&first, test_va, &map_a) ||
        !vmm_address_space_query(&second, test_va, &map_b) ||
        !map_a.user || !map_b.user || !map_a.writable || !map_b.writable ||
        map_a.executable || map_b.executable ||
        (map_a.physical_address & ~(VMM_PAGE_SIZE - 1ULL)) != phys_a ||
        (map_b.physical_address & ~(VMM_PAGE_SIZE - 1ULL)) != phys_b)
        goto cleanup;

    volatile uint64_t *page_a =
        (volatile uint64_t *)pmm_phys_to_virt(phys_a);
    volatile uint64_t *page_b =
        (volatile uint64_t *)pmm_phys_to_virt(phys_b);
    if (!page_a || !page_b)
        goto cleanup;
    page_a[0] = pattern_a;
    page_b[0] = pattern_b;
    if (page_a[0] != pattern_a || page_b[0] != pattern_b)
        goto cleanup;

    uint64_t high_entry = 0;
    if (!vmm_read_table_entry(first.root_phys, 256, &high_entry) ||
        (high_entry & (1ULL << 2)))
        goto cleanup;
    if (!vmm_read_table_entry(first.root_phys, 511, &high_entry) ||
        (high_entry & (1ULL << 2)))
        goto cleanup;

    if (!vmm_address_space_activate(&first) ||
        vmm_root_phys() != first.root_phys)
        goto cleanup;
    struct vmm_mapping_info active_info;
    if (!vmm_query_mapping(test_va, &active_info) || !active_info.user ||
        (active_info.physical_address & ~(VMM_PAGE_SIZE - 1ULL)) != phys_a)
        goto cleanup;

    if (!vmm_address_space_activate(&second) ||
        vmm_root_phys() != second.root_phys ||
        !vmm_query_mapping(test_va, &active_info) || !active_info.user ||
        (active_info.physical_address & ~(VMM_PAGE_SIZE - 1ULL)) != phys_b)
        goto cleanup;

    ok = 1;

cleanup:
    if (!vmm_activate_kernel_address_space())
        return 0;
    if (first.valid && !vmm_address_space_destroy(&first))
        ok = 0;
    if (second.valid && !vmm_address_space_destroy(&second))
        ok = 0;
    return ok;
}

/* =========================================
 * KERNEL ENTRY
 * ========================================= */

void kmain(void) {
    /* Start serial first so GDT diagnostics are visible even before graphics. */
    serial_init();

    console_print("[GDT ] Installing GDT...\n");
    gdt_init();
    console_print("[GDT ] gdt_load() returned.\n");

    uint16_t gdt_limit = 0;
    uint64_t gdt_base = 0;
    uint16_t tr = 0;

    if (gdt_verify(&gdt_limit, &gdt_base)) {
        console_print("[GDT ] GDTR base : ");
        console_print_hex(gdt_base);
        console_print("\n[GDT ] GDTR limit: ");
        console_print_hex(gdt_limit);
        console_print("\n[GDT ] Verification PASS.\n");
    } else {
        console_print("[GDT ] GDTR verification FAILED.\n");
        console_print("[GDT ] GDTR base : ");
        console_print_hex(gdt_base);
        console_print("\n[GDT ] GDTR limit: ");
        console_print_hex(gdt_limit);
        console_print("\n");
    }

    if (gdt_tss_verify(&tr)) {
        console_print("[TSS ] Task Register: ");

        console_print_hex(tr);

        console_print("\n[TSS ] Verification PASS.\n");
    } else {
        console_print("[TSS ] Verification FAILED.\n");
    }

    /* =========================================
     * IST INITIALIZATION
     * ========================================= */

    console_print("[IST ] Initializing IST1...\n");

    gdt_ist_init();

    uint64_t ist1_top = 0;

    if (gdt_ist1_verify(&ist1_top)) {
        console_print("[IST ] IST1 Stack Top: ");
        console_print_hex(ist1_top);
        console_print("\n[IST ] Verification PASS.\n");
    } else {
        console_print("[IST ] Verification FAILED!\n");
        halt_cpu();
    }

    console_print("[IDT ] Installing IDT...\n");

    idt_init();

    uint16_t idt_limit = 0;
    uint64_t idt_base = 0;
    uint8_t df_ist = 0;

    if (idt_verify(&idt_limit, &idt_base)) {
        console_print("[IDT ] IDTR base : ");
        console_print_hex(idt_base);

        console_print("\n[IDT ] IDTR limit: ");
        console_print_hex(idt_limit);

        console_print("\n[IDT ] Verification PASS.\n");
    } else {
        console_print("[IDT ] Verification FAILED.\n");
    }

    if (idt_df_ist_verify(&df_ist)) {
        console_print("[IDT ] Double Fault IST: ");
        console_print_u64(df_ist);
        console_print("\n");
        console_print("[IDT ] IST Verification PASS.\n");
    } else {
        console_print("[IDT ] IST Verification FAILED!\n");
        halt_cpu();
    }

    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision)) {
        console_print("[ERROR] Limine revision unsupported.\n");
        halt_cpu();
    }

    framebuffer_init();

    if (fb_ready)
        clear_screen(COLOR_BLACK);

    console_print("\n");
    console_print("================================\n");
    console_print("       H I S P E R I A O S      \n");
    console_print("================================\n");

    console_print("[BOOT] Limine handoff successful.\n");
    console_print("[CPU ] x86_64 kernel running.\n");

    if (fb_ready) {
        console_print("[GFX ] Framebuffer initialized.\n");

        console_print("[GFX ] Resolution: ");
        console_print_u64(fb_width);
        console_putchar('x');
        console_print_u64(fb_height);
        console_print("\n");
    } else {
        console_print("[GFX ] Framebuffer unavailable.\n");
    }

    if (!memmap_request.response || !hhdm_request.response ||
        !pmm_init(memmap_request.response, hhdm_request.response->offset)) {
        console_print("[PMM ] Initialization FAILED.\n");
        halt_cpu();
    }

    console_print("[PMM ] Free frames: ");
    console_print_u64(pmm_free_frames());
    console_print("\n");

    console_print("[PMM ] Running allocator self-test...\n");
    if (pmm_self_test()) {
        console_print("[PMM ] Allocation/HHDM/free test PASS.\n");
    } else {
        console_print("[PMM ] Allocation/HHDM/free test FAILED!\n");
        halt_cpu();
    }

    console_print("[PMM ] Free frames after test: ");
    console_print_u64(pmm_free_frames());
    console_print("\n");

    /* =========================================
     * VMM BOOTSTRAP — INSPECT, DO NOT REMAP
     * ========================================= */

    console_print("[VMM ] Inspecting active paging context...\n");

    if (!vmm_bootstrap_init()) {
        console_print("[VMM ] Active paging context FAILED.\n");
        halt_cpu();
    }

    console_print("[VMM ] CR3 value       : ");
    console_print_hex(vmm_cr3_value());
    console_print("\n[VMM ] Root table phys : ");
    console_print_hex(vmm_root_phys());
    console_print("\n[VMM ] Paging levels   : ");
    console_print_u64(vmm_paging_levels());
    console_print("\n[VMM ] Bootstrap inspection PASS.\n");

    /* Read-only PML4 inspection and entry decoding. */
    uint64_t root_entries = 0;
    if (!vmm_inspect_root(&root_entries)) {
        console_print("[VMM ] Root inspection FAILED!\n");
        halt_cpu();
    }

    console_print("[VMM ] Present root entries: ");
    console_print_u64(root_entries);
    console_print("/512\n");
    console_print("[VMM ] Decoding present root entries...\n");

    for (uint16_t i = 0; i < 512; i++) {
        uint64_t entry = 0;
        if (!vmm_read_root_entry(i, &entry)) {
            console_print("[VMM ] Root entry read FAILED!\n");
            halt_cpu();
        }

        if (!(entry & 1ULL))
            continue;

        console_print("[VMM ] Entry Index : ");
        console_print_u64(i);
        console_print("\n[VMM ] Raw Entry   : ");
        console_print_hex(entry);
        console_print("\n[VMM ] Child Phys  : ");
        console_print_hex(entry & 0x000FFFFFFFFFF000ULL);
        console_print("\n[VMM ] Present     : ");
        console_print_u64((entry >> 0) & 1ULL);
        console_print("\n[VMM ] Writable    : ");
        console_print_u64((entry >> 1) & 1ULL);
        console_print("\n[VMM ] User        : ");
        console_print_u64((entry >> 2) & 1ULL);
        console_print("\n[VMM ] NX          : ");
        console_print_u64((entry >> 63) & 1ULL);
        console_print("\n");
    }

    console_print("[VMM ] Root entry decoding PASS.\n");

    /* =========================================
     * KERNEL MEMORY PROTECTION
     * ========================================= */

    console_print("[MPRO] Enabling CR0.WP and EFER.NXE...\n");
    if (!vmm_enable_protection()) {
        console_print("[MPRO] CPU protection setup FAILED!\n");
        halt_cpu();
    }

    console_print("[MPRO] CR0.WP: enabled\n");
    console_print("[MPRO] EFER.NXE: ");
    console_print(vmm_nx_enabled() ? "enabled\n" : "disabled\n");

    console_print("[MPRO] Protecting kernel sections...\n");

    if (!vmm_protect_range_and_hhdm_alias(
            (uint64_t)(uintptr_t)__kernel_requests_start,
            (uint64_t)(uintptr_t)__kernel_requests_end,
            0, 0) ||
        !vmm_protect_range_and_hhdm_alias(
            (uint64_t)(uintptr_t)__kernel_text_start,
            (uint64_t)(uintptr_t)__kernel_text_end,
            VMM_PROT_EXEC, 0) ||
        !vmm_protect_range_and_hhdm_alias(
            (uint64_t)(uintptr_t)__kernel_rodata_start,
            (uint64_t)(uintptr_t)__kernel_rodata_end,
            0, 0) ||
        !vmm_protect_range_and_hhdm_alias(
            (uint64_t)(uintptr_t)__kernel_data_start,
            (uint64_t)(uintptr_t)__kernel_bss_end,
            VMM_PROT_WRITE, VMM_PROT_WRITE)) {
        console_print("[MPRO] Kernel section protection FAILED!\n");
        halt_cpu();
    }

    if (!verify_kernel_section_permissions()) {
        console_print("[MPRO] Permission verification FAILED!\n");
        halt_cpu();
    }

    console_print("[MPRO] .text   : R-X / supervisor\n");
    console_print("[MPRO] .rodata : R-- / supervisor\n");
    console_print("[MPRO] .data   : RW- / supervisor\n");
    console_print("[MPRO] .bss    : RW- / supervisor\n");
    console_print("[MPRO] W^X verification PASS.\n");

#if HISPERIA_RUN_MPROTECT_TEST == 1
    console_print("[TEST] Writing to protected .rodata; #PF is expected.\n");
    *(volatile uint64_t *)(uintptr_t)&vmm_rodata_probe = 0xBAD0BAD0BAD0BAD0ULL;
    console_print("[TEST] ERROR: read-only write did not fault.\n");
    halt_cpu();
#elif HISPERIA_RUN_MPROTECT_TEST == 2
    console_print("[TEST] Executing protected .data; #PF is expected.\n");
    __asm__ volatile (
        "call *%0"
        :: "r"((uintptr_t)&vmm_data_probe)
        : "memory"
    );
    console_print("[TEST] ERROR: NX execution did not fault.\n");
    halt_cpu();
#endif

    /* Guarded IST stack: low guard page remains deliberately unmapped. */
    uint64_t ist_guard = 0, ist_bottom = 0, ist_top = 0;
    console_print("[STK ] Allocating guarded IST1 stack...\n");
    if (!kernel_guarded_ist_stack_init(&ist_guard, &ist_bottom, &ist_top)) {
        console_print("[STK ] Guarded stack initialization FAILED!\n");
        halt_cpu();
    }
    struct vmm_mapping_info stack_map;
    if (vmm_query_mapping(ist_guard, &stack_map) ||
        !vmm_query_mapping(ist_bottom, &stack_map) ||
        stack_map.writable != 1 || stack_map.executable != 0 ||
        !gdt_ist1_verify(&ist1_top) || ist1_top != ist_top) {
        console_print("[STK ] Guard-page/IST verification FAILED!\n");
        halt_cpu();
    }
    console_print("[STK ] Guard page unmapped; IST1 stack mapped RW/NX. PASS.\n");

    /* Page-backed kernel heap with split/coalesce and free validation. */
    console_print("[HEAP] Initializing kernel heap...\n");
    if (!kheap_init()) {
        console_print("[HEAP] Initialization FAILED!\n");
        halt_cpu();
    }
    console_print("[HEAP] Running allocator self-test...\n");
    if (!kheap_self_test()) {
        console_print("[HEAP] Alloc/free self-test FAILED!\n");
        halt_cpu();
    }
    console_print("[HEAP] Alloc/free/split/coalesce test PASS.\n");
    console_print("[HEAP] Mapped bytes: ");
    console_print_u64(kheap_bytes_mapped());
    console_print("\n");

    /* Two isolated user address spaces; no ring-3 entry is attempted yet. */
    console_print("[AS  ] Running address-space isolation self-test...\n");
    if (!address_space_self_test()) {
        console_print("[AS  ] Address-space isolation test FAILED!\n");
        halt_cpu();
    }
    console_print("[AS  ] Separate user mappings + CR3 switch + kernel-half isolation PASS.\n");

    console_print("[INFO] Kernel initialization complete.\n");
    console_print("\n");
    console_print("Welcome to HisperiaOS!\n");

#if HISPERIA_RUN_EXCEPTION_TEST
    console_print("[TEST] Triggering exception test...\n");
    run_exception_test(EXCEPTION_TEST);
    console_print("[ERROR] Test unexpectedly returned!\n");
#endif

    halt_cpu();
}