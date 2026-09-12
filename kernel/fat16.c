/* fat16.c - a FAT16 filesystem driver: mount, directory listing, file
 * read/write/create/delete, and basic subdirectory support.
 *
 * FAT16 (not a custom format) on purpose: it's a real, fully documented
 * on-disk format, any image this code writes can be verified by
 * mounting it on a normal Linux/macOS/Windows machine (or with
 * mtools/fsck.fat) - which matters a great deal for "get a disk format
 * right the first time" - and the reference layout (boot sector, two
 * FATs, a fixed-size root directory, then the data/cluster area) is
 * unambiguous, unlike inventing one from scratch.
 *
 * This targets the FAT16 variant most tools default to for small
 * volumes: 512-byte sectors, a boot sector + BPB, exactly 2 FAT copies,
 * a fixed root directory, no long filenames (8.3 names only).
 */
#include "kernel.h"

#define SECTOR_SIZE 512
#define FAT16_EOC_MIN 0xFFF8 /* end-of-chain marker range for FAT16 */
#define FAT16_BAD_CLUSTER 0xFFF7
#define FAT16_FREE_CLUSTER 0x0000

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LONG_NAME (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)

/* On-disk boot sector + BIOS Parameter Block, exactly as FAT16 defines
 * it (packed, no padding) so we can read/write it directly as bytes 0-61
 * of sector 0. */
struct fat16_bpb {
	uint8_t  jmp[3];
	uint8_t  oem[8];
	uint16_t bytes_per_sector;
	uint8_t  sectors_per_cluster;
	uint16_t reserved_sector_count;
	uint8_t  num_fats;
	uint16_t root_entry_count;
	uint16_t total_sectors_16;
	uint8_t  media_type;
	uint16_t sectors_per_fat;
	uint16_t sectors_per_track;
	uint16_t num_heads;
	uint32_t hidden_sectors;
	uint32_t total_sectors_32;
	/* extended BPB (FAT12/16) */
	uint8_t  drive_number;
	uint8_t  reserved1;
	uint8_t  boot_signature;
	uint32_t volume_id;
	uint8_t  volume_label[11];
	uint8_t  fs_type[8];
} __attribute__((packed));

/* 32-byte on-disk directory entry (8.3 name, no long-filename entries -
 * we neither generate nor interpret them, so any LFN entries in a
 * directory we didn't create are simply skipped as ATTR_LONG_NAME). */
struct fat16_dirent {
	uint8_t  name[11]; /* 8.3, space-padded, no dot */
	uint8_t  attr;
	uint8_t  reserved;
	uint8_t  create_time_tenths;
	uint16_t create_time;
	uint16_t create_date;
	uint16_t access_date;
	uint16_t first_cluster_hi; /* 0 in real FAT16 (no high word), kept for FAT32 compat */
	uint16_t write_time;
	uint16_t write_date;
	uint16_t first_cluster_lo;
	uint32_t file_size;
} __attribute__((packed));

static struct fat16_bpb bpb;
static bool mounted = false;

static uint32_t fat_start_lba;
static uint32_t root_dir_start_lba;
static uint32_t root_dir_sectors;
static uint32_t data_start_lba;
static uint32_t total_clusters;

/* --- low-level sector helpers --- */

static bool read_sector(uint32_t lba, uint8_t *buf) {
	return ata_read_sector(lba, buf);
}

static bool write_sector(uint32_t lba, const uint8_t *buf) {
	return ata_write_sector(lba, buf);
}

static uint32_t cluster_to_lba(uint16_t cluster) {
	/* clusters are numbered starting at 2; 0 and 1 are reserved */
	return data_start_lba + (uint32_t)(cluster - 2) * bpb.sectors_per_cluster;
}

/* --- FAT table access --- */

static uint16_t fat_read_entry(uint16_t cluster) {
	uint32_t fat_offset = (uint32_t)cluster * 2;
	uint32_t sector = fat_start_lba + (fat_offset / SECTOR_SIZE);
	uint32_t offset_in_sector = fat_offset % SECTOR_SIZE;

	uint8_t buf[SECTOR_SIZE];
	if (!read_sector(sector, buf)) return FAT16_BAD_CLUSTER;

	return (uint16_t)(buf[offset_in_sector] | (buf[offset_in_sector + 1] << 8));
}

