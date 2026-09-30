/* login.c - the console UI for auth.c's password gate: first-boot
 * "set a password" flow, and the actual login prompt on every boot
 * after that. Kept separate from auth.c (which only knows about
 * salts/hashes/disk storage, no console_* calls at all) the same way
 * this project splits other logic/rendering pairs (e.g. noteedit.c
 * vs. terminal.c's nano rendering).
 *
 * First boot also brings the network up automatically, right after the
 * password is set (see first_boot_network_setup()) - the same real
 * DHCP exchange the `netconnect` command runs by hand, not a
 * simulation, and not a fetch from any invented "setup server" (this
 * kernel has no update/content server to talk to - see updatecmd.c's
 * file comment on the honest state of that). A machine with no network
 * hardware, or hardware this OS has no driver for, says so plainly and
 * setup continues regardless; nothing about first boot ever blocks on
 * having a network connection. Filesystem creation (fat16_format()) and
 * the "user" (the password itself) already happen automatically before
 * this runs at all - see kernel.c's kernel_main() for the full boot
 * sequence this is one step of. */
#include "kernel.h"

#define LOGIN_MAX_PASSWORD 64

/* Reads a line with each typed character echoed as '*' instead of
 * itself - real security value (not just cosmetic): a password's
 * length and content shouldn't be visible to anyone glancing at the
 * screen while it's typed. */
static void read_password(char *buf, size_t max_len) {
	size_t len = 0;
	for (;;) {
		char c = keyboard_getchar_blocking();
		if (c == '\n') {
			buf[len] = '\0';
			console_putchar('\n');
			console_present();
			return;
		} else if (c == '\b') {
			if (len > 0) { len--; console_putchar('\b'); }
		} else if (c >= 32 && c < 127 && len < max_len - 1) {
			buf[len++] = c;
			console_putchar('*');
		}
		console_present();
	}
}

/* Second half of first-boot setup, after the password is set: bring
 * the network up automatically the same way the `netconnect` command
 * already does by hand (net_init_and_request_lease() - see net.c),
 * rather than leaving a fresh machine with no connectivity until the
 * user discovers that command exists. Honest about the same real
 * limitations netconnect's own output already is: only wired e1000
 * hardware has an actual driver (netinfo.c), and "no network hardware
 * at all" (common - many VMs/machines genuinely have none, or a kind
 * this OS doesn't drive) is reported plainly and setup continues
 * regardless - a fresh install must never get stuck or feel broken
 * just because there's nothing to connect to. */
static void first_boot_network_setup(void) {
	console_writestring("Checking for a network connection...\n");
	console_present();

	netinfo_scan();
	struct netinfo_status status;
	if (!netinfo_get(&status)) {
		console_set_color(console_color_dim());
		console_writestring("No network hardware found - skipping. Run `netconnect` later\n");
		console_writestring("if a network adapter becomes available.\n\n");
		console_set_color(console_color_default());
		return;
	}

	if (!status.driver_supported) {
		console_set_color(console_color_dim());
		console_writestring("Found ");
		console_writestring(status.name);
		console_writestring(", but there's no driver for it yet (only\n");
		console_writestring("wired Intel e1000 is supported) - skipping. Run `netconnect`\n");
		console_writestring("later if that changes.\n\n");
		console_set_color(console_color_default());
		return;
	}

	console_writestring("Found ");
	console_writestring(status.name);
	console_writestring(" - requesting a DHCP lease (this can take a few seconds)...\n");
	console_present();

	if (net_init_and_request_lease()) {
		struct net_status net;
		net_get_status(&net);
		console_set_color(GFX_RGB(0x28, 0xC8, 0x40));
		console_writestring("Connected.\n");
		console_set_color(console_color_default());
		console_writestring("  IP: ");
		console_writestring(net.ip_str);
		console_writestring("\n\n");
	} else {
		console_set_color(console_color_dim());
		console_writestring("Couldn't get a DHCP lease (no cable connected, or no DHCP\n");
		console_writestring("server reachable) - skipping. Run `netconnect` later to retry.\n\n");
		console_set_color(console_color_default());
	}
}

/* First boot (or after AURAUSER.DAT is otherwise missing): require a
 * real password to be set, with confirmation, before the machine can
 * be used at all - rather than silently leaving it unlocked, which
 * would make "password-locked" false for anyone who hasn't explicitly
 * opted out. */
static void first_boot_setup(void) {
	console_set_color(console_color_accent());
	console_writestring("Welcome to auroraOS - set a password to protect this machine.\n");
	console_set_color(console_color_default());

	for (;;) {
		char pw1[LOGIN_MAX_PASSWORD], pw2[LOGIN_MAX_PASSWORD];

		console_writestring("New password: ");
		console_present();
		read_password(pw1, sizeof(pw1));

		if (pw1[0] == '\0') {
			console_writestring("Password can't be empty.\n");
			continue;
		}

		console_writestring("Confirm password: ");
		console_present();
		read_password(pw2, sizeof(pw2));

		if (strcmp(pw1, pw2) != 0) {
			console_writestring("Passwords didn't match - try again.\n");
			continue;
		}

		if (auth_set_password(pw1)) {
			console_set_color(GFX_RGB(0x28, 0xC8, 0x40));
			console_writestring("Password set.\n\n");
			console_set_color(console_color_default());
			first_boot_network_setup();
			return;
		}

		console_writestring("Could not save the password (disk error) - try again.\n");
	}
}

#define LOGIN_MAX_ATTEMPTS_BEFORE_DELAY 3

void login_run(void) {
	if (!fat16_is_mounted()) {
		/* Honest fallback: with no filesystem, there's nowhere to store
		 * a password at all, so the gate can't function - proceeding
		 * unlocked is the only real option (the alternative, an
		 * unremovable lockout with no way to ever set a password on
		 * this machine, would be worse). Say so plainly rather than
		 * silently skipping the login screen. */
		console_set_color(GFX_RGB(0xFF, 0x6B, 0x6B));
		console_writestring("No data disk detected - can't store a password, so the login\n");
		console_writestring("gate is unavailable this boot. Attach a data disk to enable it.\n\n");
		console_set_color(console_color_default());
		return;
	}

	if (!auth_is_configured()) {
		first_boot_setup();
		return;
	}

	int attempts = 0;
	for (;;) {
		console_set_color(console_color_accent());
		console_writestring("auroraOS is locked.\n");
		console_set_color(console_color_default());
		console_writestring("Password: ");
		console_present();

		char password[LOGIN_MAX_PASSWORD];
		read_password(password, sizeof(password));

		if (auth_check_password(password)) {
			console_set_color(GFX_RGB(0x28, 0xC8, 0x40));
			console_writestring("Welcome back.\n\n");
			console_set_color(console_color_default());
			return;
		}

		attempts++;
		console_set_color(GFX_RGB(0xFF, 0x6B, 0x6B));
		console_writestring("Incorrect password.\n");
		console_set_color(console_color_default());

		/* A real (if simple) brute-force slowdown: pause longer after
		 * every few wrong attempts. Not a substitute for a proper
		 * lockout/rate-limiting policy, but a genuine, free deterrent
		 * against a fast automated guessing loop rather than nothing. */
		if (attempts % LOGIN_MAX_ATTEMPTS_BEFORE_DELAY == 0) {
			console_writestring("Too many attempts - waiting...\n");
			console_present();
			timer_wait(300); /* ~3 seconds */
		}
	}
}
