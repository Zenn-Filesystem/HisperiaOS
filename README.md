# HisperiaOS

A freestanding x86-64 kernel in C and x86-64 assembly, booted with Limine (base revision 6).

## Implemented subsystems

- Limine boot handoff, framebuffer output, COM1 serial diagnostics
- GDT, TSS, IDT, IST1, and CPU exception reporting
- Bitmap PMM with 4 KiB frames, allocation/free validation, and HHDM access
- Four-level x86-64 page-table inspection and mapping walker
- 1 GiB / 2 MiB huge-page splitting when a 4 KiB permission change is needed
- CR0.WP + EFER.NXE activation
- Kernel section W^X permissions and HHDM alias permission updates
- Explicit read-only-write and NX-execute page-fault tests
- Supervisor page map/unmap helpers
- Guarded IST1 stack: four mapped RW/NX pages with an unmapped lower guard page
- Page-backed kernel heap with `kmalloc()` / `kfree()`, splitting, and adjacent free-block coalescing
- Independent user address-space roots, private lower-half mappings, shared supervisor-only upper-half mappings, CR3 activation, query, unmap, and destroy APIs
- Ring 3 GDT descriptors (DPL 3), TSS.RSP0 kernel-entry stack, and a DPL 3 `int 0x80` gate
- First CPL3 payload copied into a user RX page, with a separate user RW/NX stack
- Generic VFS dispatcher with an ext4 read-only backend; filesystem-specific inode/path logic is kept outside the VFS core
- Cross-transport storage discovery: AHCI first, then legacy ATA PIO fallback; both feed the same block-device, MBR/GPT, ext4, and VFS path
- Read-only filesystem syscall ABI: `write`, `exit`, `open`, `read`, `close`, `seek`, `stat`, and `readdir` (IDs 1–8), with bounded user-pointer validation/copy via the HHDM
- Ring 3 exit trampoline that restores the kernel CR3, destroys the temporary address space, and resumes the suspended kernel caller

## Source layout

```text
src/                    Kernel core and orchestration
  main.c                Boot flow and early console/input loop
  vfs.c, blockdev.c     Device-neutral kernel services
  gdt.c, idt.c, ...     CPU, memory, and process primitives

drivers/                Hardware-facing implementations
  bus/                  PCI enumeration
  firmware/             ACPI power management
  input/                PS/2 keyboard transport
  storage/              ATA PIO and AHCI controllers

lib/                    Device-neutral reusable code
  fs/                   FAT32 and ext4 filesystem implementations
  input/                Keyboard event queue and US keymap
  storage/              MBR/GPT partition parsing

include/                Public interfaces shared across subsystems
tools/                  Host-side validation programs
```

The keyboard path is intentionally split: `drivers/input/ps2_keyboard.c`
reads the i8042 controller and translates Set-1 scan codes into HID-style
keycodes; `lib/input/keyboard.c` provides a device-neutral event queue; and
`lib/input/keymap_us.c` converts keycodes plus modifiers to ASCII. Other
keyboard transports can feed the same event queue without changing the
keymap or console.

## Minimal keyboard support

The current early driver supports the first i8042/PS/2 keyboard port, enables
controller Set-2-to-Set-1 translation, and polls the output buffer. It handles
basic alphanumeric keys, punctuation, Enter, Backspace, Tab, modifiers,
Caps Lock, and common extended navigation keys. Pressed printable keys are
echoed to the framebuffer and serial console.

This is an early polling implementation: it does not yet use IRQ1, PIC/APIC
routing, USB HID, mouse input, layouts other than US, or a terminal line editor.
The ACPI power-button fixed event remains polled from the same idle loop.

Validation includes host tests for keymap behavior, event queue ordering,
and overflow handling. Hardware/QEMU keyboard operation still needs runtime
verification on the target machine. See `KEYBOARD-DRIVER.md` for the driver
contract, supported keys, and current limitations.

## Memory-protection policy

