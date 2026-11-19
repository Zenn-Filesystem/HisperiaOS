#include <stdint.h>
#include <stddef.h>

#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "ring3.h"

#define USER_CODE_BASE       0x0000000000400000ULL
#define USER_STACK_BASE      0x00000000007FF000ULL
#define USER_STACK_TOP       (USER_STACK_BASE + VMM_PAGE_SIZE)
#define USER_TOP             0x0000800000000000ULL
#define RING3_KERNEL_STACK_SIZE (16ULL * 1024ULL)
#define USER_WRITE_MAX       256ULL

#define SYS_WRITE 1ULL
#define SYS_EXIT  2ULL

struct ring3_syscall_frame {
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

    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

_Static_assert(offsetof(struct ring3_syscall_frame, r15) == 0,
               "ring3 frame r15 offset");
_Static_assert(offsetof(struct ring3_syscall_frame, rax) == 112,
               "ring3 frame rax offset");
_Static_assert(offsetof(struct ring3_syscall_frame, rip) == 120,
               "ring3 frame rip offset");
_Static_assert(offsetof(struct ring3_syscall_frame, cs) == 128,
               "ring3 frame cs offset");
_Static_assert(offsetof(struct ring3_syscall_frame, rsp) == 144,
               "ring3 frame rsp offset");
_Static_assert(sizeof(struct ring3_syscall_frame) == 160,
               "ring3 frame size");

extern uint8_t user_blob_start[];
extern uint8_t user_blob_end[];
extern void ring3_enter(uint64_t user_rip, uint64_t user_rsp);
extern void ring3_exit_trampoline(void);
extern uint64_t ring3_saved_kernel_rsp;

extern void kernel_console_write(const char *data, size_t length);

static struct vmm_address_space current_space;
static uint8_t ring3_kernel_stack[RING3_KERNEL_STACK_SIZE]
    __attribute__((aligned(16)));

static volatile uint64_t exit_status;
static volatile int exit_seen;
static volatile int exit_cleanup_ok;

static void ring3_halt(void)
{
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

static int user_range_readable(uint64_t address, uint64_t length)
{
    if (length == 0)
        return 1;
    if (address < VMM_PAGE_SIZE || address >= USER_TOP ||
        length > USER_TOP - address)
        return 0;

    uint64_t end = address + length;
    uint64_t page = address & ~(VMM_PAGE_SIZE - 1ULL);

    while (page < end) {
        struct vmm_mapping_info info;
        if (!vmm_address_space_query(&current_space, page, &info) ||
            !info.user)
            return 0;
        page += VMM_PAGE_SIZE;
    }
    return 1;
}

/* Copy from user physical pages through the supervisor-only HHDM alias. */
static int copy_from_user(char *destination,
                          uint64_t source,
                          uint64_t length)
{
    if (!destination || length > USER_WRITE_MAX ||
        !user_range_readable(source, length))
        return 0;

    uint64_t copied = 0;
    while (copied < length) {
        uint64_t current = source + copied;
        uint64_t page_va = current & ~(VMM_PAGE_SIZE - 1ULL);
        uint64_t offset = current & (VMM_PAGE_SIZE - 1ULL);
        uint64_t chunk = VMM_PAGE_SIZE - offset;
        if (chunk > length - copied)
            chunk = length - copied;

        struct vmm_mapping_info info;
        if (!vmm_address_space_query(&current_space, page_va, &info) ||
            !info.user)
            return 0;

        void *physical_alias = pmm_phys_to_virt(
            (info.physical_address & ~(VMM_PAGE_SIZE - 1ULL)) + offset);
        if (!physical_alias)
            return 0;

        const volatile char *source_bytes =
            (const volatile char *)physical_alias;
        for (uint64_t i = 0; i < chunk; i++)
            destination[copied + i] = source_bytes[i];

        copied += chunk;
    }
    return 1;
}

void ring3_syscall_dispatch(struct ring3_syscall_frame *frame)
{
    if (!frame)
        return;

    /* The syscall ABI is deliberately available only to CPL3 callers. */
    if ((frame->cs & 0x3ULL) != 0x3ULL || !current_space.valid) {
        frame->rax = (uint64_t)-1;
        return;
    }

    switch (frame->rax) {
    case SYS_WRITE: {
        char buffer[USER_WRITE_MAX];
        uint64_t length = frame->rsi;
        if (length > USER_WRITE_MAX ||
            !copy_from_user(buffer, frame->rdi, length)) {
            frame->rax = (uint64_t)-1;
            return;
        }

        kernel_console_write(buffer, (size_t)length);
        frame->rax = length;
        return;
    }

    case SYS_EXIT: {
        /*
         * In 64-bit mode IRETQ always consumes the complete five-qword
         * return frame, including RSP and SS even when returning to CPL0.
         * The syscall arrived on the dedicated TSS.RSP0 stack, so the
         * address immediately after this frame is its original stack top.
         * Do not leave the user RSP/SS (0x2B) in a Ring-0 return frame.
         */
        uintptr_t kernel_resume_rsp =
            (uintptr_t)frame + sizeof(*frame);
        uintptr_t kernel_stack_bottom = (uintptr_t)ring3_kernel_stack;
        uintptr_t kernel_stack_top =
            kernel_stack_bottom + sizeof(ring3_kernel_stack);

        if (kernel_resume_rsp != kernel_stack_top ||
            (kernel_resume_rsp & 0xFULL) != 0) {
            frame->rax = (uint64_t)-1;
            return;
        }

        exit_status = frame->rdi;
        frame->rip = (uint64_t)(uintptr_t)ring3_exit_trampoline;
        frame->cs = GDT_KERNEL_CODE_SELECTOR;
        frame->rflags = 0x2ULL; /* IF=0, IOPL=0, reserved bit 1 set. */
        frame->rsp = (uint64_t)kernel_resume_rsp;
        frame->ss = GDT_KERNEL_DATA_SELECTOR;
        frame->rax = 0;
        return;
    }

    default:
        frame->rax = (uint64_t)-1;
        return;
    }
}

/* Called by the assembly exit trampoline on the dedicated RSP0 stack. */
void ring3_finish_exit(void)
{
    if (!vmm_activate_kernel_address_space())
        ring3_halt();

    if (!current_space.valid ||
        !vmm_address_space_destroy(&current_space))
        ring3_halt();

    exit_cleanup_ok = 1;
    exit_seen = 1;

    if (exit_status == 0) {
        static const char message[] = "[R3  ] User program exited: 0\n";
        kernel_console_write(message, sizeof(message) - 1);
    } else {
        static const char message[] = "[R3  ] User program exited: nonzero\n";
        kernel_console_write(message, sizeof(message) - 1);
    }
}

int ring3_run_self_test(void)
{
    const uint64_t kernel_root = vmm_root_phys();
    const uintptr_t kernel_stack_top =
        ((uintptr_t)(ring3_kernel_stack + sizeof(ring3_kernel_stack))) &
        ~(uintptr_t)0xFULL;
    const size_t payload_size = (size_t)(user_blob_end - user_blob_start);

    if (!kernel_root || !gdt_user_segments_verify() ||
        !idt_syscall_gate_verify() || payload_size == 0 ||
        payload_size > VMM_PAGE_SIZE ||
        !gdt_set_rsp0(kernel_stack_top) ||
        !gdt_rsp0_verify(kernel_stack_top))
        return 0;

    current_space = (struct vmm_address_space){0};
    exit_status = UINT64_MAX;
    exit_seen = 0;
    exit_cleanup_ok = 0;

    uint64_t code_phys = 0;
    uint64_t stack_phys = 0;

    if (!vmm_address_space_create(&current_space))
        return 0;

    if (!vmm_address_space_map_user_page(
            &current_space,
            USER_CODE_BASE,
            VMM_PROT_USER | VMM_PROT_EXEC,
            &code_phys) ||
        !vmm_address_space_map_user_page(
            &current_space,
            USER_STACK_BASE,
            VMM_PROT_USER | VMM_PROT_WRITE,
            &stack_phys))
        goto fail;

    void *code_page = pmm_phys_to_virt(code_phys);
    if (!code_page || !stack_phys)
        goto fail;

    uint8_t *destination = (uint8_t *)code_page;
    for (size_t i = 0; i < payload_size; i++)
        destination[i] = user_blob_start[i];

    struct vmm_mapping_info code_info;
    struct vmm_mapping_info stack_info;
    if (!vmm_address_space_query(&current_space, USER_CODE_BASE, &code_info) ||
        !vmm_address_space_query(&current_space, USER_STACK_BASE, &stack_info) ||
        !code_info.user || code_info.writable || !code_info.executable ||
        !stack_info.user || !stack_info.writable || stack_info.executable)
        goto fail;

    if (!gdt_set_rsp0(kernel_stack_top) ||
        !gdt_rsp0_verify(kernel_stack_top) ||
        !vmm_address_space_activate(&current_space))
        goto fail;

    /* Verify effective (all paging levels) permissions under the new CR3. */
    struct vmm_mapping_info active_code;
    struct vmm_mapping_info active_stack;
    struct vmm_mapping_info active_kernel_stack;
    if (!vmm_query_mapping(USER_CODE_BASE, &active_code) ||
        !vmm_query_mapping(USER_STACK_BASE, &active_stack) ||
        !vmm_query_mapping(kernel_stack_top - sizeof(uint64_t),
                           &active_kernel_stack) ||
        !active_code.user || active_code.writable || !active_code.executable ||
        !active_stack.user || !active_stack.writable || active_stack.executable ||
        active_kernel_stack.user || !active_kernel_stack.writable ||
        active_kernel_stack.executable ||
        vmm_root_phys() != current_space.root_phys)
        goto fail;

    /* The payload exits through SYS_EXIT; ring3_enter then returns normally. */
    ring3_enter(USER_CODE_BASE, USER_STACK_TOP);

    return exit_seen && exit_cleanup_ok && exit_status == 0 &&
           !current_space.valid && vmm_root_phys() == kernel_root;

fail:
    if (!vmm_activate_kernel_address_space())
        ring3_halt();
    if (current_space.valid && !vmm_address_space_destroy(&current_space))
        ring3_halt();
    return 0;
}
