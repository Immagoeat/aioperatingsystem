/* apploader.c - stage 2 of "apps out of the kernel": loads a compiled
 * flat binary (see asm.c/docs/ASSEMBLY.md - the same kind of program
 * `run PROGRAM.bin` already executes) from FAT16 into its own code
 * buffer and runs it as a fiber (see fiber.c) with a real window (see
 * wm.c's "Loaded App" section), instead of every app being a C
 * function compiled straight into the kernel.
 *
 * The pieces and how they fit together:
 *
 *  - Loading: apploader_load() reads the file into this slot's own
 *    static code buffer (like asm.c's exec_buf, but one per slot since
 *    several loaded apps can be resident - i.e. have their own window
 *    open - at once) and fiber_spawn()s a small trampoline
 *    (apploader_fiber_entry) that calls into it via the same
 *    asm_run_trampoline used by `run PROGRAM.bin` - reused here rather
 *    than duplicated, since "save callee-saved registers, call the raw
 *    bytes, restore them" is exactly what both need, and reusing it
 *    means a crash inside the loaded code recovers the exact same way
 *    (see idt.c's isr_handler and this file's apploader_program_running
 *    flag below).
 *
 *  - The window: a loaded program has no C runtime/globals of its own
 *    to be handed a struct window through, so it asks for one with the
 *    SYS_WM_CREATE_WINDOW syscall instead (see kernel.h's SYS_WM_*
 *    constants and syscall.c's handlers). Because that syscall has to
 *    run from *inside* the fiber (only the fiber's own code knows what
 *    size window it wants) but the window has to exist *before*
 *    wm.c can ever resume that fiber again through a paint/key/click
 *    callback, apploader_load() resumes the freshly spawned fiber once,
 *    synchronously, right here - far enough for it to reach its own
 *    SYS_WM_CREATE_WINDOW call (which fills in this slot's
 *    pending_w/pending_h and yields straight back) before returning to
 *    the caller (terminal.c's `launch` command), which then creates
 *    the real window from those dimensions. Every later frame, wm.c's
 *    generic paint_apploader()/key_apploader()/etc. (see wm.c) resume
 *    the fiber the normal way.
 *
 *  - Input: since this ISA has no way to pass a pointer to a stack
 *    buffer (no data section, no `lea` - see docs/ASSEMBLY.md's Known
 *    Limitations), an event can't be handed back through an out-
 *    pointer the way a real OS syscall might. SYS_WM_POLL_EVENT
 *    instead returns just the event TYPE, and separate follow-up
 *    syscalls (SYS_WM_EVENT_KEY/X/Y/DELTA) fetch that one event's
 *    fields - the same "stage extra values, fetch them with more
 *    syscalls" shape SYS_GFX_RECT/SYS_GFX_LINE already use for the
 *    same reason. wm.c pushes into apploader_push_event() from its
 *    normal key/click/scroll dispatch, exactly as if the loaded app
 *    were a real window's callbacks.
 *
 *  - Drawing: the existing SYS_GFX_* syscalls (syscall.c) are reused
 *    as-is for a windowed app, not replaced - but syscall.c clips/
 *    offsets their coordinates to the calling app's own window
 *    instead of the full screen, via apploader_current_window_rect()
 *    below, so a loaded app can only ever draw inside its own window
 *    no matter what coordinates it passes. */
#include "kernel.h"

#define APP_SLOTS 8 /* matches wm.c's MAX_WINDOWS/fiber.c's FIBER_MAX - one loaded app per window/fiber slot */
#define APP_CODE_MAX 65536 /* same size as asm.c's exec_buf/CODE_BUF_SIZE - one loaded program per slot, so no sharing needed */

