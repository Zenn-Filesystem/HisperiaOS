#include <stdint.h>
#include <stddef.h>

#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "ring3.h"
#include "vfs.h"
#include "syscall.h"

#define USER_CODE_BASE       0x0000000000400000ULL
#define USER_STACK_BASE      0x00000000007FF000ULL
#define USER_STACK_TOP       (USER_STACK_BASE + VMM_PAGE_SIZE)
#define USER_TOP             0x0000800000000000ULL
#define RING3_KERNEL_STACK_SIZE (16ULL * 1024ULL)
#define USER_WRITE_MAX       512ULL
#define USER_FD_COUNT        16U
#define USER_FD_BASE         3U


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
_Static_assert(sizeof(struct hisperia_stat) == 16,
               "user stat ABI size");
_Static_assert(sizeof(struct vfs_dirent) == sizeof(struct hisperia_dirent),
               "VFS/user dirent ABI size");
_Static_assert(offsetof(struct vfs_dirent, type) ==
               offsetof(struct hisperia_dirent, type),
               "VFS/user dirent ABI type offset");
_Static_assert(offsetof(struct vfs_dirent, name) ==
               offsetof(struct hisperia_dirent, name),
               "VFS/user dirent ABI name offset");

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
static struct vfs_file user_files[USER_FD_COUNT];

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
static int copy_from_user(void *destination,
                          uint64_t source,
                          uint64_t length)
{
    if ((!destination && length != 0) || length > USER_WRITE_MAX ||
        !user_range_readable(source, length))
        return 0;

    uint8_t *destination_bytes = (uint8_t *)destination;
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
            destination_bytes[copied + i] = (uint8_t)source_bytes[i];

        copied += chunk;
    }
    return 1;
}


static int user_range_writable(uint64_t address, uint64_t length)
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
            !info.user || !info.writable)
            return 0;
        page += VMM_PAGE_SIZE;
    }
    return 1;
}

static int copy_to_user(uint64_t destination,
                        const void *source,
                        uint64_t length)
{
    if ((!source && length != 0) || length > USER_WRITE_MAX ||
        !user_range_writable(destination, length))
        return 0;

    const uint8_t *source_bytes = (const uint8_t *)source;
    uint64_t copied = 0;
    while (copied < length) {
        uint64_t current = destination + copied;
        uint64_t page_va = current & ~(VMM_PAGE_SIZE - 1ULL);
        uint64_t offset = current & (VMM_PAGE_SIZE - 1ULL);
        uint64_t chunk = VMM_PAGE_SIZE - offset;
        if (chunk > length - copied)
            chunk = length - copied;

        struct vmm_mapping_info info;
        if (!vmm_address_space_query(&current_space, page_va, &info) ||
            !info.user || !info.writable)
            return 0;

        void *physical_alias = pmm_phys_to_virt(
            (info.physical_address & ~(VMM_PAGE_SIZE - 1ULL)) + offset);
        if (!physical_alias)
            return 0;

        volatile char *destination_bytes = (volatile char *)physical_alias;
        for (uint64_t i = 0; i < chunk; i++)
            destination_bytes[i] = (char)source_bytes[copied + i];
        copied += chunk;
    }
    return 1;
}

static int copy_user_string(char *destination,
                            uint64_t source,
                            size_t capacity)
{
    if (!destination || capacity == 0 || capacity > USER_WRITE_MAX)
        return 0;

    for (size_t i = 0; i < capacity; i++) {
        char byte;
        if (!copy_from_user(&byte, source + i, 1))
            return 0;
        destination[i] = byte;
        if (byte == '\0')
            return i != 0;
    }
    return 0;
}

static struct vfs_file *user_file_from_fd(uint64_t fd)
{
    if (fd < USER_FD_BASE || fd >= USER_FD_BASE + USER_FD_COUNT)
        return 0;

    struct vfs_file *file = &user_files[fd - USER_FD_BASE];
    return file->opened ? file : 0;
}

