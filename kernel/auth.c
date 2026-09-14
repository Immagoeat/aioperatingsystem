/* auth.c - a real (if simple) password-locked login gate.
 *
 * "Password-locked" only means something if the password itself isn't
 * recoverable from what's stored on disk, so this never writes the
 * plaintext password anywhere: it's salted and hashed with a real
 * cryptographic hash (see sha256.c) before being written to
 * AURAUSER.DAT at the filesystem root, and checked the same way at
 * login - hash the entered password with the stored salt and compare
 * hashes, never compare passwords directly.
 *
 * Two honest limitations worth stating plainly:
 *   - The salt comes from timer ticks + the CMOS RTC, not a hardware
 *     RNG (there isn't one available here). That's adequate for a
 *     salt's actual job - stopping a precomputed rainbow-table lookup
 *     against this file - but it is not cryptographically random, and
 *     this isn't defending against an attacker who can also observe
 *     boot-time state.
 *   - A single-user system: there's one stored credential, not a real
 *     multi-account user database. "User system" here means "the
 *     machine is locked until the right password is entered," not
 *     multiple distinct accounts/permissions.
 *   - Anyone with physical access to the disk (e.g. booting a live USB
 *     and reading the raw FAT16 image) can delete AURAUSER.DAT to
 *     remove the password entirely, the same real limitation every
 *     OS's local-only password gate has without full-disk encryption
 *     (which this project doesn't have either - a genuinely separate,
 *     much larger undertaking). This is a real login gate, not a
 *     defense against someone who already has the disk in hand.
 */
#include "kernel.h"

#define AUTH_FILE_NAME "AURAUSER.DAT"
#define AUTH_SALT_LEN 16
#define AUTH_HASH_LEN 32
#define AUTH_MAX_PASSWORD 64

struct auth_record {
	uint8_t salt[AUTH_SALT_LEN];
	uint8_t hash[AUTH_HASH_LEN]; /* sha256(salt || password) */
} __attribute__((packed));

static void generate_salt(uint8_t out[AUTH_SALT_LEN]) {
	/* Not a hardware RNG (none available - see the file comment): mixes
	 * PIT ticks and CMOS RTC seconds/minutes/hours across calls, which
	 * varies between boots and between the moments this is actually
	 * invoked, adequate for a salt's real job of defeating precomputed
	 * hash lookups against this specific file. */
	struct rtc_time t;
	rtc_get_time(&t);
	uint32_t tick = timer_get_ticks();
	uint32_t mix = tick ^ ((uint32_t)t.hours << 16) ^ ((uint32_t)t.minutes << 8) ^ t.seconds;

	for (int i = 0; i < AUTH_SALT_LEN; i++) {
		/* a small, deliberately simple LCG to spread the mixed seed
		 * across all 16 bytes - not itself claimed to be
		 * cryptographically strong, just a spreading function over
		 * already-mixed entropy */
		mix = mix * 1103515245u + 12345u + (uint32_t)i;
		out[i] = (uint8_t)(mix >> 16);
	}
}

static void hash_password(const uint8_t salt[AUTH_SALT_LEN], const char *password, uint8_t out_hash[AUTH_HASH_LEN]) {
	uint8_t buf[AUTH_SALT_LEN + AUTH_MAX_PASSWORD];
	memcpy(buf, salt, AUTH_SALT_LEN);
	size_t pw_len = strlen(password);
	if (pw_len > AUTH_MAX_PASSWORD) pw_len = AUTH_MAX_PASSWORD;
	memcpy(buf + AUTH_SALT_LEN, password, pw_len);
	sha256(buf, (uint32_t)(AUTH_SALT_LEN + pw_len), out_hash);
}

bool auth_is_configured(void) {
	if (!fat16_is_mounted()) return false;
	struct fat16_entry entry;
	return fat16_stat(FAT16_ROOT_CLUSTER, AUTH_FILE_NAME, &entry) && !entry.is_dir && entry.size == sizeof(struct auth_record);
}

bool auth_set_password(const char *password) {
	if (!fat16_is_mounted() || password[0] == '\0') return false;

	struct auth_record rec;
	generate_salt(rec.salt);
	hash_password(rec.salt, password, rec.hash);

	return fat16_write_file(FAT16_ROOT_CLUSTER, AUTH_FILE_NAME, &rec, sizeof(rec));
}

bool auth_check_password(const char *password) {
	if (!fat16_is_mounted()) return false;

	struct fat16_entry entry;
	if (!fat16_stat(FAT16_ROOT_CLUSTER, AUTH_FILE_NAME, &entry) || entry.is_dir) return false;
	if (entry.size != sizeof(struct auth_record)) return false;

	struct auth_record rec;
	uint32_t got = fat16_read_file(entry.first_cluster, entry.size, 0, &rec, sizeof(rec));
	if (got != sizeof(rec)) return false;

	uint8_t computed_hash[AUTH_HASH_LEN];
	hash_password(rec.salt, password, computed_hash);

	/* constant-time-ish comparison: always checks every byte rather
	 * than returning on the first mismatch, so a timing side-channel
	 * can't leak how many leading bytes matched. Not a complete defense
	 * (this kernel's execution has plenty of other timing variance),
	 * but a real, free improvement over the obvious short-circuiting
	 * loop, so there's no reason not to do it. */
	uint8_t diff = 0;
	for (int i = 0; i < AUTH_HASH_LEN; i++) diff |= (uint8_t)(computed_hash[i] ^ rec.hash[i]);
	return diff == 0;
}
