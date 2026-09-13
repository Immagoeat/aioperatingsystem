/* e1000.c - driver for the Intel 8254x family Ethernet controller
 * (82540EM/82545EM and compatible), the NIC QEMU emulates by default
 * (`-device e1000`, or QEMU's own default NIC when none is specified).
 *
 * This targets exactly the hardware this project can actually verify
 * against (see README's Network section) rather than trying to be a
 * general PCI Ethernet driver. Register layout and descriptor formats
 * are from Intel's public 8254x software developer's manual - every
 * offset/bit referenced below is a real documented register, not a
 * guess.
 *
 * Polling only (no interrupts): simpler, and entirely adequate for the
 * kind of synchronous "send a DHCP request, wait for the reply" usage
 * net.c actually needs - a real OS would use interrupts for a
 * general-purpose stack, but that's more infrastructure than this
 * single-purpose use justifies. */
#include "kernel.h"

/* --- register offsets (byte offsets into the MMIO BAR0 window) --- */
#define REG_CTRL     0x0000 /* Device Control */
#define REG_STATUS   0x0008 /* Device Status */
#define REG_EECD     0x0010 /* EEPROM/Flash Control */
#define REG_EERD     0x0014 /* EEPROM Read */
#define REG_ICR      0x00C0 /* Interrupt Cause Read */
#define REG_IMS      0x00D0 /* Interrupt Mask Set */
#define REG_IMC      0x00D8 /* Interrupt Mask Clear */
#define REG_RCTL     0x0100 /* Receive Control */
#define REG_TCTL     0x0400 /* Transmit Control */
#define REG_TIPG     0x0410 /* Transmit IPG */
#define REG_RDBAL    0x2800 /* RX Descriptor Base Address Low */
#define REG_RDBAH    0x2804 /* RX Descriptor Base Address High */
#define REG_RDLEN    0x2808 /* RX Descriptor Length */
#define REG_RDH      0x2810 /* RX Descriptor Head */
#define REG_RDT      0x2818 /* RX Descriptor Tail */
#define REG_TDBAL    0x3800 /* TX Descriptor Base Address Low */
#define REG_TDBAH    0x3804 /* TX Descriptor Base Address High */
#define REG_TDLEN    0x3808 /* TX Descriptor Length */
#define REG_TDH      0x3810 /* TX Descriptor Head */
#define REG_TDT      0x3818 /* TX Descriptor Tail */
#define REG_RAL0     0x5400 /* Receive Address Low (slot 0) */
#define REG_RAH0     0x5404 /* Receive Address High (slot 0) */

#define CTRL_RESET      (1u << 26)
#define CTRL_SET_LINK_UP (1u << 6)

#define RCTL_EN         (1u << 1)  /* receiver enable */
#define RCTL_BAM        (1u << 15) /* broadcast accept mode */
#define RCTL_BSIZE_2048 (0u << 16) /* buffer size 2048 (with BSEX=0) */
#define RCTL_SECRC      (1u << 26) /* strip Ethernet CRC */

#define TCTL_EN         (1u << 1)  /* transmitter enable */
#define TCTL_PSP        (1u << 3)  /* pad short packets */
#define TCTL_CT_SHIFT   4          /* collision threshold */
#define TCTL_COLD_SHIFT 12         /* collision distance */

#define RX_DESC_COUNT 32
#define TX_DESC_COUNT 8
#define RX_BUFFER_SIZE 2048

/* RX/TX descriptors: exact on-wire layout the hardware DMAs to/from,
 * per the 8254x manual - field order and sizes must match precisely. */
struct e1000_rx_desc {
	uint64_t addr;
	uint16_t length;
	uint16_t checksum;
	uint8_t  status;
	uint8_t  errors;
	uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
	uint64_t addr;
	uint16_t length;
	uint8_t  cso;
	uint8_t  cmd;
	uint8_t  status;
	uint8_t  css;
	uint16_t special;
} __attribute__((packed));

#define RX_STATUS_DD (1u << 0) /* descriptor done */
#define TX_CMD_EOP   (1u << 0) /* end of packet */
#define TX_CMD_RS    (1u << 3) /* report status */
#define TX_STATUS_DD (1u << 0) /* descriptor done */

static volatile uint32_t *mmio_base;
static bool e1000_present = false;
static uint8_t mac_addr[6];

/* Descriptor rings and packet buffers - static/aligned, since this
 * driver only ever supports one card (the one net.c actually needs).
 * 16-byte alignment comfortably satisfies the hardware's descriptor
 * alignment requirement (docs call for 16-byte minimum). */
