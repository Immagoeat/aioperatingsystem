/* ata.c - ATA PIO disk driver (LBA28).
 *
 * Polling PIO rather than DMA or IRQ-driven: much less machinery
 * (no bus-mastering setup, no scatter-gather lists) at the cost of
 * blocking the CPU during each transfer, which is fine for a single-
 * tasking hobby kernel. Works against QEMU's emulated IDE controller
 * and against real hardware in IDE/legacy-compatibility mode, which is
 * still how BIOS/CSM boot commonly exposes disks - real AHCI-only
 * hardware (no IDE/CSM compatibility mode at all) is out of scope here;
 * that's a genuinely different, much larger driver.
 *
 * Probes all four legacy PATA slots - primary/secondary bus, each with
 * a master and slave position - rather than assuming a fixed one, so
 * this finds a real writable data disk on a wider range of real
 * machines/VM configs than "always primary slave" could. The boot
 * medium (a CD-ROM, ATAPI) is explicitly skipped by its IDENTIFY
 * signature, and the first genuine (non-ATAPI) disk found across all
 * four slots is the one auroraOS's filesystem lives on. QEMU's -cdrom
 * commonly claims primary master or secondary master; this driver
 * doesn't need to know or care which, since it skips ATAPI wherever it
 * finds it and just keeps looking. */
#include "kernel.h"

#define ATA_PRIMARY_IO     0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_REG_DATA(io)      ((uint16_t)((io) + 0))
#define ATA_REG_ERROR(io)     ((uint16_t)((io) + 1))
#define ATA_REG_SECCOUNT0(io) ((uint16_t)((io) + 2))
#define ATA_REG_LBA0(io)      ((uint16_t)((io) + 3))
#define ATA_REG_LBA1(io)      ((uint16_t)((io) + 4))
#define ATA_REG_LBA2(io)      ((uint16_t)((io) + 5))
#define ATA_REG_HDDEVSEL(io)  ((uint16_t)((io) + 6))
#define ATA_REG_COMMAND(io)   ((uint16_t)((io) + 7))
#define ATA_REG_STATUS(io)    ((uint16_t)((io) + 7))

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_CACHE_FLUSH 0xE7

/* The slot actually in use, found by ata_init()'s probe. io/ctrl are
 * the bus's port bases; drive_select_bit is 0 for master, 0x10 for
 * slave (bit 4 of the drive/head register). */
static bool ata_present = false;
static uint32_t ata_total_sectors = 0;
static uint16_t ata_io_base;
static uint16_t ata_ctrl_base;
static uint8_t ata_drive_select_bit;

struct ata_slot { uint16_t io, ctrl; uint8_t drive_select_bit; };
static const struct ata_slot ata_slots[4] = {
	{ ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   0x00 }, /* primary master */
	{ ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   0x10 }, /* primary slave */
	{ ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0x00 }, /* secondary master */
	{ ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, 0x10 }, /* secondary slave */
};

static void ata_400ns_delay(uint16_t ctrl) {
	/* Reading the (unused) alternate status register 4x is the
	 * standard ATA trick for a ~400ns delay to let status settle. */
	for (int i = 0; i < 4; i++) inb(ctrl);
}

static bool ata_wait_not_busy(uint16_t io) {
	uint32_t timeout = 1000000;
	while (timeout--) {
		if (!(inb(ATA_REG_STATUS(io)) & ATA_SR_BSY)) return true;
	}
	return false;
}

static bool ata_wait_drq(uint16_t io) {
	uint32_t timeout = 1000000;
	while (timeout--) {
		uint8_t status = inb(ATA_REG_STATUS(io));
		if (status & ATA_SR_ERR) return false;
		if (status & ATA_SR_DRQ) return true;
	}
	return false;
}

/* Tries IDENTIFY on one (bus, drive) slot. Returns true and fills
 * *out_total_sectors only for a genuine, present, non-ATAPI ATA disk -
 * an absent slot, a timed-out/busy slot, or an ATAPI device (the boot
 * CD-ROM) all correctly return false so the caller just moves on to
 * the next slot. */
