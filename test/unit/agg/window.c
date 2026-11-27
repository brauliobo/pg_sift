/*
 * test/unit/agg/window.c - the ring of held rows of agg/window.c: it grows while one palloc holds it and work_mem has room, and spills its rows past that, at any work_mem.
 */
#include "postgres.h"

#include "agg/window.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

static SiftWindowExec *
range_window(uint64 max_bytes)
{
	SiftWindowSpec spec = {.mode = SWIN_MODE_WINDOW,.n_part = 1,.n_ord = 1,.n_funcs = 1,.limit = SIFT_WIN_NO_LIMIT};

	spec.funcs[0] = (SiftWinFuncSpec) {.kind = SWIN_COUNT_STAR,.range_frame = true};
	return sift_window_create(CurrentMemoryContext, &spec, max_bytes, NULL, NULL, NULL, NULL, NULL);
}

static void
grow(void *arg)
{
	grow_ring(arg);
}

/* a ring of cap rows (none held) at a work_mem of 16 GB: grows to twice cap, or spills */
static bool
grow_at(uint32 cap, uint32 *cap_after)
{
	SiftWindowExec *w = range_window((uint64) 4 << 30);
	const uint32 cap0 = w->cap;
	bool		ok;

	w->cap = cap;
	ok = pgshim_try(grow, w);
	*cap_after = w->cap;
	if (w->cap == cap)
		w->cap = cap0;
	sift_window_free(w);
	return ok;
}

static void
test_ring_grows(void)
{
	uint32		cap;

	SIFT_CHECK(grow_at(64, &cap) && cap == 128);
}

static void
test_ring_past_max_alloc_spills(void)
{
	uint32		cap;

	SIFT_CHECK((uint64) 2 * (1U << 22) * sizeof(SiftWinRow) > MaxAllocSize);
	SIFT_CHECK(grow_at(1U << 22, &cap));
	SIFT_CHECK(cap == 1U << 22);
	SIFT_CHECK(grow_at(1U << 31, &cap));
	SIFT_CHECK(cap == 1U << 31);
}

int
main(void)
{
	pgshim_keep();
	if (unit_wants("window_ring_grows"))
		test_ring_grows();
	if (unit_wants("window_ring_max_alloc"))
		test_ring_past_max_alloc_spills();
	puts("window: ok");
	return 0;
}