/* volatile: hardware writes these descriptors' status bytes via DMA,
 * completely outside the compiler's view of program order. Without
 * this, GCC is free to (and at -O2, does) hoist a repeated read like
 * `tx_descs[i].status` in a polling loop out of the loop entirely,
 * reading it once and spinning on a stale cached value forever - the
 * exact bug that made every real send here time out until this was
 * added, confirmed by testing (removing volatile reproduces the
 * hang/failure again). */
static volatile struct e1000_rx_desc rx_descs[RX_DESC_COUNT] __attribute__((aligned(16)));
static volatile struct e1000_tx_desc tx_descs[TX_DESC_COUNT] __attribute__((aligned(16)));
static uint8_t rx_buffers[RX_DESC_COUNT][RX_BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[TX_DESC_COUNT][RX_BUFFER_SIZE] __attribute__((aligned(16)));
static uint16_t rx_tail;
static uint16_t tx_tail;

static uint32_t reg_read(uint32_t offset) {
	return mmio_base[offset / 4];
}

static void reg_write(uint32_t offset, uint32_t value) {
	mmio_base[offset / 4] = value;
}

/* Reads the card's burned-in MAC address via the EEPROM read register
 * (REG_EERD): write the word address + start bit, poll for the "done"
 * bit, read the 16-bit word back out of the same register's upper
 * half. Words 0-2 hold the 6-byte MAC address, low byte first. */
static bool read_mac_from_eeprom(void) {
	for (int word = 0; word < 3; word++) {
		reg_write(REG_EERD, (uint32_t)(word << 8) | 1u); /* address in bits 8-15, start bit 0 */

		uint32_t value = 0;
		bool done = false;
		for (int spin = 0; spin < 100000; spin++) {
			value = reg_read(REG_EERD);
			if (value & (1u << 4)) { done = true; break; } /* done bit */
		}
		if (!done) return false;

		uint16_t data = (uint16_t)(value >> 16);
		mac_addr[word * 2 + 0] = (uint8_t)(data & 0xFF);
		mac_addr[word * 2 + 1] = (uint8_t)(data >> 8);
	}
	return true;
}

static void setup_rx(void) {
	/* safe to discard volatile here specifically: this runs before the
	 * hardware is told the ring even exists (REG_RDBAL/RDT below), so
	 * there's no concurrent DMA writer to race with yet */
	memset((void *)(uintptr_t)rx_descs, 0, sizeof(rx_descs));
	for (int i = 0; i < RX_DESC_COUNT; i++) {
		rx_descs[i].addr = (uint64_t)(uintptr_t)rx_buffers[i];
	}

	uint64_t base = (uint64_t)(uintptr_t)rx_descs;
	reg_write(REG_RDBAL, (uint32_t)(base & 0xFFFFFFFFu));
	reg_write(REG_RDBAH, (uint32_t)(base >> 32));
	reg_write(REG_RDLEN, (uint32_t)sizeof(rx_descs));
	reg_write(REG_RDH, 0);
	reg_write(REG_RDT, RX_DESC_COUNT - 1); /* tail points at the last free descriptor */
	rx_tail = RX_DESC_COUNT - 1;

	reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);
}

static void setup_tx(void) {
	/* same rationale as setup_rx() above: no DMA writer racing us yet */
	memset((void *)(uintptr_t)tx_descs, 0, sizeof(tx_descs));

	uint64_t base = (uint64_t)(uintptr_t)tx_descs;
	reg_write(REG_TDBAL, (uint32_t)(base & 0xFFFFFFFFu));
	reg_write(REG_TDBAH, (uint32_t)(base >> 32));
	reg_write(REG_TDLEN, (uint32_t)sizeof(tx_descs));
	reg_write(REG_TDH, 0);
	reg_write(REG_TDT, 0);
	tx_tail = 0;

	uint32_t tctl = TCTL_EN | TCTL_PSP | (15u << TCTL_CT_SHIFT) | (64u << TCTL_COLD_SHIFT);
	reg_write(REG_TCTL, tctl);
	reg_write(REG_TIPG, 0x0060200A); /* standard IPG timings from the datasheet's example config */
}

/* This driver only targets the one chipset it's named after (see the
 * file comment), so "find the e1000" is just "find a network
 * controller whose vendor:device is Intel 82540EM" rather than a
 * general NIC-selection policy. */
#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E

struct e1000_find_result {
	struct pci_network_device dev;
	bool found;
};

