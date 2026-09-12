#!/usr/bin/env bash
# Build a bootable auroraOS.iso without needing grub-mkrescue/xorriso.
# Uses grub-mkimage directly (i386-pc-eltorito target) with the GRUB
# config embedded in a memdisk, plus genisoimage for El Torito packaging.
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
mkdir -p "$BUILD_DIR/memdisk/boot/grub"
mkdir -p "$BUILD_DIR/iso/boot"

cat > "$BUILD_DIR/memdisk/boot/grub/grub.cfg" <<'EOF'
set timeout=2
set default=0

insmod iso9660
insmod biosdisk

menuentry "auroraOS" {
	search --no-floppy --set=root --file /boot/auroraos.bin
	multiboot /boot/auroraos.bin
	boot
}
EOF

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

cp "$KERNEL_BIN" "$BUILD_DIR/iso/boot/auroraos.bin"
cp "$BUILD_DIR/eltorito.img" "$BUILD_DIR/iso/boot/eltorito.img"

genisoimage -R -b boot/eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table \
	-o "$OUT_ISO" "$BUILD_DIR/iso"

echo "Built $OUT_ISO"
