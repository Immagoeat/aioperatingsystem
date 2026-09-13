/* netinfo.c - honest network hardware status for the GUI's network
 * panel (see wm.c).
 *
 * auroraOS has a real driver for exactly one chipset - the Intel e1000
 * (see e1000.c), plus just enough of a stack (net.c) to get a DHCP
 * lease over it. There is still no Wi-Fi (real Wi-Fi needs a driver
 * per chipset family plus firmware blobs plus a WPA supplicant -
 * thousands of lines of work, not something to fake). What this module
 * does: enumerate PCI devices (see pci.c), report whichever
 * network-class controller is really attached, and say honestly
 * whether it's one auroraOS can actually drive - never a fake
 * "Connected" state for hardware with no real driver behind it. */
#include "kernel.h"

#define NETINFO_NAME_MAX 48
#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E

static void local_strncpy_bounded(char *dst, const char *src, size_t n) {
	size_t i = 0;
	for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
	dst[i] = '\0';
}

struct netinfo_result {
	bool found_any;
	bool is_wireless;
	bool driver_supported;
	char name[NETINFO_NAME_MAX];
	uint16_t vendor_id, device_id;
};

static struct netinfo_result scan_result;

static void netinfo_collect(const struct pci_network_device *dev, void *userdata) {
	struct netinfo_result *result = (struct netinfo_result *)userdata;
	if (result->found_any) return; /* report the first one found; see netinfo_get() */

	result->found_any = true;
	result->is_wireless = dev->is_wireless_class;
	result->driver_supported = (dev->vendor_id == E1000_VENDOR_ID && dev->device_id == E1000_DEVICE_ID);
	result->vendor_id = dev->vendor_id;
	result->device_id = dev->device_id;

	if (dev->known_name) {
		local_strncpy_bounded(result->name, dev->known_name, NETINFO_NAME_MAX);
	} else {
		/* Generic fallback: still real information (a network
		 * controller genuinely is present at this vendor:device ID),
		 * just not a chipset auroraOS happens to have a friendly name
		 * for. Format as "0xVVVV:0xDDDD" so it's at least useful for
		 * looking the ID up. */
		const char *hex = "0123456789ABCDEF";
		char buf[NETINFO_NAME_MAX];
		int i = 0;
		buf[i++] = '0'; buf[i++] = 'x';
		for (int shift = 12; shift >= 0; shift -= 4) buf[i++] = hex[(dev->vendor_id >> shift) & 0xF];
		buf[i++] = ':'; buf[i++] = '0'; buf[i++] = 'x';
		for (int shift = 12; shift >= 0; shift -= 4) buf[i++] = hex[(dev->device_id >> shift) & 0xF];
		buf[i] = '\0';
		local_strncpy_bounded(result->name, buf, NETINFO_NAME_MAX);
	}
}

void netinfo_scan(void) {
	memset(&scan_result, 0, sizeof(scan_result));
	pci_find_network_controllers(netinfo_collect, &scan_result);
}

bool netinfo_get(struct netinfo_status *out) {
	out->hardware_found = scan_result.found_any;
	out->is_wireless = scan_result.is_wireless;
	out->driver_supported = scan_result.driver_supported;
	local_strncpy_bounded(out->name, scan_result.found_any ? scan_result.name : "", NETINFO_NAME_MAX);
	return scan_result.found_any;
}
