CC = gcc
AS = gcc
LD = ld

# -mno-red-zone is mandatory for x86-64 kernel/freestanding code: the
# System V ABI's 128-byte "red zone" below %rsp is only safe for code
# that can never be interrupted asynchronously, which does not describe
# an OS kernel taking hardware interrupts. -mcmodel=large keeps codegen
# from assuming the kernel lives in the low 2GB with 32-bit-relative
# addressing, since we control our own link address (1MB) rather than
# a general-purpose toolchain default. -mgeneral-regs-only (which
# implies -mno-sse/-mno-mmx/etc.) stops GCC from auto-vectorizing loops
# (memcpy-like patterns especially) into SSE instructions: SSE2 is
# baseline on x86-64 as far as the compiler's concerned, but using it
# requires the kernel to have enabled it first (CR0/CR4 bits, plus
# saving/restoring FPU/SSE state across interrupts) - we do neither, so
# any SSE instruction traps as an Invalid Opcode exception. Simplest fix
# for a kernel with no real floating-point needs: never emit them.
CFLAGS = -m64 -mno-red-zone -mcmodel=large -mgeneral-regs-only -std=gnu11 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-pie -no-pie
ASFLAGS = -m64 -ffreestanding -c
LDFLAGS = -m elf_x86_64 -T kernel/linker.ld -nostdlib -z max-page-size=0x1000

# Wallpapers are baked in at build time as raw RGB C arrays (see
# tools/img_to_c.py) since the freestanding kernel has no image decoder.
# This is the resolution they're stored at; gfx_blit_rgb_scaled() scales
# to whatever the real runtime screen resolution turns out to be.
WALLPAPER_W = 1280
WALLPAPER_H = 800
WALLPAPER_SRCS = assets/wallpapers/auroraosday.png assets/wallpapers/auroraosnight.png
WALLPAPER_GEN = kernel/generated/wallpaper_day.c kernel/generated/wallpaper_night.c

KERNEL_SRCS_C = $(wildcard kernel/*.c) $(WALLPAPER_GEN)
KERNEL_OBJS_C = $(KERNEL_SRCS_C:.c=.o)
KERNEL_OBJ_S = kernel/boot.o

ISO = auroraos.iso
DISK_IMG = auroraos_disk.img
KERNEL_BIN = kernel_bin/auroraos.bin

.PHONY: all kernel iso disk run clean wallpapers

all: iso disk

kernel/generated/wallpaper_day.c: assets/wallpapers/auroraosday.png tools/img_to_c.py
	mkdir -p kernel/generated
	python3 tools/img_to_c.py $< wallpaper_day $(WALLPAPER_W) $(WALLPAPER_H) > $@

kernel/generated/wallpaper_night.c: assets/wallpapers/auroraosnight.png tools/img_to_c.py
	mkdir -p kernel/generated
	python3 tools/img_to_c.py $< wallpaper_night $(WALLPAPER_W) $(WALLPAPER_H) > $@

wallpapers: $(WALLPAPER_GEN)

kernel/boot.o: kernel/boot.s
	$(AS) $(ASFLAGS) $< -o $@

kernel/%.o: kernel/%.c kernel/kernel.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel/generated/%.o: kernel/generated/%.c kernel/kernel.h
	$(CC) $(CFLAGS) -Ikernel -c $< -o $@

kernel: $(KERNEL_OBJ_S) $(KERNEL_OBJS_C)
	mkdir -p kernel_bin
	$(LD) $(LDFLAGS) -o $(KERNEL_BIN) $(KERNEL_OBJ_S) $(KERNEL_OBJS_C)

# Builds auroraos.iso via scripts/make_iso.sh (grub-mkimage + genisoimage),
# which works even when xorriso/grub-mkrescue are unavailable.
iso: kernel
	./scripts/make_iso.sh

# Blank writable disk image for the FAT16 filesystem (see kernel/ata.c,
# kernel/fat16.c, and scripts/make_disk.sh). Separate from the ISO: the
# ISO is the read-only CD-ROM boot medium, this is a plain disk auroraOS
# formats and writes to at runtime. Not regenerated once it exists, so
# your files survive rebuilding the kernel/ISO.
disk:
	./scripts/make_disk.sh

# Boot the ISO through GRUB, with the disk image attached as index=1
# (primary slave) - verified not to collide with wherever -cdrom puts
# itself. This has to go through GRUB (not QEMU's built-in -kernel
# multiboot loader): the kernel's multiboot header asks for a VBE
# graphics mode, and it's GRUB that performs that BIOS video mode call
# on our behalf before handing off to the kernel. -vga std requests
# QEMU's standard (non-Cirrus) VGA/VBE emulation, which is what this
# was developed and tested against. Needs qemu-system-x86_64, not
# qemu-system-i386: the kernel switches itself into 64-bit long mode
# early in boot.s, which an i386-only CPU model can't execute at all.
# -boot d forces booting from the CD-ROM first: without it, some
# BIOS/QEMU version combinations prefer booting from the hard disk once
# one is attached, which would try (and fail) to boot the blank/FAT16
# data disk instead of auroraOS itself.
run: iso disk
	qemu-system-x86_64 -cdrom $(ISO) -vga std -drive file=$(DISK_IMG),format=raw,if=ide,index=1 -boot d

clean:
	rm -f kernel/*.o
	rm -rf kernel_bin build kernel/generated
	rm -f $(ISO)

# Deliberately NOT part of `clean`: the disk image holds the user's
# files across rebuilds. Remove it explicitly (and re-run `make disk`)
# to reset the filesystem to blank.
clean-disk:
	rm -f $(DISK_IMG)