/* APP_CACHE_W/APP_CACHE_H (kernel.h) cap a loaded app's window content
 * much smaller than wm.c's own window-size ceiling: wm.c's compositor
 * repaints the ENTIRE desktop background fresh every single frame with
 * no persistence anywhere (confirmed the hard way - see the git history
 * around this file for the debugging that found this), so a loaded
 * app's drawing would otherwise need to be re-issued in full every
 * frame to stay visible - a real burden given this ISA has no
 * multiplication/shifts and only +-127-byte short jumps
 * (docs/ASSEMBLY.md), making a real per-frame redraw loop hard to fit.
 * Instead, apploader_cache_* below keeps a plain pixel snapshot of this
 * window's content and wm.c's paint_apploader() re-blits it every frame
 * before resuming the app's fiber - so a loaded app really can draw
 * once and it just stays visible, the same way SYS_WM_CREATE_WINDOW's
 * own doc comment always promised. The cap keeps that snapshot's memory
 * bounded (one full APP_CACHE_W x APP_CACHE_H buffer per slot, x
 * APP_SLOTS) - deliberately smaller than a hand-rolled program in this
 * toy ISA is likely to need anyway, and shared with wm.c's resize
 * handling (kernel.h) as the hard ceiling on how big a loaded app's
 * window can be dragged. */

enum app_slot_state {
	APP_SLOT_UNUSED = 0,
	APP_SLOT_AWAITING_WINDOW, /* fiber spawned and resumed once, waiting on this slot's window to actually be created (see this file's top comment) */
	APP_SLOT_RUNNING,         /* window exists; wm.c resumes this slot's fiber each frame */
};

struct app_event {
	int type; /* WM_EVENT_* from kernel.h, or WM_EVENT_NONE for an empty queue slot */
	int key;
	int x, y;
	int delta;
};

#define APP_EVENT_QUEUE_LEN 16 /* generous relative to one key/click per WM frame - a fiber that's fallen behind by more than this is a real bug in that app, not something worth a bigger queue */

struct app_slot {
	enum app_slot_state state;
	int fiber_id;
	int window_id; /* index into wm.c's own windows[] once state == APP_SLOT_RUNNING; -1 until then */
	char filename[32];

	int pending_w, pending_h; /* filled in by SYS_WM_CREATE_WINDOW, read by apploader_load() right after the first resume */
	bool create_window_ok;    /* SYS_WM_CREATE_WINDOW's own return value - false if the app somehow calls it twice or never at all before its first yield */

	struct app_event events[APP_EVENT_QUEUE_LEN];
	int event_head, event_tail; /* ring buffer, same shape as keyboard.c's kbd_buffer */
	struct app_event last_polled; /* the most recent event SYS_WM_POLL_EVENT returned - SYS_WM_EVENT_KEY/X/Y/DELTA read its fields, since the ISA can't pass a struct pointer back (see this file's top comment) */

	uint8_t code[APP_CODE_MAX];
};

static struct app_slot app_slots[APP_SLOTS];

/* Per-slot pixel snapshot of the window's actual content (APP_CACHE_W/
 * APP_CACHE_H's doc comment above has the full story on why this
 * exists) - kept as its own top-level array rather than embedded in
 * struct app_slot, the same way fiber.c keeps fiber_stacks[] separate
 * from struct fiber, so the two megabyte-scale allocations (this and
 * code[]) are each easy to see the size of at a glance. Indexed
 * [slot][y][x], y-major to match how a row is blitted/restored one
 * scanline at a time below. */
static gfx_color_t app_cache[APP_SLOTS][APP_CACHE_H][APP_CACHE_W];

/* A loaded app's window starts out with nothing drawn - fill its cache
 * with a plain neutral background up front (the windowed equivalent of
 * every built-in app implicitly starting from its own COL_WIN_BODY-ish
 * background) so the very first frame (before the app has drawn
 * anything at all) restores something sensible instead of default-
 * zeroed (black) pixels. Called once, from apploader_attach_window(). */
#define APP_CACHE_BG GFX_RGB(0x18, 0x1A, 0x22)
static void apploader_cache_init(int slot, int w, int h) {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			app_cache[slot][y][x] = APP_CACHE_BG;
		}
	}
}

