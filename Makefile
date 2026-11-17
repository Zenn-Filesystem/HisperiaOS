# =========================================
# HISPERIA OS BUILD SYSTEM
# =========================================

PROJECT := HisperiaOS

CC := clang
LD := ld.lld

BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/iso_root

C_SRC := \
    src/main.c \
    src/gdt.c \
    src/idt.c \
    src/isr.c \
    src/pmm.c \
    src/vmm.c

ASM_SRC := \
    src/gdt_load.S \
    src/isr.S

C_OBJ := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(C_SRC))
ASM_OBJ := $(patsubst src/%.S,$(BUILD_DIR)/%.asm.o,$(ASM_SRC))
OBJ := $(C_OBJ) $(ASM_OBJ)

KERNEL := $(BUILD_DIR)/kernel.elf
ISO := $(BUILD_DIR)/hisperia.iso

LINKER_SCRIPT := linker.ld
LIMINE_DIR := limine
LIMINE_TOOL := $(LIMINE_DIR)/limine
LIMINE_CONF := limine.conf

# =========================================
# COMPILER FLAGS
# =========================================

RUN_EXCEPTION_TEST ?= 0

CFLAGS := \
    --target=x86_64-unknown-none-elf \
    -std=gnu11 \
    -O2 \
    -Wall \
    -Wextra \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pic \
    -fno-pie \
    -fno-builtin \
    -fno-asynchronous-unwind-tables \
    -fno-unwind-tables \
    -mno-red-zone \
    -mcmodel=kernel \
    -mno-mmx \
    -mno-sse \
    -mno-sse2 \
    -Iinclude \
    -MMD -MP

CFLAGS += -DHISPERIA_RUN_EXCEPTION_TEST=$(RUN_EXCEPTION_TEST)

# =========================================
# LINKER FLAGS
# =========================================

LDFLAGS := \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    --build-id=none

# =========================================
# DEFAULT
# =========================================

.PHONY: all kernel iso run clean help

all: kernel

# =========================================
# BUILD DIRECTORY
# =========================================

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# =========================================
# COMPILE
# =========================================

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.asm.o: src/%.S | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Header dependency tracking: ubah include/*.h -> objek terkait dibangun ulang.
-include $(OBJ:.o=.d)


# =========================================
# LINK
# =========================================

$(KERNEL): $(OBJ) $(LINKER_SCRIPT)
	$(LD) $(LDFLAGS) \
	    -T $(LINKER_SCRIPT) \
	    -o $(KERNEL) \
	    $(OBJ)

kernel: $(KERNEL)
	@echo ""
	@echo "[BUILD] Kernel compiled successfully."
	@echo "[BUILD] $(KERNEL)"
	@echo ""

# =========================================
# ISO GENERATION
# =========================================

iso: $(ISO)

$(ISO): $(KERNEL) $(LIMINE_CONF)
	@test -f $(LIMINE_DIR)/limine-bios.sys || \
	    (echo "[ERROR] Missing limine-bios.sys"; exit 1)

	@test -f $(LIMINE_DIR)/limine-bios-cd.bin || \
	    (echo "[ERROR] Missing limine-bios-cd.bin"; exit 1)

	@test -f $(LIMINE_DIR)/limine-uefi-cd.bin || \
	    (echo "[ERROR] Missing limine-uefi-cd.bin"; exit 1)

	@test -f $(LIMINE_DIR)/BOOTX64.EFI || \
	    (echo "[ERROR] Missing BOOTX64.EFI"; exit 1)

	@test -x $(LIMINE_TOOL) || \
	    (echo "[ERROR] Limine CLI not found or not executable"; exit 1)

	rm -rf $(ISO_ROOT)

	mkdir -p $(ISO_ROOT)/boot/limine
	mkdir -p $(ISO_ROOT)/EFI/BOOT

	cp $(KERNEL) \
	    $(ISO_ROOT)/boot/kernel.elf

	cp $(LIMINE_CONF) \
	    $(ISO_ROOT)/boot/limine/limine.conf

	cp $(LIMINE_DIR)/limine-bios.sys \
	    $(ISO_ROOT)/boot/limine/

	cp $(LIMINE_DIR)/limine-bios-cd.bin \
	    $(ISO_ROOT)/boot/limine/

	cp $(LIMINE_DIR)/limine-uefi-cd.bin \
	    $(ISO_ROOT)/boot/limine/

	cp $(LIMINE_DIR)/BOOTX64.EFI \
	    $(ISO_ROOT)/EFI/BOOT/

	xorriso -as mkisofs \
	    -R -r -J \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot \
	    -boot-load-size 4 \
	    -boot-info-table \
	    -hfsplus \
	    -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part \
	    --efi-boot-image \
	    --protective-msdos-label \
	    $(ISO_ROOT) \
	    -o $(ISO)

	$(LIMINE_TOOL) bios-install $(ISO)

	@echo ""
	@echo "[ISO] Successfully generated!"
	@echo "[ISO] $(ISO)"
	@echo ""

# =========================================
# QEMU
# =========================================

run: iso
	qemu-system-x86_64 \
	    -M q35 \
	    -m 256M \
	    -cdrom $(ISO) \
	    -serial stdio \
	    -monitor none \
	    -no-reboot \
	    -no-shutdown

# =========================================
# CLEAN
# =========================================

clean:
	rm -rf $(BUILD_DIR)
	@echo "[CLEAN] Build directory removed."

# =========================================
# HELP
# =========================================

help:
	@echo "HisperiaOS Build System"
	@echo ""
	@echo "make        - Build kernel"
	@echo "make kernel - Build kernel"
	@echo "make iso    - Generate bootable ISO"
	@echo "make run    - Run ISO using QEMU"
	@echo "make RUN_EXCEPTION_TEST=1 - Enable deliberate exception test"
	@echo "make clean  - Clean build files"