static bool fat_write_entry(uint16_t cluster, uint16_t value) {
	uint32_t fat_offset = (uint32_t)cluster * 2;
	uint32_t sector_in_fat = fat_offset / SECTOR_SIZE;
	uint32_t offset_in_sector = fat_offset % SECTOR_SIZE;

	uint8_t buf[SECTOR_SIZE];

	/* FAT16 always keeps exactly num_fats identical copies; write to all
	 * of them so the second copy isn't left stale (some tools/firmware
	 * do consult it if the first looks corrupt). */
	for (uint8_t fat_index = 0; fat_index < bpb.num_fats; fat_index++) {
		uint32_t sector = fat_start_lba + fat_index * bpb.sectors_per_fat + sector_in_fat;
		if (!read_sector(sector, buf)) return false;
		buf[offset_in_sector] = (uint8_t)(value & 0xFF);
		buf[offset_in_sector + 1] = (uint8_t)((value >> 8) & 0xFF);
		if (!write_sector(sector, buf)) return false;
	}
	return true;
}

static uint16_t fat_find_free_cluster(void) {
	/* Clusters 0 and 1 are reserved; valid data clusters start at 2. */
	for (uint16_t c = 2; c < total_clusters + 2; c++) {
		if (fat_read_entry(c) == FAT16_FREE_CLUSTER) return c;
	}
	return 0; /* 0 = no free cluster (never a valid data cluster number) */
}

/* --- mount / format --- */