| Region | Permissions |
|---|---|
| Limine requests | Read-only, NX, supervisor |
| Kernel `.text` | Read + Execute, supervisor |
| Kernel `.rodata` | Read-only, NX, supervisor |
| Kernel `.data` / `.bss` | Read + Write, NX, supervisor |
| Guard page below IST1 | Not present |
| IST1 stack pages | Read + Write, NX, supervisor |
| Heap pages | Read + Write, NX, supervisor |
| User pages | User-accessible, NX by default; W^X enforced |

The HHDM aliases for kernel sections are also adjusted so the direct map does not trivially bypass the section permissions.

## Build

Requirements: `clang`, `ld.lld`, `xorriso`, the included Limine boot assets, and `qemu-system-x86_64` for the `run` target.

```sh
make clean
make kernel
make iso
make run
```

`make kernel` builds `build/kernel.elf`; `make iso` builds `build/hisperia.iso`; `make run` boots it in QEMU with serial output.

## Fault-enforcement tests

Each test intentionally causes one fatal page fault and then halts. Run the tests separately:

```sh
# Attempt to write into .rodata: expect Present=1, Write=1, supervisor #PF
make clean
make RUN_MPROTECT_TEST=1 run

# Attempt to execute a function pointer into .data: expect Present=1, Instruction=1 #PF
make clean
make RUN_MPROTECT_TEST=2 run
```

The exception path checks both the page-fault error bits and CR2's page against the intended probe before reporting PASS. Rebuild normal mode afterwards:

```sh
make clean
make run
```

## Runtime self-tests on normal boot

Normal startup runs:

1. PMM allocate / touch through HHDM / free self-test.
2. W^X mapping inspection for each kernel section and its HHDM alias.
3. Guard-page presence check and IST1 stack range verification.
4. Heap allocation, data-integrity, free, and coalescing self-test.
5. Two-address-space test: identical user VA mapped to distinct frames, user permission inspection, CR3 switching, kernel-half supervisor isolation, restoration of the kernel root, and destruction of both test spaces.

## Ring 3 first-program milestone

Normal boot now runs a tiny position-independent assembly payload at CPL3. It exercises `write`, `stat`, `readdir`, `open`, `seek`, `read`, `close`, and `exit` through `int 0x80`. Filesystem calls are dispatched through the generic VFS; the current mounted ext4 root is read-only. The syscall handler validates the caller privilege and user mappings, copies data through the supervisor HHDM alias, and never dereferences a user pointer directly. The syscall numbers and fixed `stat` / `dirent` layouts are declared in `include/syscall.h`. `SYS_EXIT` rewrites the interrupt return frame to a Ring 0 trampoline; the trampoline restores the kernel CR3, destroys the temporary address space, and returns to the suspended kernel call site.

Selectors are kept stable for the existing kernel/TSS setup: kernel code `0x08`, kernel data `0x10`, TSS `0x18`, user data `0x2B`, and user code `0x33`. The syscall interrupt gate is vector `0x80`, selector `0x08`, type attributes `0xEE` (present, DPL 3, 64-bit interrupt gate).

The demonstration is intentionally a single synchronous task. It is a real CPU CPL3 transition, but it is not yet an application environment: the payload is a built-in assembly blob rather than an ELF file, and the syscall ABI is deliberately small and read-only. `open` currently accepts regular files; `readdir` uses an absolute path and a 32-bit byte-offset cursor rather than a directory file descriptor. The 16-entry descriptor table is currently part of the Ring 3 test harness, not a per-process resource table. User IF remains cleared because hardware IRQ handlers are not installed yet; software `int 0x80` works independently of IF.

## Address-space scope and limitations

The address-space work is the kernel-side VM foundation, **not a complete process subsystem**. It can create independent four-level roots, populate user pages, switch CR3, inspect/unmap those pages, and free the private lower-half page-table tree. The high half is shared from the boot kernel root with the top-level U/S bit cleared.

