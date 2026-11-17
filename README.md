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

## Address-space scope and limitations

The address-space work is the kernel-side VM foundation, **not a complete process subsystem**. It can create independent four-level roots, populate user pages, switch CR3, inspect/unmap those pages, and free the private lower-half page-table tree. The high half is shared from the boot kernel root with the top-level U/S bit cleared.

The following are not implemented yet:

- ring-3 transition / user-mode entry and return
- syscall ABI and syscall entry
- ELF loader, user stack setup, and process image lifecycle
- scheduler and per-thread kernel stacks
- SMP locking and TLB shootdown (the current implementation is single-CPU oriented)
- five-level paging (LA57)
- complete auditing of all possible physical aliases and memory types

Do not run these APIs concurrently from multiple CPUs. Address-space destruction must only be done after switching away from that root on the running CPU.

## Validation status of this source package

- Normal kernel ELF: compiled successfully with Clang + LLD.
- `RUN_MPROTECT_TEST=1`: compile and link verified.
- `RUN_MPROTECT_TEST=2`: compile and link verified.
- ELF program headers checked: separate R, R-X, R, and RW load segments; no RWX load segment.
- QEMU / physical-hardware runtime: **not verified in the packaging environment**. `qemu-system-x86_64` and `xorriso` were not installed there. Boot and inspect the serial log on the target machine before treating the new runtime self-tests as passed.

## Repository hygiene

Generated build output, ELF / ISO files, object and dependency files, logs, and ZIP archives are ignored by Git.
