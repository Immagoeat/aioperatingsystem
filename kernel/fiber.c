/* fiber.c - cooperative ("fiber"-style) multitasking: several separate
 * stacks/execution contexts that take turns running, each one running
 * until it explicitly yields back rather than being preempted by a
 * timer interrupt. This is the foundation the "apps out of the kernel"
 * work is being built on (see the plan this was scoped against): before
 * any app's code can move out of wm.c into a separately-loaded program,
 * something has to let more than one program's execution state exist
 * at once, since asm_run() today is fully synchronous - the kernel
 * (and every other window) is frozen for as long as one asm_run() call
 * is on the stack, which is fine for a one-shot `run PROGRAM.bin` but
 * not for a windowed app that needs to keep responding to input while
 * other windows also exist.
 *
 * Deliberately cooperative, not preemptive: switching only ever happens
 * at a fiber_yield() call the running fiber makes itself (directly, or
 * indirectly via a blocking-looking syscall like SYS_WM_POLL_EVENT that
 * yields internally when there's no event yet). No timer-interrupt-
 * driven context switch, no per-fiber kernel stack separate from its
 * user stack, no protection against a fiber that never yields (it just
 * hangs the system the same way a `run`ning program already can today -
 * see asm.c's file comment on this kernel having no isolation by
 * design). That's a real limitation compared to a preemptive scheduler,
 * but matches the honest scope of what a single-CPU, no-paging, hand-
 * rolled hobby kernel can support correctly, and is enough to run
 * several apps' *cooperative* code side by side, taking turns once per
 * WM frame.
 *
 * The actual register-save/stack-swap is hand-written assembly
 * (fiber_switch, in boot.s) - "save these exact registers and point
 * %rsp at a completely different stack" isn't expressible in C. This
 * file is everything above that: the fiber table, creating a fiber's
 * initial stack (so fiber_switch's generic pop-and-ret sequence can
 * "start" a fiber the exact same way it resumes one), and the
 * yield/switch bookkeeping. */
#include "kernel.h"

#define FIBER_MAX 8 /* matches wm.c's MAX_WINDOWS/MAX_APPS - one fiber per resident app, same slot-count convention */
#define FIBER_STACK_SIZE (64 * 1024) /* same size as the kernel's own boot.s stack - no dynamic allocator exists to size this at runtime, see fiber.c's file comment and boot.s's stack_bottom/stack_top */

enum fiber_state {
	FIBER_UNUSED = 0,
	FIBER_READY,   /* not currently running, but eligible to be switched to */
	FIBER_RUNNING, /* currently the one executing (only ever one at a time) */
};

struct fiber {
	enum fiber_state state;
	uint64_t rsp; /* saved stack pointer - valid whenever state != FIBER_RUNNING; see fiber_switch in boot.s */
	struct fiber *resumed_by; /* whoever's fiber_resume() call last switched into this fiber - fiber_yield() switches back to exactly this, not always the main context, since fiber_resume() can itself be called from inside another fiber (not currently done anywhere, but the bookkeeping is correct either way rather than silently assuming it never happens) */
};

static struct fiber fibers[FIBER_MAX];
static int current_fiber = -1; /* index into fibers[], or -1 when running as the original kernel/WM context (fiber_main_context) */

/* The context the kernel/WM was running on before ever switching to a
 * fiber - fiber_switch() needs somewhere to save that state too, the
 * same as any other fiber, so switching back to "not in any fiber" is
 * the exact same mechanism as switching between two fibers rather than
 * a special case. */
static struct fiber main_context;

/* Each fiber's own stack - static, like every other sizable buffer in
 * this kernel (no allocator to get one from at runtime; see this
 * file's top comment and asm.c's exec_buf for the same pattern). */
static uint8_t fiber_stacks[FIBER_MAX][FIBER_STACK_SIZE];

extern void fiber_switch(uint64_t *save_rsp_here, uint64_t new_rsp);

/* Each fiber's entry function, recorded here at fiber_create() time
 * rather than trusted to survive in a register across the pop/ret in
 * fiber_switch and into fiber_trampoline's C prologue (a register
 * value planted on the stack for `pop` to load is only guaranteed to
 * still be in that register for as long as nothing else touches it -
 * fragile to rely on past the very next instruction, let alone across
 * a function's own prologue). Indexed by fiber slot, same as
 * fiber_stacks. */
static void (*fiber_entry_fn[FIBER_MAX])(void);