The following are not implemented yet:

- ELF loader and dynamic program loading
- general process image lifecycle and robust per-process resources
- scheduler and per-thread kernel stacks
- user-mode exception delivery / process termination on faults (unexpected user exceptions currently reach the kernel fatal exception path)
- SMP locking and TLB shootdown (the current implementation is single-CPU oriented)
- five-level paging (LA57)
- complete auditing of all possible physical aliases and memory types

Do not run these APIs concurrently from multiple CPUs. Address-space destruction must only be done after switching away from that root on the running CPU.

## Ring 3 boot validation

After building and booting the normal kernel, the serial output should include:

```text
[GDT ] Ring 3 code/data descriptors PASS.
[IDT ] int 0x80 DPL3 gate PASS.
[R3  ] Starting Ring 3 user-program test...
[USER] Hello from HisperiaOS Ring 3!
[USER] stat + readdir syscalls PASS
[R3  ] User program exited: 0
[R3  ] CPL3 execution + syscall + exit round-trip PASS.
```

`make validate` runs host tests for block/partition/FAT32 validation, generic VFS dispatch, and synthetic ext4 + VFS integration, then compiles the normal kernel and both deliberate memory-protection fault-test configurations. The actual CPL3 file-syscall path must still be runtime-tested on the target machine or in QEMU; a successful compile alone does not prove that `iretq`, TSS stack switching, AHCI I/O under the user CR3, or the syscall return path execute correctly.

## Validation status of this source package

- Normal kernel ELF: compiled successfully with Clang + LLD.
- `RUN_MPROTECT_TEST=1`: compile and link verified.
- `RUN_MPROTECT_TEST=2`: compile and link verified.
- ELF program headers checked: separate R, R-X, R, and RW load segments; no RWX load segment.
- AHCI physical-hardware runtime: the maintainer reports successful SATA detection and partition enumeration on the target laptop.
- New Ring 3 filesystem syscalls: **not runtime-verified in the packaging environment**. `qemu-system-x86_64` and `xorriso` were not installed there. Boot on the target machine and inspect the serial log before treating the `stat` / `readdir` / file-read syscall path as passed.

## Repository hygiene

Generated build output, ELF / ISO files, object and dependency files, logs, and ZIP archives are ignored by Git.


## Storage foundation (Block Device / MBR-GPT / FAT32)

The storage stack provides a 512-byte `block_device` interface, a RAM-disk
backend for kernel tests, MBR/GPT parsing (including GPT CRC32 checks), and
FAT32 BPB/geometry detection. Both legacy ATA PIO and AHCI register as block-device
backends. At boot the kernel probes AHCI first, then tries ATA PIO if no ext4 root
was mounted; partition scanning and ext4/VFS mounting are shared across both
transports.

Run the host-side parser tests with `make test-storage-host`. To exercise the
legacy IDE / ATA PIO path in QEMU, use `make run-fat32`; to exercise AHCI, use
`make run-sata`. The generated QEMU fixture is FAT32, which is currently detected
but is not yet registered as a generic VFS backend. Ring 3 file syscalls therefore
require an ext4 partition to be mounted. FAT32 VFS operations, NVMe, USB mass
storage, and GBA ROM loading remain future work. See `CROSS-LOAD.md` for the
cross-transport boot flow.

## AHCI SATA (experimental)

See `AHCI-README.md`. Build with `make validate`; test the AHCI path with `make run-sata` on a host with QEMU and xorriso installed. The driver is polling and read-only. The maintainer reports that it detects the SATA device and enumerates its partitions on the target laptop; this does not imply support for every AHCI controller.


## ext4 + VFS

See [EXT4-VFS-README.md](EXT4-VFS-README.md) for the read-only ext4 backend and VFS API. The metadata checksum / `CSUM_SEED` implementation and its exact support limits are documented in [EXT4-READONLY-V0.2.md](EXT4-READONLY-V0.2.md).