static bool ata_try_identify(const struct ata_slot *slot, uint32_t *out_total_sectors) {
	uint16_t io = slot->io;

	outb(ATA_REG_HDDEVSEL(io), (uint8_t)(0xA0 | slot->drive_select_bit));
	ata_400ns_delay(slot->ctrl);

	outb(ATA_REG_SECCOUNT0(io), 0);
	outb(ATA_REG_LBA0(io), 0);
	outb(ATA_REG_LBA1(io), 0);
	outb(ATA_REG_LBA2(io), 0);
	outb(ATA_REG_COMMAND(io), ATA_CMD_IDENTIFY);

	uint8_t status = inb(ATA_REG_STATUS(io));
	if (status == 0) return false; /* nothing wired to this slot at all */

	if (!ata_wait_not_busy(io)) return false;

	/* A non-ATA (e.g. ATAPI) device sets LBA1/LBA2 to a signature other
	 * than 0 during IDENTIFY; treat anything but a plain ATA disk as
	 * absent rather than risk misinterpreting its data - this is what
	 * correctly skips over the boot CD-ROM wherever it's attached. */
	uint8_t lba1 = inb(ATA_REG_LBA1(io));
	uint8_t lba2 = inb(ATA_REG_LBA2(io));
	if (lba1 != 0 || lba2 != 0) return false;

	if (!ata_wait_drq(io)) return false;

	/* Read all 256 IDENTIFY words; the only field we actually need is
	 * words 60-61 (total addressable LBA28 sectors, little-endian
	 * 32-bit), but every word must still be read to drain the data port
	 * before issuing further commands. */
	uint16_t identify[256];
	for (int i = 0; i < 256; i++) identify[i] = inw(ATA_REG_DATA(io));

	*out_total_sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
	return true;
}

bool ata_init(void) {
	ata_present = false;

	for (int i = 0; i < 4; i++) {
		uint32_t total_sectors;
		if (ata_try_identify(&ata_slots[i], &total_sectors)) {
			ata_io_base = ata_slots[i].io;
			ata_ctrl_base = ata_slots[i].ctrl;
			ata_drive_select_bit = ata_slots[i].drive_select_bit;
			ata_total_sectors = total_sectors;
			ata_present = true;
			return true;
		}
	}

	return false;
}

bool ata_is_present(void) {
	return ata_present;
}

uint32_t ata_get_total_sectors(void) {
	return ata_total_sectors;
}

static bool ata_pio_setup(uint32_t lba, uint8_t sector_count) {
	if (!ata_wait_not_busy(ata_io_base)) return false;

	outb(ATA_REG_HDDEVSEL(ata_io_base), (uint8_t)(0xE0 | ata_drive_select_bit | ((lba >> 24) & 0x0F)));
	ata_400ns_delay(ata_ctrl_base);
	outb(ATA_REG_SECCOUNT0(ata_io_base), sector_count);
	outb(ATA_REG_LBA0(ata_io_base), (uint8_t)(lba & 0xFF));
	outb(ATA_REG_LBA1(ata_io_base), (uint8_t)((lba >> 8) & 0xFF));
	outb(ATA_REG_LBA2(ata_io_base), (uint8_t)((lba >> 16) & 0xFF));
	return true;
}

bool ata_read_sector(uint32_t lba, uint8_t *buf512) {
	if (!ata_present) return false;
	if (!ata_pio_setup(lba, 1)) return false;

	outb(ATA_REG_COMMAND(ata_io_base), ATA_CMD_READ_PIO);

	if (!ata_wait_drq(ata_io_base)) return false;

	uint16_t *dst = (uint16_t *)buf512;
	for (int i = 0; i < 256; i++) {
		dst[i] = inw(ATA_REG_DATA(ata_io_base));
	}
	return true;
}

bool ata_write_sector(uint32_t lba, const uint8_t *buf512) {
	if (!ata_present) return false;
	if (!ata_pio_setup(lba, 1)) return false;

	outb(ATA_REG_COMMAND(ata_io_base), ATA_CMD_WRITE_PIO);

	if (!ata_wait_drq(ata_io_base)) return false;

	const uint16_t *src = (const uint16_t *)buf512;
	for (int i = 0; i < 256; i++) {
		outw(ATA_REG_DATA(ata_io_base), src[i]);
	}

	outb(ATA_REG_COMMAND(ata_io_base), ATA_CMD_CACHE_FLUSH);
	ata_wait_not_busy(ata_io_base);

	return true;
}