static void e1000_pci_match_callback(const struct pci_network_device *dev, void *userdata) {
	struct e1000_find_result *result = (struct e1000_find_result *)userdata;
	if (result->found) return; /* already have one; ignore any further matches */
	if (dev->vendor_id == E1000_VENDOR_ID && dev->device_id == E1000_DEVICE_ID) {
		result->dev = *dev;
		result->found = true;
	}
}

bool e1000_init(void) {
	struct e1000_find_result result;
	memset(&result, 0, sizeof(result));
	pci_find_network_controllers(e1000_pci_match_callback, &result);
	if (!result.found) return false;

	struct pci_network_device found = result.dev;

	pci_enable_bus_mastering(found.bus, found.device, found.function);
	uint32_t bar0 = pci_read_bar(found.bus, found.device, found.function, 0);
	if (bar0 == 0) return false;

	mmio_base = (volatile uint32_t *)(uintptr_t)bar0;

	/* Reset the device, then wait for it to come back - the reset bit
	 * self-clears once the reset completes, mirroring how every real
	 * e1000 driver brings the card up from a possibly-dirty state.
	 * Time-based (via the PIT tick, already running at this point in
	 * boot) rather than a fixed spin count, so this can't race ahead
	 * of the actual reset completing on a fast host/hypervisor. */
	reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RESET);
	uint32_t reset_deadline = timer_get_ticks() + 5; /* ~50ms at the 100Hz tick rate - generous for a reset that normally completes in microseconds */
	while (timer_get_ticks() < reset_deadline) { }
	while (reg_read(REG_CTRL) & CTRL_RESET) { } /* then confirm the self-clearing bit actually cleared */

	reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SET_LINK_UP);
	reg_write(REG_IMC, 0xFFFFFFFF); /* mask all interrupts - polling only */
	reg_read(REG_ICR); /* clear any pending interrupt causes */

	if (!read_mac_from_eeprom()) return false;

	/* Program the card's own MAC into receive-address slot 0 so it
	 * actually accepts frames addressed to it (not strictly required
	 * for QEMU's default config, but correct per spec and needed on
	 * real hardware/other backends). */
	uint32_t ral = (uint32_t)mac_addr[0] | ((uint32_t)mac_addr[1] << 8) |
	               ((uint32_t)mac_addr[2] << 16) | ((uint32_t)mac_addr[3] << 24);
	uint32_t rah = (uint32_t)mac_addr[4] | ((uint32_t)mac_addr[5] << 8) | (1u << 31); /* AV = address valid */
	reg_write(REG_RAL0, ral);
	reg_write(REG_RAH0, rah);

	setup_rx();
	setup_tx();

	e1000_present = true;
	return true;
}

bool e1000_is_present(void) {
	return e1000_present;
}

void e1000_get_mac(uint8_t out[6]) {
	memcpy(out, mac_addr, 6);
}

bool e1000_send(const void *data, uint16_t len) {
	if (!e1000_present || len > RX_BUFFER_SIZE) return false;

	memcpy(tx_buffers[tx_tail], data, len);

	tx_descs[tx_tail].addr = (uint64_t)(uintptr_t)tx_buffers[tx_tail];
	tx_descs[tx_tail].length = len;
	tx_descs[tx_tail].cmd = TX_CMD_EOP | TX_CMD_RS;
	tx_descs[tx_tail].status = 0;

	uint16_t desc_index = tx_tail;
	tx_tail = (uint16_t)((tx_tail + 1) % TX_DESC_COUNT);
	reg_write(REG_TDT, tx_tail);

	/* poll for completion - fine for the low, synchronous send volume
	 * this driver actually needs (a handful of DHCP/ARP packets) */
	for (int spin = 0; spin < 1000000; spin++) {
		if (tx_descs[desc_index].status & TX_STATUS_DD) return true;
	}
	return false;
}

/* Polls for one received frame, if any is ready. Returns the frame
 * length (copied into `out`, capacity `max_len`), or 0 if nothing has
 * arrived. Non-blocking - callers that need to wait loop on this
 * themselves (see net.c's DHCP exchange, which has its own timeout). */
uint16_t e1000_poll_receive(void *out, uint16_t max_len) {
	if (!e1000_present) return 0;

	uint16_t check_index = (uint16_t)((rx_tail + 1) % RX_DESC_COUNT);
	if (!(rx_descs[check_index].status & RX_STATUS_DD)) return 0;

	uint16_t len = rx_descs[check_index].length;
	if (len > max_len) len = max_len;
	memcpy(out, rx_buffers[check_index], len);

	rx_descs[check_index].status = 0;
	rx_tail = check_index;
	reg_write(REG_RDT, rx_tail);

	return len;
}