/* Called (from a fresh fiber's very first resume, via the synthesized
 * stack fiber_create() built) to actually invoke the fiber's entry
 * function - see fiber_create()'s comment for the exact stack shape
 * that lands execution here. Should never actually return - a fiber's
 * entry function is meant to run forever, yielding whenever it has
 * nothing to do - but if one ever does return (a bug, or a deliberate
 * "this app exited"), there's nothing correct to resume on this stack
 * afterward, so it parks the fiber rather than letting execution fall
 * off the end into garbage. Real "app exited, tear down its window"
 * handling is a later stage's job once fibers are actually wired up to
 * real apps - see this file's top comment. */
static void fiber_trampoline(void) {
	int idx = current_fiber;
	fiber_entry_fn[idx]();

	fibers[idx].state = FIBER_UNUSED;
	for (;;) fiber_yield();
}

/* Builds a fresh fiber's initial stack so that switching to it for the
 * first time behaves identically to resuming one that previously
 * yielded - fiber_switch's pop sequence (rbx, rbp, r12, r13, r14, r15,
 * then ret) doesn't know or care whether those values are "real" saved
 * registers or the zeroes planted here; the fiber's actual entry point
 * is looked up by fiber_trampoline from fiber_entry_fn[] instead of
 * being threaded through a register, so the fiber's own code always
 * starts from a clean, well-defined slate. */
static bool fiber_create(int idx, void (*entry)(void)) {
	if (idx < 0 || idx >= FIBER_MAX) return false;

	uint8_t *stack_top = fiber_stacks[idx] + FIBER_STACK_SIZE;
	uint64_t *sp = (uint64_t *)((uintptr_t)stack_top & ~(uintptr_t)0xF); /* 16-byte align, same as any x86-64 call would leave it */

	*(--sp) = (uint64_t)(uintptr_t)fiber_trampoline; /* the "return address" fiber_switch's `ret` will jump to */
	*(--sp) = 0; /* r15 */
	*(--sp) = 0; /* r14 */
	*(--sp) = 0; /* r13 */
	*(--sp) = 0; /* r12 */
	*(--sp) = 0; /* rbp */
	*(--sp) = 0; /* rbx */

	fiber_entry_fn[idx] = entry;
	fibers[idx].rsp = (uint64_t)(uintptr_t)sp;
	fibers[idx].state = FIBER_READY;
	return true;
}

/* Finds a free slot and starts a fiber running `entry` in it. Returns
 * the fiber index (>= 0) on success, or -1 if every slot is in use -
 * the caller (wm.c, once apps are wired to this) is expected to treat
 * that the same as "no free window slot" already is elsewhere. */
int fiber_spawn(void (*entry)(void)) {
	for (int i = 0; i < FIBER_MAX; i++) {
		if (fibers[i].state == FIBER_UNUSED) {
			return fiber_create(i, entry) ? i : -1;
		}
	}
	return -1;
}

/* Switches away from whatever's running now (a fiber, or the original
 * kernel/WM context) to fiber `idx`, and returns once execution comes
 * back here - either because that fiber yielded, or (not yet
 * implemented - see fiber_trampoline's comment) because it exited.
 * Safe to call from the main kernel/WM context OR from inside another
 * fiber, since both cases just save the current state into whichever
 * `struct fiber` represents "here" (main_context, or fibers[current]). */
void fiber_resume(int idx) {
	if (idx < 0 || idx >= FIBER_MAX || fibers[idx].state != FIBER_READY) return;

	struct fiber *from = (current_fiber < 0) ? &main_context : &fibers[current_fiber];
	int prev_fiber = current_fiber;

	fibers[idx].state = FIBER_RUNNING;
	fibers[idx].resumed_by = from;
	current_fiber = idx;

	fiber_switch(&from->rsp, fibers[idx].rsp);

	/* Execution resumes here once `idx` switches back (via
	 * fiber_yield()) - restore whose "current" context this actually
	 * is, since fiber_switch doesn't know about current_fiber, only
	 * raw stack pointers. */
	current_fiber = prev_fiber;
}

/* Called from inside a fiber to hand control back to whichever
 * fiber_resume() call last switched into it (the WM's frame loop, in
 * the intended use - see this file's top comment). Does nothing if
 * called outside any fiber (current_fiber == -1), since there's
 * nothing to yield *to* other than itself in that case. */
void fiber_yield(void) {
	if (current_fiber < 0) return;
	struct fiber *self = &fibers[current_fiber];
	struct fiber *to = self->resumed_by;
	self->state = FIBER_READY;
	fiber_switch(&self->rsp, to->rsp);
}

bool fiber_is_running(void) {
	return current_fiber >= 0;
}