static void user_files_close_all(void)
{
    for (size_t i = 0; i < USER_FD_COUNT; i++) {
        if (user_files[i].opened)
            vfs_close(&user_files[i]);
        user_files[i] = (struct vfs_file){0};
    }
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
    case HISPERIA_SYS_WRITE: {
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

    case HISPERIA_SYS_OPEN: {
        char path[VFS_PATH_MAX + 1U];
        if (frame->rsi != 0 ||
            !copy_user_string(path, frame->rdi, sizeof(path))) {
            frame->rax = (uint64_t)-1;
            return;
        }

        for (size_t i = 0; i < USER_FD_COUNT; i++) {
            if (!user_files[i].opened && vfs_open(path, &user_files[i])) {
                frame->rax = USER_FD_BASE + i;
                return;
            }
        }
        frame->rax = (uint64_t)-1;
        return;
    }

    case HISPERIA_SYS_READ: {
        struct vfs_file *file = user_file_from_fd(frame->rdi);
        uint64_t destination = frame->rsi;
        uint64_t length = frame->rdx;
        char buffer[USER_WRITE_MAX];
        if (!file || length > USER_WRITE_MAX ||
            !user_range_writable(destination, length)) {
            frame->rax = (uint64_t)-1;
            return;
        }

        int64_t result = vfs_read(file, buffer, (size_t)length);
        if (result < 0 ||
            (result > 0 && !copy_to_user(destination, buffer,
                                         (uint64_t)result))) {
            frame->rax = (uint64_t)-1;
            return;
        }
        frame->rax = (uint64_t)result;
        return;
    }

    case HISPERIA_SYS_CLOSE: {
        struct vfs_file *file = user_file_from_fd(frame->rdi);
        if (!file) {
            frame->rax = (uint64_t)-1;
            return;
        }
        vfs_close(file);
        frame->rax = 0;
        return;
    }

    case HISPERIA_SYS_SEEK: {
        struct vfs_file *file = user_file_from_fd(frame->rdi);
        uint64_t new_offset = 0;
        if (!file || frame->rdx > VFS_SEEK_END ||
            !vfs_seek(file, (int64_t)frame->rsi,
                      (int)frame->rdx, &new_offset)) {
            frame->rax = (uint64_t)-1;
            return;
        }
        frame->rax = new_offset;
        return;
    }

    case HISPERIA_SYS_STAT: {
        struct hisperia_stat user_stat = {0};
        char path[VFS_PATH_MAX + 1U];

        if (!copy_user_string(path, frame->rdi, sizeof(path)) ||
            !vfs_stat(path, &user_stat.size, &user_stat.type) ||
            !copy_to_user(frame->rsi, &user_stat, sizeof(user_stat))) {
            frame->rax = (uint64_t)-1;
            return;
        }

        frame->rax = 0;
        return;
    }

    case HISPERIA_SYS_READDIR: {
        char path[VFS_PATH_MAX + 1U];
        uint32_t cursor = 0;
        struct vfs_dirent entry;
        volatile uint8_t *entry_bytes = (volatile uint8_t *)&entry;
        for (size_t i = 0; i < sizeof(entry); i++)
            entry_bytes[i] = 0;

        if (!copy_user_string(path, frame->rdi, sizeof(path)) ||
            !user_range_writable(frame->rsi, sizeof(cursor)) ||
            !user_range_writable(frame->rdx, sizeof(entry)) ||
            !copy_from_user(&cursor, frame->rsi, sizeof(cursor))) {
            frame->rax = (uint64_t)-1;
            return;
        }

        int result = vfs_readdir(path, &cursor, &entry);
        if (result < 0 ||
            (result == 1 && !copy_to_user(frame->rdx, &entry,
                                          sizeof(entry))) ||
            !copy_to_user(frame->rsi, &cursor, sizeof(cursor))) {
            frame->rax = (uint64_t)-1;
            return;
        }

        /* 1 = entry copied, 0 = end of directory, -1 = error. */
        frame->rax = (uint64_t)result;
        return;
    }

    case HISPERIA_SYS_EXIT: {
        /*
         * IRETQ always consumes the complete five-qword return frame.
         * Replace user RSP/SS with the suspended kernel stack before return.
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

        user_files_close_all();
        exit_status = frame->rdi;
        frame->rip = (uint64_t)(uintptr_t)ring3_exit_trampoline;
        frame->cs = GDT_KERNEL_CODE_SELECTOR;
        frame->rflags = 0x2ULL;
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
    user_files_close_all();
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