/* Which slot is the fiber currently executing syscalls on behalf of -
 * set by apploader_resume() around every fiber_resume() call so
 * syscall.c knows whose window to clip SYS_GFX_* into and whose event
 * queue SYS_WM_POLL_EVENT reads from. -1 when no loaded app is
 * currently running (a plain `run PROGRAM.bin` program, or the kernel/
 * WM itself) - syscall.c falls back to full-screen/unclipped behavior
 * in that case, unchanged from before this file existed. */
static int current_app_slot = -1;

/* Mirrors asm_program_running/asm_program_crashed (asm.c) for loaded
 * apps - see idt.c's isr_handler, which checks this exactly the same
 * way to recover a crashing loaded app instead of halting the kernel.
 * A single flag (not per-slot) is correct even with several resident
 * apps: the cooperative model means only one fiber is ever actually
 * executing at a time (see fiber.c's file comment), so "is the
 * currently-running thing a loaded app's own code" never needs to
 * distinguish which slot - current_app_slot already answers that. */
bool apploader_program_running = false;
bool apploader_program_crashed = false;

extern void asm_run_trampoline(void *code); /* asm.c/boot.s - reused here, see this file's top comment */

int apploader_current_slot(void) {
	return current_app_slot;
}

int apploader_current_window_id(void) {
	if (current_app_slot < 0) return -1;
	return app_slots[current_app_slot].window_id;
}

static void apploader_reset_slot(struct app_slot *slot) {
	slot->state = APP_SLOT_UNUSED;
	slot->fiber_id = -1;
	slot->window_id = -1;
	slot->filename[0] = '\0';
	slot->pending_w = 0;
	slot->pending_h = 0;
	slot->create_window_ok = false;
	slot->event_head = 0;
	slot->event_tail = 0;
	slot->last_polled.type = WM_EVENT_NONE;
}

/* The fiber's actual entry point (what fiber_spawn() is given) - one
 * shared trampoline for every slot, since fiber_entry_fn (fiber.c) is
 * a plain `void (*)(void)` with nowhere to pass "which slot". Reads
 * current_app_slot instead, which apploader_resume() always sets
 * before switching in - correct because a fiber only ever starts
 * running (for the very first time) from inside apploader_load(),
 * which sets it right before the first fiber_resume() call the same
 * way apploader_resume() does for every later one. */
static void apploader_fiber_entry(void) {
	int slot_idx = current_app_slot;
	struct app_slot *slot = &app_slots[slot_idx];

	apploader_program_crashed = false;
	apploader_program_running = true;
	asm_run_trampoline(slot->code);
	apploader_program_running = false;

	/* Falls through here either because the loaded program executed a
	 * plain `ret` (same "clean exit" path asm_run() itself treats as
	 * exit code 0 - see asm.c), or because idt.c's isr_handler redirected
	 * rip to a ret after a crash (apploader_program_crashed is set in
	 * that case). Either way the app is done: free the slot so its
	 * window/fiber/code buffer can be reused, then let fiber_trampoline
	 * (fiber.c) park this fiber for good - there's nothing left to
	 * resume. wm.c notices window_id's owning slot is UNUSED again next
	 * frame and tears the window down - see paint_apploader(). */
	apploader_reset_slot(slot);
}

/* Loads `filename` from `dir_cluster` into a free slot's code buffer
 * and starts it running far enough to ask for its window (see this
 * file's top comment for the full sequence). Returns the slot index on
 * success, or -1 on any failure (no free slot, file not found/too
 * large, fiber_spawn() out of slots, or the app never called
 * SYS_WM_CREATE_WINDOW before its first yield/exit) - the caller
 * (terminal.c's `launch` command) reports that the same way `run`
 * already reports "no such file" etc. On success, out_w/out_h are
 * the window size the app asked for, ready for the caller to actually
 * create the window with (create_window() itself is wm.c-local, so
 * wm.c does that part, not this file - see wm.c's "Loaded App"
 * section). */
