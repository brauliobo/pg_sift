/*
 * test/unit/agg/win/datebin.c - the pending rows of a peer group of agg/win/datebin.c: the rows held in memory stop where one palloc of their cells ends, the rows past it go to the temp file, and
 * the arrays grow no further than the rows the budget holds.
 */
#include "postgres.h"

#include "agg/win/datebin.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

static SiftWinx *
lags(int nfuncs, uint64 max_pending)
{
	SiftWinxSpec spec = {.n_ord = 1,.nfuncs = nfuncs,.range_frame = true,.max_pending = max_pending};

	for (int f = 0; f < nfuncs; f++)
		spec.func[f] = (SiftWinxFunc) {.kind = SWX_LAG,.arg_type = SVEC_I64,.filter = -1,.offset = 1};
	return sift_winx_create(CurrentMemoryContext, &spec, NULL, NULL, NULL, NULL);
}

static void
pend_one(void *arg)
{
	static const WinxCell cells[SIFT_WINX_MAX_FUNCS];

	winx_pend(arg, 1, 1, cells);
}

/* a budget of 8M rows of 16 cells, more than one palloc of their cells holds: the row past those goes to the temp file */
static void
test_pending_past_max_alloc_spills(void)
{
	SiftWinx   *w = lags(SIFT_WINX_MAX_FUNCS, UINT64CONST(1) << 23);
	const uint64 held = MaxAllocSize / (sizeof(WinxCell) * SIFT_WINX_MAX_FUNCS) + 1;

	w->npend = w->pcap = held;
	SIFT_CHECK(pgshim_try(pend_one, w));
	SIFT_CHECK(w->npend == held && w->nspilled == 1);
	w->npend = w->pcap = 0;
	sift_winx_free(w);
}

static void
test_pending_arrays_stop_at_budget(void)
{
	SiftWinx   *w = lags(2, 1500);

	for (int i = 0; i < 1200; i++)
		SIFT_CHECK(pgshim_try(pend_one, w));
	SIFT_CHECK(w->npend == 1200 && w->pcap <= 1500);
	sift_winx_free(w);
}

int
main(void)
{
	pgshim_keep();
	if (unit_wants("winx_pending_max_alloc"))
		test_pending_past_max_alloc_spills();
	if (unit_wants("winx_pending_budget"))
		test_pending_arrays_stop_at_budget();
	puts("winx: ok");
	return 0;
}
