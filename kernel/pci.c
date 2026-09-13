/* pci.c - minimal PCI configuration-space access and bus enumeration.
 *
 * Uses the standard legacy PCI mechanism ("Configuration Mechanism #1"):
 * write a (bus, device, function, register) address to I/O port 0xCF8,
 * then read/write the corresponding 32-bit config-space word through
 * port 0xCFC. Every x86 PC and every PCI-emulating hypervisor (QEMU
 * included) supports this - it predates PCI Express and is still how
 * firmware/OSes probe PCI(e) devices that live below the platform's ECAM
 * region, or when ECAM (MMIO config space) support hasn't been added
 * (it hasn't, here).
 *
 * This is deliberately just enumeration + identification, not a driver
 * framework: it's the real, honest first step toward "detect what
 * network hardware exists" without pretending to have Wi-Fi/Ethernet
 * drivers that don't exist. See netinfo.c for what this feeds into. */
#include "kernel.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_config_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
	return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
	       ((uint32_t)function << 8) | (offset & 0xFC);
}

static uint32_t pci_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
	outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
	return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_read_vendor_id(uint8_t bus, uint8_t device, uint8_t function) {
	return (uint16_t)(pci_read_dword(bus, device, function, 0x00) & 0xFFFF);
}

static uint16_t pci_read_device_id(uint8_t bus, uint8_t device, uint8_t function) {
	return (uint16_t)((pci_read_dword(bus, device, function, 0x00) >> 16) & 0xFFFF);
}

static uint8_t pci_read_header_type(uint8_t bus, uint8_t device, uint8_t function) {
	return (uint8_t)((pci_read_dword(bus, device, function, 0x0C) >> 16) & 0xFF);
}

/* class/subclass live in the top two bytes of the register-0x08 dword */
static uint8_t pci_read_class(uint8_t bus, uint8_t device, uint8_t function) {
	return (uint8_t)((pci_read_dword(bus, device, function, 0x08) >> 24) & 0xFF);
}

static uint8_t pci_read_subclass(uint8_t bus, uint8_t device, uint8_t function) {
	return (uint8_t)((pci_read_dword(bus, device, function, 0x08) >> 16) & 0xFF);
}

#define PCI_CLASS_NETWORK 0x02

/* A small table of vendor/device IDs worth naming by hand: the network
 * controllers QEMU actually emulates (so this is genuinely checkable in
 * this repo's own dev environment), plus a couple of very common real
 * wired-NIC chipsets. Not remotely exhaustive - anything else still
 * shows up correctly, just with a generic "Network controller" label
 * instead of a friendly name. */
struct known_nic { uint16_t vendor, device; const char *name; };
static const struct known_nic known_nics[] = {
	{ 0x8086, 0x100E, "Intel 82540EM (e1000)" },   /* QEMU -device e1000 */
	{ 0x8086, 0x10D3, "Intel 82574L" },
	{ 0x10EC, 0x8139, "Realtek RTL8139" },          /* QEMU -device rtl8139 */
	{ 0x10EC, 0x8168, "Realtek RTL8168/8111" },
	{ 0x1AF4, 0x1000, "VirtIO network device" },    /* QEMU -device virtio-net */
	{ 0x15AD, 0x07B0, "VMware VMXNET3" },
};
#define KNOWN_NIC_COUNT (int)(sizeof(known_nics) / sizeof(known_nics[0]))

static const char *lookup_nic_name(uint16_t vendor, uint16_t device) {
	for (int i = 0; i < KNOWN_NIC_COUNT; i++) {
		if (known_nics[i].vendor == vendor && known_nics[i].device == device) {
			return known_nics[i].name;
		}
	}
	return NULL;
}

void pci_find_network_controllers(pci_device_callback cb, void *userdata) {
	/* Full (bus, device, function) sweep - brute-force, but PCI config
	 * space access is cheap (a couple of port I/O ops) and this only
	 * runs once, so there's no need for the usual "skip absent buses
	 * via bridge discovery" optimization real OSes do. */
	for (int bus = 0; bus < 256; bus++) {
		for (int device = 0; device < 32; device++) {
			uint16_t vendor = pci_read_vendor_id((uint8_t)bus, (uint8_t)device, 0);
			if (vendor == 0xFFFF) continue; /* no device at this slot */

			uint8_t header_type = pci_read_header_type((uint8_t)bus, (uint8_t)device, 0);
			int function_count = (header_type & 0x80) ? 8 : 1; /* bit 7 = multi-function */

			for (int function = 0; function < function_count; function++) {
				uint16_t fn_vendor = pci_read_vendor_id((uint8_t)bus, (uint8_t)device, (uint8_t)function);
				if (fn_vendor == 0xFFFF) continue;

				uint8_t class_code = pci_read_class((uint8_t)bus, (uint8_t)device, (uint8_t)function);
				if (class_code != PCI_CLASS_NETWORK) continue;

				uint16_t dev_id = pci_read_device_id((uint8_t)bus, (uint8_t)device, (uint8_t)function);
				uint8_t subclass = pci_read_subclass((uint8_t)bus, (uint8_t)device, (uint8_t)function);
				const char *name = lookup_nic_name(fn_vendor, dev_id);

				struct pci_network_device info;
				info.bus = (uint8_t)bus;
				info.device = (uint8_t)device;
				info.function = (uint8_t)function;
				info.vendor_id = fn_vendor;
				info.device_id = dev_id;
				/* subclass 0x80 is PCI's generic "Other network
				 * controller" bucket, which in practice is almost
				 * always how Wi-Fi adapters identify themselves
				 * (there's no PCI class code specific to Wi-Fi) */
				info.is_wireless_class = (subclass == 0x80);
				info.known_name = name;

				cb(&info, userdata);
			}
		}
	}
}