int apploader_load(const char *filename, uint16_t dir_cluster, int *out_w, int *out_h) {
	int idx = -1;
	for (int i = 0; i < APP_SLOTS; i++) {
		if (app_slots[i].state == APP_SLOT_UNUSED) { idx = i; break; }
	}
	if (idx < 0) return -1;

	struct fat16_entry entry;
	if (!fat16_stat(dir_cluster, filename, &entry) || entry.is_dir) return -1;
	if (entry.size == 0 || entry.size > APP_CODE_MAX) return -1;

	struct app_slot *slot = &app_slots[idx];
	apploader_reset_slot(slot);

	uint32_t got = fat16_read_file(entry.first_cluster, entry.size, 0, slot->code, entry.size);
	if (got != entry.size) return -1;

	int i = 0;
	for (; filename[i] && i < (int)sizeof(slot->filename) - 1; i++) slot->filename[i] = filename[i];
	slot->filename[i] = '\0';

	int fiber_id = fiber_spawn(apploader_fiber_entry);
	if (fiber_id < 0) return -1;
	slot->fiber_id = fiber_id;
	slot->state = APP_SLOT_AWAITING_WINDOW;

	/* First resume: runs until the app's own SYS_WM_CREATE_WINDOW call
	 * (which yields right after recording pending_w/pending_h - see
	 * syscall.c) or until it exits/crashes without ever calling it. */
	current_app_slot = idx;
	fiber_resume(fiber_id);
	current_app_slot = -1;

	if (slot->state == APP_SLOT_UNUSED) return -1; /* exited/crashed before creating a window */
	if (!slot->create_window_ok) { apploader_reset_slot(slot); return -1; } /* ran, yielded, but never actually asked for a window - nothing to show */

	*out_w = slot->pending_w;
	*out_h = slot->pending_h;
	return idx;
}

/* Called by wm.c once it's actually created the real struct window for
 * a freshly-loaded slot (see apploader_load()'s doc comment) - records
 * which window slot this app owns, and flips it into the state where
 * apploader_resume() will keep resuming its fiber every frame. */
void apploader_attach_window(int app_slot, int window_id) {
	if (app_slot < 0 || app_slot >= APP_SLOTS) return;
	app_slots[app_slot].window_id = window_id;
	app_slots[app_slot].state = APP_SLOT_RUNNING;
	apploader_cache_init(app_slot, app_slots[app_slot].pending_w, app_slots[app_slot].pending_h);
}

/* Copies `slot`'s current window content (real screen pixels, via
 * gfx_getpixel - see gfx.c) into its cache, and the reverse (cache back
 * onto the real screen, via gfx_putpixel). See APP_CACHE_W/APP_CACHE_H's
 * doc comment for why this round-trip exists at all: it's what makes a
 * loaded app's one-time drawing survive wm.c repainting the desktop
 * background underneath it every frame. wx/wy are the window's
 * on-screen content-area origin (from wm_window_rect()); w/h are
 * clamped to APP_CACHE_W/APP_CACHE_H by whoever set the window's real
 * size in the first place (apploader_syscall_create_window()), so
 * there's nothing left to clamp here. */
static void apploader_cache_snapshot(int slot, int wx, int wy, int w, int h) {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			app_cache[slot][y][x] = gfx_getpixel(wx + x, wy + y);
		}
	}
}

static void apploader_cache_restore(int slot, int wx, int wy, int w, int h) {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			gfx_putpixel(wx + x, wy + y, app_cache[slot][y][x]);
		}
	}
}

/* Resumes `slot`'s fiber for one slice - called once per WM frame from
 * paint_apploader() (see wm.c), the same "runs until it yields or has
 * nothing to do" shape every other fiber_resume() call already has.
 * Does nothing if the slot isn't actually running (already exited -
 * wm.c checks apploader_slot_is_alive() separately to know when to
 * tear the window down, rather than silently no-op forever).
 *
 * Brackets the actual resume with a cache restore-then-snapshot (see
 * APP_CACHE_W/APP_CACHE_H's doc comment): wm.c's own draw_window() has
 * already re-filled this window's body with a plain background color
 * by the time this runs (same as every other window, every frame - see
 * draw_window()), which would otherwise permanently erase anything a
 * loaded app drew on a previous frame and didn't redraw this frame.
 * Restoring the cache first puts last frame's result back, THEN the
 * fiber runs (and may draw more on top, updating things), THEN the
 * result is captured back into the cache for next frame - so a loaded
 * app really can draw once, on its very first resume, and have that
 * keep showing up forever after with no redraw loop of its own. */
