/* ata.c - ATA PIO disk driver (LBA28, primary bus, SLAVE drive).
 *
 * Polling PIO rather than DMA or IRQ-driven: much less machinery
 * (no bus-mastering setup, no scatter-gather lists) at the cost of
 * blocking the CPU during each transfer, which is fine for a single-
 * tasking hobby kernel. Works against QEMU's emulated IDE controller
 * and against real hardware in IDE/legacy-compatibility mode, which is
 * still how BIOS/CSM boot commonly exposes disks.
 *
 * Deliberately targets the primary bus's SLAVE drive, not the master:
 * the boot medium is a CD-ROM (ATAPI) at primary master (QEMU's -cdrom
 * is index=0 by default) or secondary master (QEMU's traditional
 * "second IDE channel" slot, which -cdrom also commonly claims), and
 * this driver is for a separate writable data disk auroraOS's
 * filesystem lives on - attached in QEMU as index=1 (see the Makefile's
 * `run` target and scripts/make_disk.sh, which creates that disk
 * image), which was verified to never collide with where -cdrom puts
 * itself regardless of index=0 vs index=2 conventions.
 */
#include "kernel.h"

#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6

#define ATA_REG_DATA       (ATA_PRIMARY_IO + 0)
#define ATA_REG_ERROR      (ATA_PRIMARY_IO + 1)
#define ATA_REG_SECCOUNT0  (ATA_PRIMARY_IO + 2)
#define ATA_REG_LBA0       (ATA_PRIMARY_IO + 3)
#define ATA_REG_LBA1       (ATA_PRIMARY_IO + 4)
#define ATA_REG_LBA2       (ATA_PRIMARY_IO + 5)
#define ATA_REG_HDDEVSEL   (ATA_PRIMARY_IO + 6)
#define ATA_REG_COMMAND    (ATA_PRIMARY_IO + 7)
#define ATA_REG_STATUS     (ATA_PRIMARY_IO + 7)

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_CACHE_FLUSH 0xE7

static bool ata_present = false;
static uint32_t ata_total_sectors = 0;

static void ata_400ns_delay(void) {
	/* Reading the (unused) alternate status register 4x is the
	 * standard ATA trick for a ~400ns delay to let status settle. */
	for (int i = 0; i < 4; i++) inb(ATA_PRIMARY_CTRL);
}

static bool ata_wait_not_busy(void) {
	uint32_t timeout = 1000000;
	while (timeout--) {
		if (!(inb(ATA_REG_STATUS) & ATA_SR_BSY)) return true;
	}
	return false;
}

static bool ata_wait_drq(void) {
	uint32_t timeout = 1000000;
	while (timeout--) {
		uint8_t status = inb(ATA_REG_STATUS);
		if (status & ATA_SR_ERR) return false;
		if (status & ATA_SR_DRQ) return true;
	}
	return false;
}

bool ata_init(void) {
	/* Select the slave drive (bit 4 of this register: 0=master,
	 * 1=slave), then IDENTIFY. If the status register reads back 0xFF
	 * or never leaves BSY, there's no drive in this slot. */
	outb(ATA_REG_HDDEVSEL, 0xB0);
	ata_400ns_delay();

	outb(ATA_REG_SECCOUNT0, 0);
	outb(ATA_REG_LBA0, 0);
	outb(ATA_REG_LBA1, 0);
	outb(ATA_REG_LBA2, 0);
	outb(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

	uint8_t status = inb(ATA_REG_STATUS);
	if (status == 0) {
		ata_present = false;
		return false;
	}

	if (!ata_wait_not_busy()) {
		ata_present = false;
		return false;
	}

	/* A non-ATA (e.g. ATAPI) device sets LBA1/LBA2 to a signature other
	 * than 0 during IDENTIFY; treat anything but a plain ATA disk as
	 * absent rather than risk misinterpreting its data. */
	uint8_t lba1 = inb(ATA_REG_LBA1);
	uint8_t lba2 = inb(ATA_REG_LBA2);
	if (lba1 != 0 || lba2 != 0) {
		ata_present = false;
		return false;
	}

	if (!ata_wait_drq()) {
		ata_present = false;
		return false;
	}

	/* Read all 256 IDENTIFY words; the only field we actually need is
	 * words 60-61 (total addressable LBA28 sectors, little-endian
	 * 32-bit), but every word must still be read to drain the data port
	 * before issuing further commands. */
	uint16_t identify[256];
	for (int i = 0; i < 256; i++) identify[i] = inw(ATA_REG_DATA);

	ata_total_sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);

	ata_present = true;
	return true;
}

bool ata_is_present(void) {
	return ata_present;
}

uint32_t ata_get_total_sectors(void) {
	return ata_total_sectors;
}

static bool ata_pio_setup(uint32_t lba, uint8_t sector_count) {
	if (!ata_wait_not_busy()) return false;

	outb(ATA_REG_HDDEVSEL, (uint8_t)(0xF0 | ((lba >> 24) & 0x0F)));
	ata_400ns_delay();
	outb(ATA_REG_SECCOUNT0, sector_count);
	outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFF));
	outb(ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFF));
	outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFF));
	return true;
}

bool ata_read_sector(uint32_t lba, uint8_t *buf512) {
	if (!ata_present) return false;
	if (!ata_pio_setup(lba, 1)) return false;

	outb(ATA_REG_COMMAND, ATA_CMD_READ_PIO);

	if (!ata_wait_drq()) return false;

	uint16_t *dst = (uint16_t *)buf512;
	for (int i = 0; i < 256; i++) {
		dst[i] = inw(ATA_REG_DATA);
	}
	return true;
}

bool ata_write_sector(uint32_t lba, const uint8_t *buf512) {
	if (!ata_present) return false;
	if (!ata_pio_setup(lba, 1)) return false;

	outb(ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

	if (!ata_wait_drq()) return false;

	const uint16_t *src = (const uint16_t *)buf512;
	for (int i = 0; i < 256; i++) {
		outw(ATA_REG_DATA, src[i]);
	}

	outb(ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
	ata_wait_not_busy();

	return true;
}
