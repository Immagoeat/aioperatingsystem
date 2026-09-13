/* netinfo.c - honest network hardware status for the GUI's network
 * panel (see wm.c).
 *
 * There is no network stack in auroraOS: no Ethernet driver, no
 * TCP/IP, and certainly no Wi-Fi (real Wi-Fi needs a driver per
 * chipset family plus firmware blobs plus a WPA supplicant - thousands
 * of lines of work, not something to fake). What this module actually
 * does is real: enumerate PCI devices (see pci.c) and report whichever
 * network-class controller, if any, is really attached to this
 * machine/VM. The UI is built to say exactly that - detected hardware,
 * no driver, not connected - rather than a fake "Connected" toggle. */
#include "kernel.h"

#define NETINFO_NAME_MAX 48

static void local_strncpy_bounded(char *dst, const char *src, size_t n) {
	size_t i = 0;
	for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
	dst[i] = '\0';
}

struct netinfo_result {
	bool found_any;
	bool is_wireless;
	char name[NETINFO_NAME_MAX];
	uint16_t vendor_id, device_id;
};

static struct netinfo_result scan_result;

static void netinfo_collect(const struct pci_network_device *dev, void *userdata) {
	struct netinfo_result *result = (struct netinfo_result *)userdata;
	if (result->found_any) return; /* report the first one found; see netinfo_get() */

	result->found_any = true;
	result->is_wireless = dev->is_wireless_class;
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
	local_strncpy_bounded(out->name, scan_result.found_any ? scan_result.name : "", NETINFO_NAME_MAX);
	return scan_result.found_any;
}