void apploader_resume(int slot) {
	if (slot < 0 || slot >= APP_SLOTS || app_slots[slot].state != APP_SLOT_RUNNING) return;

	int wx, wy, ww, wh;
	bool have_rect = wm_window_rect(app_slots[slot].window_id, &wx, &wy, &ww, &wh);
	if (have_rect) apploader_cache_restore(slot, wx, wy, ww, wh);

	current_app_slot = slot;
	fiber_resume(app_slots[slot].fiber_id);
	current_app_slot = -1;

	if (have_rect) apploader_cache_snapshot(slot, wx, wy, ww, wh);
}

bool apploader_slot_is_alive(int slot) {
	if (slot < 0 || slot >= APP_SLOTS) return false;
	return app_slots[slot].state == APP_SLOT_RUNNING;
}

const char *apploader_slot_filename(int slot) {
	if (slot < 0 || slot >= APP_SLOTS) return "";
	return app_slots[slot].filename;
}

/* --- syscall-facing helpers (called from syscall.c's SYS_WM_* cases) --- */

bool apploader_syscall_create_window(int w, int h) {
	if (current_app_slot < 0) return false;
	struct app_slot *slot = &app_slots[current_app_slot];
	if (slot->create_window_ok) return false; /* already created - one window per loaded app, no exceptions */
	if (w < 100) w = 100; /* floors matching wm.c's own smallest real windows, so a loaded app can't ask for something the chrome can't even render */
	if (h < 80) h = 80;
	/* Capped much smaller than a real window's own ceiling would allow
	 * (wm.c's windows can be far bigger) - see APP_CACHE_W/APP_CACHE_H's
	 * comment on why a loaded app's window has its own, tighter cap. */
	if (w > APP_CACHE_W) w = APP_CACHE_W;
	if (h > APP_CACHE_H) h = APP_CACHE_H;
	slot->pending_w = w;
	slot->pending_h = h;
	slot->create_window_ok = true;
	return true;
}

void apploader_push_event(int slot, int type, int key, int x, int y, int delta) {
	if (slot < 0 || slot >= APP_SLOTS) return;
	struct app_slot *s = &app_slots[slot];
	int next = (s->event_tail + 1) % APP_EVENT_QUEUE_LEN;
	if (next == s->event_head) return; /* queue full - drop the event rather than overwrite an unread one; see this file's top comment on APP_EVENT_QUEUE_LEN */
	struct app_event *e = &s->events[s->event_tail];
	e->type = type; e->key = key; e->x = x; e->y = y; e->delta = delta;
	s->event_tail = next;
}

int apploader_syscall_poll_event(void) {
	if (current_app_slot < 0) return WM_EVENT_NONE;
	struct app_slot *slot = &app_slots[current_app_slot];
	if (slot->event_head == slot->event_tail) {
		slot->last_polled.type = WM_EVENT_NONE;
		return WM_EVENT_NONE;
	}
	slot->last_polled = slot->events[slot->event_head];
	slot->event_head = (slot->event_head + 1) % APP_EVENT_QUEUE_LEN;
	return slot->last_polled.type;
}

int apploader_syscall_event_key(void)   { return current_app_slot < 0 ? 0 : app_slots[current_app_slot].last_polled.key; }
int apploader_syscall_event_x(void)     { return current_app_slot < 0 ? 0 : app_slots[current_app_slot].last_polled.x; }
int apploader_syscall_event_y(void)     { return current_app_slot < 0 ? 0 : app_slots[current_app_slot].last_polled.y; }
int apploader_syscall_event_delta(void) { return current_app_slot < 0 ? 0 : app_slots[current_app_slot].last_polled.delta; }
