#!/usr/bin/env bash
# Build a bootable, hybrid (CD + raw USB) auroraOS.iso.
#
# Prefers grub-mkrescue (needs xorriso), which produces a proper hybrid
# ISO: bootable both as an optical disc via El Torito, and as a raw disk
# image on a USB flash drive (real PCs mostly have no optical drive, and
# tools like Rufus/dd/Ventoy write ISOs straight to a USB stick as a disk
# image - that only works if the ISO also carries a valid MBR partition
# table, which grub-mkrescue embeds via boot_hybrid.img).
#
# Falls back to a hand-built El Torito-only ISO (grub-mkimage +
# genisoimage) if xorriso/grub-mkrescue aren't available. That ISO still
# boots fine from a real or virtual CD/DVD, but may not boot from a
# plain USB stick write on hardware that skips CD emulation.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
KERNEL_BIN="$ROOT_DIR/kernel_bin/auroraos.bin"
OUT_ISO="$ROOT_DIR/auroraos.iso"

if [ ! -f "$KERNEL_BIN" ]; then
	echo "error: $KERNEL_BIN not found. Run 'make kernel' first." >&2
	exit 1
fi

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/iso/boot/grub"
cp "$KERNEL_BIN" "$BUILD_DIR/iso/boot/auroraos.bin"

cat > "$BUILD_DIR/iso/boot/grub/grub.cfg" <<'EOF'
set timeout=1
set default=0

insmod iso9660
insmod biosdisk

# Auto-detect the display's best available resolution rather than
# forcing a fixed mode: `gfxmode=auto` has GRUB enumerate the VBE modes
# the BIOS actually reports for this display (including EDID-reported
# modes, when the BIOS exposes them) and pick the highest usable one,
# and `gfxpayload=keep` tells GRUB to hand that already-set mode
# straight to the kernel instead of renegotiating down to whatever
# fixed width/height the Multiboot header asks for (boot.s's request is
# now just a fallback for the rare setup where gfxpayload didn't take).
insmod vbe
insmod video
set gfxmode=auto
set gfxpayload=keep

# Real (not simulated) OTA-style update path: the boot ISO itself is a
# read-only CD-ROM image, so a running auroraOS can't rewrite what it
# booted from - but it CAN write a new kernel binary to its writable
# FAT16 data disk (see kernel/updatecmd.c's `update` command). GRUB
# looks for that file, on ANY attached disk, before falling back to the
# kernel baked into this ISO. `search` here doesn't error out if the
# file isn't found (unlike the required `search` below for the ISO's
# own kernel) - it just leaves $root unset, which the `if` catches.
insmod fat
insmod part_msdos

menuentry "auroraOS" {
	search --no-floppy --set=root --file /AURORAOS.UPD || true
	if [ -n "$root" ]; then
		echo "Found an update on the data disk - booting it."
		multiboot /AURORAOS.UPD
		boot
	fi

	search --no-floppy --set=root --file /boot/auroraos.bin
	multiboot /boot/auroraos.bin
	boot
}
EOF

if command -v grub-mkrescue >/dev/null 2>&1 && command -v xorriso >/dev/null 2>&1; then
	echo "Building hybrid ISO with grub-mkrescue (CD + USB bootable)..."
	grub-mkrescue -o "$OUT_ISO" "$BUILD_DIR/iso"
	echo "Built $OUT_ISO (hybrid: bootable from a burned CD/DVD or a USB stick written with dd/Rufus/Ventoy)"
	exit 0
fi

echo "grub-mkrescue/xorriso not available - falling back to a manual" >&2
echo "El Torito-only ISO (CD/DVD boot only, may not boot from raw USB)." >&2

mkdir -p "$BUILD_DIR/memdisk/boot/grub"
cp "$BUILD_DIR/iso/boot/grub/grub.cfg" "$BUILD_DIR/memdisk/boot/grub/grub.cfg"

cat > "$BUILD_DIR/early.cfg" <<'EOF'
configfile (memdisk)/boot/grub/grub.cfg
EOF

( cd "$BUILD_DIR/memdisk" && tar -cf "$BUILD_DIR/memdisk.tar" boot )

grub-mkimage \
	-O i386-pc-eltorito \
	-o "$BUILD_DIR/eltorito.img" \
	-m "$BUILD_DIR/memdisk.tar" \
	-c "$BUILD_DIR/early.cfg" \
	-p /boot/grub \
	biosdisk iso9660 multiboot normal configfile search search_fs_file memdisk tar

cp "$BUILD_DIR/eltorito.img" "$BUILD_DIR/iso/boot/eltorito.img"

genisoimage -R -b boot/eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table \
	-o "$OUT_ISO" "$BUILD_DIR/iso"

if command -v isohybrid >/dev/null 2>&1; then
	isohybrid "$OUT_ISO" 2>/dev/null || true
fi

echo "Built $OUT_ISO"
