/*
 * col/f2/scan/verdict.c - what the predicates of a scan say of one row of the stored values (yes, no, or undecided for the heap), and how a row is handed to a sink.
 */
#include "postgres.h"

#include "col/f2/scan/int.h"
#include "core/popcount.h"

Verdict
sift_f2_row_verdict(const SiftColSchema *schema, const F2Scan *s, const ColView *views, const bool *decided, int r)
{
	Verdict		all = V_YES;

	for (int k = 0; k < s->npreds; k++)
	{
		Verdict		v;

		if (decided[k])
			continue;
		v = sift_f2_pred_row(&schema->cols[s->preds[k].col], &s->preds[k], &s->psets[k], &views[s->preds[k].col], r);

		if (v == V_NO)
			return V_NO;
		if (v == V_MAYBE)
			all = V_MAYBE;
	}
	return all;
}

/* A row handed to a sink: decided yes, or undecided for a sink that takes those; any other sink counts only what the stored values decided, so it is an error for it */
void
sift_f2_sink_verdict(const F2Scan *s, Verdict v, uint64 *live, uint64 *maybe, int r)
{
	if (v == V_MAYBE && !s->sink_maybe)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a qual of the aggregate cannot be decided on the stored values"),
						errhint("Run the statement with sift.enable_agg = off: the heap decides such a qual.")));
	live[r >> 6] |= (uint64) (v == V_YES) << (r & 63);
	maybe[r >> 6] |= (uint64) (v == V_MAYBE) << (r & 63);
}

/* A row of a granule that is not decoded: undecided, or yes when the chunks decided every predicate and the sink wants no column (the heap gives the columns of an undecided row) */
Verdict
sift_f2_heap_verdict(const F2Scan *s, const bool *decided)
{
	for (int k = 0; k < s->npreds; k++)
		if (!decided[k])
			return V_MAYBE;
	return s->nsink_leaves == 0 ? V_YES : V_MAYBE;
}

/* The rows a bridge leaves in a granule that the heap decides faster than the columns of the granule decode (a hot heap fetch against a chunk of SIFT_GRANULE_ROWS values) */
bool
sift_f2_scan_few_rows(const uint64 *active, int n)
{
	int			rows = 0;

	for (int w = 0; w < (n + 63) / 64; w++)
		rows += sift_pc64(active[w]);
	return rows <= SIFT_F2_HEAP_DECIDE_ROWS;
}

static int
nth_row(const uint64 *active, int nwords, uint32 k)
{
	for (int w = 0; w < nwords; w++)
	{
		uint64		bits = active[w];
		uint32		c = sift_pc64(bits);

		if (k <= c)
		{
			while (--k > 0)
				bits &= bits - 1;
			return w * 64 + __builtin_ctzll(bits);
		}
		k -= c;
	}
	return -1;
}

/* Keeps the rows of active that the row window of the scan reads (the first or last keep of those of its range); the rows kept */
uint32
sift_f2_scan_window(F2Scan *s, uint64 *active, int n)
{
	SiftF2RowWin *w = &s->rowwin;
	const int	nwords = (n + 63) / 64;
	uint32		total = 0;
	int			cut;

	w->end = w->fwd ? w->hi : w->lo;
	for (int i = 0; i < nwords; i++)
	{
		const int	lo = w->lo - i * 64,
					hi = w->hi - i * 64;

		active[i] &= (lo <= 0 ? ~UINT64CONST(0) : lo >= 64 ? 0 : ~UINT64CONST(0) << lo) & (hi >= 64 ? ~UINT64CONST(0) : hi <= 0 ? 0 : (UINT64CONST(1) << hi) - 1);
		total += sift_pc64(active[i]);
	}
	if (w->keep == 0 || total <= w->keep)
		return total;
	cut = nth_row(active, nwords, w->fwd ? w->keep : total - w->keep + 1);
	for (int i = 0; i < nwords; i++)
	{
		const int	at = cut - i * 64;

		active[i] &= w->fwd ? (at >= 63 ? ~UINT64CONST(0) : at < 0 ? 0 : (UINT64CONST(1) << (at + 1)) - 1) : (at <= 0 ? ~UINT64CONST(0) : at >= 64 ? 0 : ~UINT64CONST(0) << at);
	}
	w->end = w->fwd ? cut + 1 : cut;
	return w->keep;
}

/* One past the last row of active */
int
sift_f2_scan_rows_to(const uint64 *active, int n)
{
	for (int w = (n + 63) / 64 - 1; w >= 0; w--)
		if (active[w] != 0)
			return w * 64 + 64 - __builtin_clzll(active[w]);
	return 0;
}
