#include "kernel.h"

struct multiboot_info {
	uint32_t flags;
	uint32_t mem_lower;
	uint32_t mem_upper;
	uint32_t boot_device;
	uint32_t cmdline;
	uint32_t mods_count;
	uint32_t mods_addr;
	uint32_t syms[4];
	uint32_t mmap_length;
	uint32_t mmap_addr;
} __attribute__((packed));

struct mmap_entry {
	uint32_t size;
	uint64_t addr;
	uint64_t len;
	uint32_t type;
} __attribute__((packed));

static struct multiboot_info *mbi = 0;

void memory_init(uint32_t mb_info_addr) {
	mbi = (struct multiboot_info *)mb_info_addr;
}

uint32_t memory_total_kb(void) {
	if (!mbi) return 0;
	return mbi->mem_lower + mbi->mem_upper;
}

void memory_print_map(void) {
	if (!mbi) {
		kprintf("No multiboot memory info available.\n");
		return;
	}

	kprintf("Lower memory: %u KB\n", mbi->mem_lower);
	kprintf("Upper memory: %u KB\n", mbi->mem_upper);
	kprintf("Total usable: %u KB (~%u MB)\n", memory_total_kb(), memory_total_kb() / 1024);

	if (mbi->flags & (1 << 6)) {
		kprintf("\nMemory map:\n");
		struct mmap_entry *entry = (struct mmap_entry *)mbi->mmap_addr;
		uint32_t end = mbi->mmap_addr + mbi->mmap_length;
		while ((uint32_t)entry < end) {
			kprintf("  addr=0x%x len=0x%x type=%d\n",
				(uint32_t)entry->addr, (uint32_t)entry->len, entry->type);
			entry = (struct mmap_entry *)((uint32_t)entry + entry->size + 4);
		}
	}
}
