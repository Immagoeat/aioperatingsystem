#!/usr/bin/env bash
# Creates a blank, raw disk image for auroraOS's persistent filesystem.
#
# This is separate from auroraos.iso (the boot medium) on purpose: the
# ISO is a CD-ROM image (read-only, El Torito) that GRUB and the kernel
# boot from; auroraos_disk.img is a plain writable disk image that gets
# attached as a second drive (secondary ATA bus - see kernel/ata.c) for
# the FAT16 filesystem the terminal/editor/compiler actually read and
# write. auroraOS formats it as FAT16 itself on first boot if it doesn't
# already look like one (see fat16_mount()/fat16_format() in
# kernel/fat16.c) - this script just needs to produce the right *size*
# of zeroed file; the filesystem structure is written by the kernel.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_IMG="$ROOT_DIR/auroraos_disk.img"
DISK_SIZE_MB="${1:-64}"

if [ -f "$DISK_IMG" ]; then
	echo "Disk image already exists at $DISK_IMG (size unchanged; delete it first if you want to resize/reset it)."
	exit 0
fi

dd if=/dev/zero of="$DISK_IMG" bs=1M count="$DISK_SIZE_MB" status=none

echo "Created $DISK_IMG (${DISK_SIZE_MB}MB, blank). auroraOS will format it as FAT16 on first boot."
