/* updatecmd.c - a real, if simple, over-the-air-style update mechanism
 * for a kernel that has no network stack to actually receive updates
 * over.
 *
 * The boot medium (auroraos.iso) is a read-only CD-ROM image - nothing
 * running inside auroraOS can rewrite the kernel binary baked into it.
 * What auroraOS CAN do is write to its own writable FAT16 data disk,
 * and GRUB (see scripts/make_iso.sh's grub.cfg) is configured to look
 * for a file named /AURORAOS.UPD on ANY attached disk and boot that
 * instead of the ISO's built-in kernel, if present.
 *
 * So "installing an update" here means: a new kernel binary is first
 * copied onto the FAT16 data disk under a staging name (today, that
 * copy has to happen from the host machine - there's no network or USB
 * mass-storage driver yet to fetch/receive it from inside auroraOS
 * itself), then this command validates it looks like a real Multiboot
 * kernel and installs it as /AURORAOS.UPD, where GRUB will find and
 * boot it next time. This is genuinely how the reboot behaves - it's
 * not a simulation - it's just honest that "receiving" the update file
 * itself isn't automated yet. */
#include "kernel.h"

#define UPDATE_STAGING_NAME "NEWKRNL.BIN"
#define UPDATE_INSTALLED_NAME "AURORAOS.UPD"

/* Multiboot 1 magic (see boot.s) - checking for it at the start of the
 * staged file is a cheap, real sanity check that this is actually a
 * multiboot-bootable kernel and not, say, a text file or a corrupted
 * transfer, before installing it as something GRUB will try to boot. */
#define MULTIBOOT_MAGIC 0x1BADB002u

/* Sized comfortably above the current ~6MB kernel binary (mostly baked-
 * in wallpaper image data) with room to grow, while staying well within
 * the 128MB QEMU hands this kernel by default (see the Makefile's `run`
 * target) - fat16_write_file() has no streaming/chunked mode, so the
 * whole candidate binary genuinely has to fit in memory at once to
 * install it in a single call. */
#define UPDATE_BUFFER_SIZE (12 * 1024 * 1024)
static uint8_t update_buffer[UPDATE_BUFFER_SIZE];

static bool looks_like_multiboot_kernel(const uint8_t *data, uint32_t len) {
	/* the Multiboot header must appear within the first 8KB (per spec)
	 * and is 4-byte aligned */
	uint32_t scan_len = len < 8192 ? len : 8192;
	for (uint32_t i = 0; i + 4 <= scan_len; i += 4) {
		uint32_t word = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
		                ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
		if (word == MULTIBOOT_MAGIC) return true;
	}
	return false;
}

/* Entry point for the shell/terminal's `update` command (wired up in
 * terminal.c's terminal_dispatch()). */
void updatecmd_run(void) {
	if (!fat16_is_mounted()) {
		console_writestring("No filesystem available (no data disk was detected at boot).\n");
		return;
	}

	struct fat16_entry entry;
	if (!fat16_stat(FAT16_ROOT_CLUSTER, UPDATE_STAGING_NAME, &entry) || entry.is_dir) {
		console_writestring("No update staged.\n\n");
		console_writestring("auroraOS has no network/USB driver yet to fetch an update\n");
		console_writestring("itself, so getting one here means copying a new kernel\n");
		console_writestring("binary onto the data disk from the host machine (the same\n");
		console_writestring("way the disk image itself gets created), named exactly:\n");
		console_writestring("  " UPDATE_STAGING_NAME "\n\n");
		console_writestring("Once that file is present at the root of the data disk,\n");
		console_writestring("run `update` again to validate and install it.\n");
		return;
	}

	if (entry.size == 0 || entry.size > UPDATE_BUFFER_SIZE) {
		kprintf("Staged update is %u bytes - refusing (must be > 0 and <= %u bytes).\n",
			entry.size, (uint32_t)UPDATE_BUFFER_SIZE);
		return;
	}

	console_writestring("Reading staged update...\n");
	console_present();
	uint32_t got = fat16_read_file(entry.first_cluster, entry.size, 0, update_buffer, entry.size);
	if (got != entry.size) {
		console_writestring("Read error - update not installed.\n");
		return;
	}

	if (!looks_like_multiboot_kernel(update_buffer, got)) {
		console_writestring("Staged file doesn't look like a bootable auroraOS kernel\n");
		console_writestring("(no Multiboot header found in the first 8KB) - refusing to\n");
		console_writestring("install it. Not touching the current boot configuration.\n");
		return;
	}

	console_writestring("Valid Multiboot kernel - installing...\n");
	console_present();
	if (!fat16_write_file(FAT16_ROOT_CLUSTER, UPDATE_INSTALLED_NAME, update_buffer, got)) {
		console_writestring("Install failed (disk full?) - not touching the current boot configuration.\n");
		return;
	}

	console_set_color(console_color_accent());
	kprintf("Update installed (%u bytes) as %s.\n", got, UPDATE_INSTALLED_NAME);
	console_set_color(console_color_default());
	console_writestring("GRUB will boot it automatically on the next restart - run\n");
	console_writestring("`reboot` now, or continue and reboot whenever you're ready.\n");
}