bool fat16_mount(void) {
	uint8_t sector0[SECTOR_SIZE];
	if (!ata_is_present()) return false;
	if (!read_sector(0, sector0)) return false;

	memcpy(&bpb, sector0, sizeof(bpb));

	/* Sanity-check the BPB rather than trust an arbitrary disk blindly:
	 * a boot sector signature of 0x55AA at bytes 510-511, a plausible
	 * bytes-per-sector, and a non-zero FAT size are all things a real
	 * FAT16 volume always has and garbage/unformatted media won't. */
	bool signature_ok = sector0[510] == 0x55 && sector0[511] == 0xAA;
	bool bps_ok = bpb.bytes_per_sector == SECTOR_SIZE;
	bool fat_size_ok = bpb.sectors_per_fat > 0;
	bool fat_count_ok = bpb.num_fats >= 1 && bpb.num_fats <= 2;

	if (!signature_ok || !bps_ok || !fat_size_ok || !fat_count_ok) {
		mounted = false;
		return false;
	}

	fat_start_lba = bpb.reserved_sector_count;
	root_dir_sectors = ((uint32_t)bpb.root_entry_count * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
	root_dir_start_lba = fat_start_lba + (uint32_t)bpb.num_fats * bpb.sectors_per_fat;
	data_start_lba = root_dir_start_lba + root_dir_sectors;

	uint32_t total_sectors = bpb.total_sectors_16 ? bpb.total_sectors_16 : bpb.total_sectors_32;
	uint32_t data_sectors = total_sectors - data_start_lba;
	total_clusters = bpb.sectors_per_cluster ? data_sectors / bpb.sectors_per_cluster : 0;

	mounted = true;
	return true;
}

bool fat16_is_mounted(void) {
	return mounted;
}

/* Formats the attached disk as a fresh FAT16 volume. Layout chosen for
 * a small (tens-of-MB) disk image: 1 reserved sector, 2 FATs, 512 root
 * entries (the traditional/common FAT16 root size), 4 sectors/cluster
 * (2KB clusters - a reasonable balance of low waste vs. FAT table size
 * for a small volume). disk_sectors is the disk's total sector count
 * (from ATA IDENTIFY, or a caller-supplied estimate). */
bool fat16_format(uint32_t disk_sectors) {
	if (!ata_is_present()) return false;

	memset(&bpb, 0, sizeof(bpb));
	bpb.jmp[0] = 0xEB; bpb.jmp[1] = 0x3C; bpb.jmp[2] = 0x90;
	memcpy(bpb.oem, "AURORAOS", 8);
	bpb.bytes_per_sector = SECTOR_SIZE;
	bpb.sectors_per_cluster = 4;
	bpb.reserved_sector_count = 1;
	bpb.num_fats = 2;
	bpb.root_entry_count = 512;
	bpb.media_type = 0xF8; /* fixed disk */
	bpb.sectors_per_track = 63;
	bpb.num_heads = 16;
	bpb.hidden_sectors = 0;

	if (disk_sectors > 0xFFFF) {
		bpb.total_sectors_16 = 0;
		bpb.total_sectors_32 = disk_sectors;
	} else {
		bpb.total_sectors_16 = (uint16_t)disk_sectors;
		bpb.total_sectors_32 = 0;
	}

	uint32_t root_sectors = ((uint32_t)bpb.root_entry_count * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;

	/* Approximate sectors-per-FAT (FAT16 needs 2 bytes/cluster): solve
	 * for it accounting for the FAT area's own size, then round up. A
	 * couple of correction passes converge this exactly for any
	 * reasonable disk size without needing the full official formula. */
	uint32_t data_sectors_est = disk_sectors - bpb.reserved_sector_count - root_sectors;
	uint32_t clusters_est = data_sectors_est / bpb.sectors_per_cluster;
	uint32_t fat_size = ((clusters_est + 2) * 2 + SECTOR_SIZE - 1) / SECTOR_SIZE;
	for (int i = 0; i < 3; i++) {
		uint32_t data_sectors = disk_sectors - bpb.reserved_sector_count - root_sectors - (uint32_t)bpb.num_fats * fat_size;
		uint32_t clusters = data_sectors / bpb.sectors_per_cluster;
		fat_size = ((clusters + 2) * 2 + SECTOR_SIZE - 1) / SECTOR_SIZE;
	}
	bpb.sectors_per_fat = (uint16_t)fat_size;

	bpb.drive_number = 0x80;
	bpb.boot_signature = 0x29;
	bpb.volume_id = 0xA5A5A5A5;
	memcpy(bpb.volume_label, "AURORAOS   ", 11);
	memcpy(bpb.fs_type, "FAT16   ", 8);

	uint8_t sector0[SECTOR_SIZE];
	memset(sector0, 0, SECTOR_SIZE);
	memcpy(sector0, &bpb, sizeof(bpb));
	sector0[510] = 0x55;
	sector0[511] = 0xAA;
	if (!write_sector(0, sector0)) return false;

	fat_start_lba = bpb.reserved_sector_count;
	root_dir_sectors = root_sectors;
	root_dir_start_lba = fat_start_lba + (uint32_t)bpb.num_fats * bpb.sectors_per_fat;
	data_start_lba = root_dir_start_lba + root_dir_sectors;
	uint32_t data_sectors = disk_sectors - data_start_lba;
	total_clusters = data_sectors / bpb.sectors_per_cluster;

	/* Zero both FAT copies except cluster 0/1's required reserved
	 * entries (media-type byte + all-1s marker, per spec), and zero the
	 * whole root directory region so every entry starts "unused". */
	uint8_t zero_sector[SECTOR_SIZE];
	memset(zero_sector, 0, SECTOR_SIZE);
	for (uint8_t fi = 0; fi < bpb.num_fats; fi++) {
		for (uint32_t s = 0; s < bpb.sectors_per_fat; s++) {
			if (!write_sector(fat_start_lba + fi * bpb.sectors_per_fat + s, zero_sector)) return false;
		}
	}
	for (uint32_t s = 0; s < root_dir_sectors; s++) {
		if (!write_sector(root_dir_start_lba + s, zero_sector)) return false;
	}

	fat_write_entry(0, 0xFF00 | bpb.media_type);
	fat_write_entry(1, 0xFFFF);

	mounted = true;
	return true;
}

/* --- 8.3 filename conversion --- */

/* Converts a plain "NAME.EXT" (case-insensitive, <=8 name / <=3 ext
 * chars) into the on-disk 11-byte space-padded 8.3 form. Returns false
 * for names that don't fit the 8.3 constraints (no long filename
 * support). */
static bool name_to_83(const char *name, uint8_t out11[11]) {
	memset(out11, ' ', 11);

	int i = 0, j = 0;
	while (name[i] && name[i] != '.' && j < 8) {
		char c = name[i];
		if (c >= 'a' && c <= 'z') c -= 32;
		out11[j++] = (uint8_t)c;
		i++;
	}
	if (name[i] && name[i] != '.') return false; /* base name too long */

	while (name[i] && name[i] != '.') i++; /* skip any remaining base chars if too long was already caught */

	if (name[i] == '.') {
		i++;
		int k = 0;
		while (name[i] && k < 3) {
			char c = name[i];
			if (c >= 'a' && c <= 'z') c -= 32;
			out11[8 + k] = (uint8_t)c;
			i++;
			k++;
		}
		if (name[i]) return false; /* extension too long */
	}

	return true;
}

/* Converts an on-disk 11-byte 8.3 name back into "NAME.EXT" (or "NAME"
 * with no dot if there's no extension), null-terminated into out, which
 * must be at least 13 bytes. */
static void name_from_83(const uint8_t in11[11], char *out) {
	int o = 0;
	for (int i = 0; i < 8 && in11[i] != ' '; i++) out[o++] = (char)in11[i];
	if (in11[8] != ' ') {
		out[o++] = '.';
		for (int i = 8; i < 11 && in11[i] != ' '; i++) out[o++] = (char)in11[i];
	}
	out[o] = '\0';
}

/* --- directory iteration ---
 *
 * A "directory cursor" abstracts over the two physically different
 * kinds of directory in FAT16: the root directory is a fixed-size flat
 * array of sectors, while subdirectories are ordinary cluster chains
 * (like files) whose entries happen to be fat16_dirents. Every
 * directory-scanning function below is written once against this
 * cursor instead of twice. */
struct dir_cursor {
	bool is_root;
	uint32_t sector_index;   /* for root: 0..root_dir_sectors-1 */
	uint16_t cluster;        /* for subdirs: current cluster */
	uint32_t sector_in_cluster;
};

static void dir_cursor_init_root(struct dir_cursor *cur) {
	cur->is_root = true;
	cur->sector_index = 0;
	cur->cluster = 0;
	cur->sector_in_cluster = 0;
}

static void dir_cursor_init_cluster(struct dir_cursor *cur, uint16_t first_cluster) {
	cur->is_root = false;
	cur->cluster = first_cluster;
	cur->sector_in_cluster = 0;
}

/* Reads the cursor's current sector into buf and advances the cursor.
 * Returns false once the directory is exhausted. If out_lba is
 * non-NULL, it's set to the LBA the sector was actually read from -
 * callers that need to write back to that exact sector (find_or_alloc_slot)
 * use this instead of trying to reconstruct it from cursor state after
 * the cursor has already moved on. */
static bool dir_cursor_next_sector(struct dir_cursor *cur, uint8_t *buf, uint32_t *out_lba) {
	if (cur->is_root) {
		if (cur->sector_index >= root_dir_sectors) return false;
		uint32_t lba = root_dir_start_lba + cur->sector_index;
		bool ok = read_sector(lba, buf);
		cur->sector_index++;
		if (out_lba) *out_lba = lba;
		return ok;
	}

	if (cur->cluster < 2 || cur->cluster >= FAT16_EOC_MIN) return false;

	uint32_t lba = cluster_to_lba(cur->cluster) + cur->sector_in_cluster;
	bool ok = read_sector(lba, buf);
	if (out_lba) *out_lba = lba;

	cur->sector_in_cluster++;
	if (cur->sector_in_cluster >= bpb.sectors_per_cluster) {
		cur->sector_in_cluster = 0;
		cur->cluster = fat_read_entry(cur->cluster);
	}
	return ok;
}

/* --- public directory entry type: struct fat16_entry is declared in
 * kernel.h since fat16_stat()'s signature is part of the public API --- */

/* Calls callback(entry, userdata) for every in-use entry in the
 * directory starting at first_cluster (0 = root directory). Stops early
 * if callback returns false. */
typedef bool (*dirent_callback)(const struct fat16_entry *entry, void *userdata);

static void walk_directory(uint16_t dir_first_cluster, dirent_callback cb, void *userdata) {
	struct dir_cursor cur;
	if (dir_first_cluster == 0) dir_cursor_init_root(&cur);
	else dir_cursor_init_cluster(&cur, dir_first_cluster);

	uint8_t buf[SECTOR_SIZE];
	while (dir_cursor_next_sector(&cur, buf, NULL)) {
		struct fat16_dirent *entries = (struct fat16_dirent *)buf;
		for (int i = 0; i < SECTOR_SIZE / 32; i++) {
			struct fat16_dirent *e = &entries[i];

			if (e->name[0] == 0x00) return; /* end of directory marker */
			if (e->name[0] == 0xE5) continue; /* deleted entry */
			if ((e->attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) continue; /* LFN entry, skip */
			if (e->attr & ATTR_VOLUME_ID) continue;

			struct fat16_entry out;
			name_from_83(e->name, out.name);
			out.is_dir = (e->attr & ATTR_DIRECTORY) != 0;
			out.size = e->file_size;
			out.first_cluster = e->first_cluster_lo;

			if (!cb(&out, userdata)) return;
		}
	}
}

/* --- listing (fat16_list) --- */

struct list_ctx {
	fat16_list_callback user_cb;
	void *user_data;
};

static bool list_trampoline(const struct fat16_entry *entry, void *userdata) {
	struct list_ctx *ctx = (struct list_ctx *)userdata;
	ctx->user_cb(entry->name, entry->is_dir, entry->size, ctx->user_data);
	return true;
}

void fat16_list(uint16_t dir_cluster, fat16_list_callback cb, void *userdata) {
	if (!mounted) return;
	struct list_ctx ctx = { cb, userdata };
	walk_directory(dir_cluster, list_trampoline, &ctx);
}

/* --- lookup by name within one directory --- */

static int memcmp_local(const void *a, const void *b, size_t n) {
	const uint8_t *pa = a, *pb = b;
	for (size_t i = 0; i < n; i++) {
		if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
	}
	return 0;
}

struct find_ctx {
	const uint8_t *target11;
	bool found;
	struct fat16_entry result;
};

static bool find_trampoline(const struct fat16_entry *entry, void *userdata) {
	struct find_ctx *ctx = (struct find_ctx *)userdata;
	uint8_t entry11[11];
	if (!name_to_83(entry->name, entry11)) return true; /* shouldn't happen for names we wrote, skip defensively */
	if (memcmp_local(entry11, ctx->target11, 11) == 0) {
		ctx->found = true;
		ctx->result = *entry;
		return false; /* stop walking */
	}
	return true;
}

static bool find_in_dir(uint16_t dir_cluster, const char *name, struct fat16_entry *out) {
	uint8_t target11[11];
	if (!name_to_83(name, target11)) return false;

	struct find_ctx ctx;
	ctx.target11 = target11;
	ctx.found = false;
	walk_directory(dir_cluster, find_trampoline, &ctx);

	if (ctx.found && out) *out = ctx.result;
	return ctx.found;
}

bool fat16_stat(uint16_t dir_cluster, const char *name, struct fat16_entry *out) {
	if (!mounted) return false;
	return find_in_dir(dir_cluster, name, out);
}

/* --- cluster chain management --- */

static void free_cluster_chain(uint16_t first_cluster) {
	uint16_t cluster = first_cluster;
	while (cluster >= 2 && cluster < FAT16_EOC_MIN) {
		uint16_t next = fat_read_entry(cluster);
		fat_write_entry(cluster, FAT16_FREE_CLUSTER);
		cluster = next;
	}
}

/* Extends (or starts) a cluster chain by one cluster, linking it onto
 * the end of last_cluster (or starting a fresh chain if last_cluster is
 * 0). Returns the new cluster number, or 0 if the disk is full. */
static uint16_t extend_cluster_chain(uint16_t last_cluster) {
	uint16_t new_cluster = fat_find_free_cluster();
	if (new_cluster == 0) return 0;

	if (!fat_write_entry(new_cluster, FAT16_EOC_MIN)) return 0;
	if (last_cluster >= 2) {
		if (!fat_write_entry(last_cluster, new_cluster)) return 0;
	}

	/* zero the new cluster's data so stale disk contents never leak
	 * into a file/directory that's growing into it */
	uint8_t zero_sector[SECTOR_SIZE];
	memset(zero_sector, 0, SECTOR_SIZE);
	uint32_t lba = cluster_to_lba(new_cluster);
	for (uint8_t s = 0; s < bpb.sectors_per_cluster; s++) {
		if (!write_sector(lba + s, zero_sector)) return 0;
	}

	return new_cluster;
}

/* --- raw directory-entry slot access ---
 *
 * Unlike walk_directory() (read-only, used for listing/lookup), these
 * operate on a specific (sector_lba, index_in_sector) slot so callers
 * can write a new/updated 32-byte dirent in place. */

struct dirent_slot {
	uint32_t sector_lba;
	int index_in_sector; /* 0-15 */
};

/* Finds either an existing entry matching `name`, or (if not found) the
 * first free slot (deleted entry, or the end-of-directory marker) in
 * the directory starting at dir_cluster. For the root directory,
 * dir_cluster is 0. If the directory (a subdirectory; the root can't
 * grow) is completely full, extends it by one more cluster and returns
 * the first slot of that new cluster. Returns false only on a real I/O
 * failure or a full root directory. */
static bool find_or_alloc_slot(uint16_t dir_cluster, const char *name, struct dirent_slot *out_slot, bool *out_exists, struct fat16_dirent *out_existing) {
	uint8_t target11[11];
	if (!name_to_83(name, target11)) return false;

	struct dir_cursor cur;
	if (dir_cluster == 0) dir_cursor_init_root(&cur);
	else dir_cursor_init_cluster(&cur, dir_cluster);

	uint8_t buf[SECTOR_SIZE];
	bool have_free_slot = false;
	struct dirent_slot free_slot = {0, 0};
	/* Tracks the cluster the sector we're about to read belongs to,
	 * captured BEFORE calling dir_cursor_next_sector() (which may
	 * advance cur.cluster to the next cluster as a side effect once the
	 * last sector of the current one has been read) - so this is always
	 * accurate for the sector just processed, unlike trying to recover
	 * it from cur's state afterwards. */
	uint16_t cluster_of_current_sector = dir_cluster;
	uint32_t this_sector_lba;

	while (1) {
		uint16_t cluster_before_read = cur.is_root ? 0 : cur.cluster;
		if (!dir_cursor_next_sector(&cur, buf, &this_sector_lba)) break;
		if (!cur.is_root) cluster_of_current_sector = cluster_before_read;

		struct fat16_dirent *entries = (struct fat16_dirent *)buf;
		for (int i = 0; i < SECTOR_SIZE / 32; i++) {
			struct fat16_dirent *e = &entries[i];

			if (e->name[0] == 0x00) {
				/* end of directory: this and every later slot are free */
				if (!have_free_slot) {
					free_slot.sector_lba = this_sector_lba;
					free_slot.index_in_sector = i;
					have_free_slot = true;
				}
				*out_slot = free_slot;
				*out_exists = false;
				return true;
			}
			if (e->name[0] == 0xE5) {
				if (!have_free_slot) {
					free_slot.sector_lba = this_sector_lba;
					free_slot.index_in_sector = i;
					have_free_slot = true;
				}
				continue;
			}
			if ((e->attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) continue;

			if (memcmp_local(e->name, target11, 11) == 0) {
				out_slot->sector_lba = this_sector_lba;
				out_slot->index_in_sector = i;
				*out_exists = true;
				if (out_existing) *out_existing = *e;
				return true;
			}
		}
	}

	/* Ran off the end without finding a free slot or the target: for a
	 * subdirectory, grow it by one cluster and use its first slot; the
	 * fixed-size root directory has no more room. cluster_of_current_sector
	 * is exact here since the loop above only exits once
	 * dir_cursor_next_sector() returns false, i.e. after the true last
	 * cluster in the chain was fully processed. */
	if (dir_cluster == 0) return false;

	uint16_t new_cluster = extend_cluster_chain(cluster_of_current_sector);
	if (new_cluster == 0) return false;

	out_slot->sector_lba = cluster_to_lba(new_cluster);
	out_slot->index_in_sector = 0;
	*out_exists = false;
	return true;
}

static bool write_dirent_slot(const struct dirent_slot *slot, const struct fat16_dirent *entry) {
	uint8_t buf[SECTOR_SIZE];
	if (!read_sector(slot->sector_lba, buf)) return false;
	memcpy(&buf[slot->index_in_sector * 32], entry, 32);
	return write_sector(slot->sector_lba, buf);
}

/* --- file/directory creation, deletion --- */

bool fat16_create(uint16_t dir_cluster, const char *name, bool as_directory) {
	if (!mounted) return false;

	struct dirent_slot slot;
	bool exists;
	if (!find_or_alloc_slot(dir_cluster, name, &slot, &exists, NULL)) return false;
	if (exists) return true; /* already exists: treat as success, matches touch's semantics */

	uint8_t name11[11];
	if (!name_to_83(name, name11)) return false;

	struct fat16_dirent entry;
	memset(&entry, 0, sizeof(entry));
	memcpy(entry.name, name11, 11);
	entry.attr = as_directory ? ATTR_DIRECTORY : ATTR_ARCHIVE;
	entry.first_cluster_lo = 0;
	entry.file_size = 0;

	if (as_directory) {
		uint16_t new_cluster = extend_cluster_chain(0);
		if (new_cluster == 0) return false;
		entry.first_cluster_lo = new_cluster;
		/* Real FAT directories start with "." and ".." entries; we
		 * don't rely on them anywhere (fat16_list/lookup don't need
		 * self/parent links), so they're intentionally omitted rather
		 * than risk getting their special-casing wrong. */
	}

	return write_dirent_slot(&slot, &entry);
}

bool fat16_delete(uint16_t dir_cluster, const char *name) {
	if (!mounted) return false;

	struct dirent_slot slot;
	bool exists;
	struct fat16_dirent existing;
	if (!find_or_alloc_slot(dir_cluster, name, &slot, &exists, &existing)) return false;
	if (!exists) return false;

	if (existing.first_cluster_lo != 0) {
		free_cluster_chain(existing.first_cluster_lo);
	}

	uint8_t buf[SECTOR_SIZE];
	if (!read_sector(slot.sector_lba, buf)) return false;
	buf[slot.index_in_sector * 32] = 0xE5; /* mark deleted */
	return write_sector(slot.sector_lba, buf);
}

/* --- file data read/write --- */

uint32_t fat16_read_file(uint16_t first_cluster, uint32_t file_size, uint32_t offset, void *out, uint32_t max_len) {
	if (!mounted || first_cluster < 2) return 0;
	if (offset >= file_size) return 0;

	uint32_t to_read = file_size - offset;
	if (to_read > max_len) to_read = max_len;

	uint32_t cluster_size = (uint32_t)bpb.sectors_per_cluster * SECTOR_SIZE;
	uint32_t cluster_index = offset / cluster_size;
	uint32_t offset_in_cluster = offset % cluster_size;

	uint16_t cluster = first_cluster;
	for (uint32_t i = 0; i < cluster_index; i++) {
		cluster = fat_read_entry(cluster);
		if (cluster < 2 || cluster >= FAT16_EOC_MIN) return 0; /* offset beyond the chain: corrupt/truncated */
	}

	uint8_t *dst = (uint8_t *)out;
	uint32_t total_read = 0;

	while (total_read < to_read && cluster >= 2 && cluster < FAT16_EOC_MIN) {
		uint32_t lba = cluster_to_lba(cluster);
		uint8_t sector_buf[SECTOR_SIZE];

		uint32_t sector_in_cluster = offset_in_cluster / SECTOR_SIZE;
		uint32_t byte_in_sector = offset_in_cluster % SECTOR_SIZE;

		for (; sector_in_cluster < bpb.sectors_per_cluster && total_read < to_read; sector_in_cluster++) {
			if (!read_sector(lba + sector_in_cluster, sector_buf)) return total_read;

			uint32_t chunk = SECTOR_SIZE - byte_in_sector;
			if (chunk > to_read - total_read) chunk = to_read - total_read;

			memcpy(dst + total_read, sector_buf + byte_in_sector, chunk);
			total_read += chunk;
			byte_in_sector = 0;
		}

		offset_in_cluster = 0;
		cluster = fat_read_entry(cluster);
	}

	return total_read;
}

/* Overwrites (from offset 0) a file's entire contents with `data`
 * (length `len`), growing or shrinking its cluster chain as needed, and
 * updates its directory entry's size and starting cluster. This is a
 * whole-file rewrite rather than a general read/modify/write at an
 * arbitrary offset, which is all the terminal's `echo >` / editor
 * save-file operations need. */
bool fat16_write_file(uint16_t dir_cluster, const char *name, const void *data, uint32_t len) {
	if (!mounted) return false;

	struct dirent_slot slot;
	bool exists;
	struct fat16_dirent entry;
	if (!find_or_alloc_slot(dir_cluster, name, &slot, &exists, &entry)) return false;

	if (!exists) {
		uint8_t name11[11];
		if (!name_to_83(name, name11)) return false;
		memset(&entry, 0, sizeof(entry));
		memcpy(entry.name, name11, 11);
		entry.attr = ATTR_ARCHIVE;
		entry.first_cluster_lo = 0;
		entry.file_size = 0;
	}

	/* Free the old chain entirely and allocate a fresh one sized for
	 * `len`: simpler and less bug-prone than in-place chain surgery
	 * (splicing to grow, unlinking to shrink) for a first
	 * implementation, at the cost of some extra disk I/O on rewrite. */
	if (entry.first_cluster_lo != 0) {
		free_cluster_chain(entry.first_cluster_lo);
		entry.first_cluster_lo = 0;
	}

	uint32_t cluster_size = (uint32_t)bpb.sectors_per_cluster * SECTOR_SIZE;
	uint32_t clusters_needed = len == 0 ? 0 : (len + cluster_size - 1) / cluster_size;

	uint16_t first_cluster = 0;
	uint16_t last_cluster = 0;
	const uint8_t *src = (const uint8_t *)data;
	uint32_t written = 0;

	for (uint32_t c = 0; c < clusters_needed; c++) {
		uint16_t new_cluster = extend_cluster_chain(last_cluster);
		if (new_cluster == 0) {
			if (first_cluster != 0) free_cluster_chain(first_cluster);
			return false;
		}
		if (first_cluster == 0) first_cluster = new_cluster;
		last_cluster = new_cluster;

		uint32_t lba = cluster_to_lba(new_cluster);
		uint32_t chunk_total = len - written;
		if (chunk_total > cluster_size) chunk_total = cluster_size;

		uint8_t sector_buf[SECTOR_SIZE];
		uint32_t chunk_written = 0;
		for (uint8_t s = 0; s < bpb.sectors_per_cluster && chunk_written < chunk_total; s++) {
			uint32_t this_chunk = chunk_total - chunk_written;
			if (this_chunk > SECTOR_SIZE) this_chunk = SECTOR_SIZE;

			memset(sector_buf, 0, SECTOR_SIZE);
			memcpy(sector_buf, src + written + chunk_written, this_chunk);
			if (!write_sector(lba + s, sector_buf)) return false;
			chunk_written += this_chunk;
		}
		written += chunk_written;
	}

	entry.first_cluster_lo = first_cluster;
	entry.file_size = len;

	return write_dirent_slot(&slot, &entry);
}

