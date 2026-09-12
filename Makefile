CC = gcc
AS = gcc
LD = ld

CFLAGS = -m32 -std=gnu11 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-pie -no-pie
ASFLAGS = -m32 -ffreestanding -c
LDFLAGS = -m elf_i386 -T kernel/linker.ld -nostdlib

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
KERNEL_BIN = kernel_bin/auroraos.bin

.PHONY: all kernel iso run clean wallpapers

all: iso

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

# Boot the ISO through GRUB. This has to go through GRUB (not QEMU's
# built-in -kernel multiboot loader): the kernel's multiboot header asks
# for a VBE graphics mode, and it's GRUB that performs that BIOS video
# mode call on our behalf before handing off to the kernel. -vga std
# requests QEMU's standard (non-Cirrus) VGA/VBE emulation, which is what
# this was developed and tested against.
run: iso
	qemu-system-i386 -cdrom $(ISO) -vga std

clean:
	rm -f kernel/*.o
	rm -rf kernel_bin build kernel/generated
	rm -f $(ISO)
