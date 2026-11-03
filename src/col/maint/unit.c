/* col/maint/unit.c - independently allocated unit generations for the rewrite experiment. */
#include "postgres.h"
#include "utils/timestamp.h"
#include "col/maint/maint_int.h"
#include "core/eval.h"
#include "core/tomb.h"
#include "core/visibility.h"

#ifdef SIFT_TESTING
bool		sift_test_col_unit_gate = false;
#endif

/* A child owns a complete segment, including dictionaries and liveness; no parent allocation is shared. */
static bool
write_unit(Relation index, SiftColSegment *seg, uint32 u, SiftSegEntry *out)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift unit generation", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	const SiftColRel *r = sift_col_rel_private(index, cx);
	SiftColBatch *b = sift_cbatch_create(cx, seg->schema);
	SiftCPaggSpec spec = r->pagg;
	SiftWriteTarget *wt;
	SiftColSegWriter *w;
	TransactionId stamp = InvalidTransactionId;
	bool written;

	sift_cseg_decode_unit(seg, u, b);
	written = b->nrows > 0;
	if (written)
	{
		wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
		w = sift_colw_begin(wt, sift_store_new_gen(index), seg->schema, r->policy, Max(seg->entry.format, sift_col_format_minor), cx);
		sift_col_pagg_rewrite(index, &seg->entry, 1, &spec);
		sift_colw_set_preagg(w, &spec);
		if (b->keyed)
			sift_colw_set_keyed(w, sift_colk_aim_rows(index), r->ncut > 0 ? r->cut[0] : -1);
		sift_colw_add(w, b);
		sift_colw_finish(w, out);
		(void) sift_seg_xstamp(&seg->entry, &stamp);
		sift_seg_set_xstamp(out, stamp);
		out->flags |= SIFT_SEG_UNIT;
		sift_wt_finish(wt);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return written;
}

/* Caller holds the maintenance lock. The directory reserve bounds this experimental layout. */
void
maint_unit_layout(Relation index, bool strict)
{
	SiftDirSnapshot *snap;
	uint64 roots = 0;

	if (!sift_eval_on(SIFT_EVAL_UNIT_REWRITE))
		return;
	snap = palloc(sizeof(*snap));
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftSegEntry *e = &snap->segs[i];
		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			SiftColSegment *seg = sift_cseg_open(index, e, CurrentMemoryContext);
			roots += seg->nsb;
			sift_cseg_close(seg);
		}
		else
			roots++;
	}
	if (roots > SIFT_MAX_SEGMENTS - 16)
	{
		pfree(snap);
		if (strict)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("unit generation layout exceeds the directory capacity"),
						errhint("Use fewer units or leave unit_rewrite disabled.")));
		return;
	}
	for (int i = 0; i < snap->nsegs; i++)
	{
		SiftSegEntry *e = &snap->segs[i];
		if (e->kind == SIFT_SEG_IMMUTABLE && !(e->flags & SIFT_SEG_UNIT))
		{
			MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift unit layout", ALLOCSET_DEFAULT_SIZES);
			MemoryContext old = MemoryContextSwitchTo(cx);
			SiftColSegment *seg = sift_cseg_open(index, e, cx);
			SiftSegEntry *out = palloc(seg->nsb * sizeof(*out));
			SiftMaintInputs mi = sift_maint_inputs(index, e, 1);
			int n = 0;
			for (uint32 u = 0; u < seg->nsb; u++)
				if (write_unit(index, seg, u, &out[n]))
					n++;
			sift_maint_replace_n(index, mi.gens, 1, out, n, mi.release, mi.nrelease, "published column unit generations of", GetCurrentTimestamp(), e->ndocs);
			sift_cseg_close(seg);
			MemoryContextSwitchTo(old);
			MemoryContextDelete(cx);
		}
	}
	pfree(snap);
}

/* Capture ownership before clearing: a closed TID may have been reused by a row of another unit. */
void
maint_unit_bulkdelete(Relation index, SiftColSegment *seg, IndexBulkDeleteCallback cb, void *cb_state, IndexBulkDeleteResult *stats)
{
	SiftTombLog *log;
	uint64 *bits = NULL, *now;
	uint32 n = 0, owned = 0;
	uint64 live, row_cost, scan;
	SiftSegEntry out;
	SiftMaintInputs mi;
	bool written;

	if (sift_eval_on(SIFT_EVAL_UNIT_REWRITE) && (seg->entry.flags & SIFT_SEG_UNIT) && seg->nsb == 1 &&
		(log = sift_col_log(index)) != NULL && (log->nents > 0 || log->nrecent > 0))
	{
		SiftKey *keys;

		n = seg->sbdir[0].nrows;
		keys = palloc(n * sizeof(*keys));
		bits = palloc(SIFT_COL_NWORDS(n) * sizeof(*bits));
		sift_cseg_unit_keys(seg, 0, keys);
		sift_cseg_live(seg, 0, bits);
		for (uint32 i = 0; i < n; i++)
		{
			int nt;
			const SiftTomb *t;
			bool found = false;

			if (!((bits[i / 64] >> (i % 64)) & 1))
				continue;
			t = sift_tomb_find(log, keys[i], &nt);
			for (int j = 0; j < nt; j++)
				if (t[j].xid != FrozenTransactionId)
				{
					found = true;
					break;
				}
			if (!found)
				bits[i / 64] &= ~(UINT64CONST(1) << (i % 64));
		}
		pfree(keys);
	}
	/* This callback remains the only authority to clear liveness; tombstones retire later in the index pass. */
	sift_cseg_bulkdelete(seg, cb, cb_state, stats);
	if (bits == NULL)
		return;
	now = palloc(SIFT_COL_NWORDS(n) * sizeof(*now));
	sift_cseg_live(seg, 0, now);
	for (uint32 w = 0; w < SIFT_COL_NWORDS(n); w++)
		owned += pg_popcount64(bits[w] & ~now[w]);
	pfree(now);
	pfree(bits);
	if (owned == 0)
		return;
	live = sift_cseg_live_rows(seg);
	row_cost = SIFT_COL_REWRITE_ROW + SIFT_COL_REWRITE_BYTE * seg->entry.bytes / Max(seg->entry.ndocs, 1);
	scan = Min((uint64) owned * SIFT_COL_DEAD_ROW, (uint64) n * SIFT_COL_DEAD_SCAN_ROW);
	if (scan * SIFT_COL_DEAD_SCANS < live * row_cost)
		return;
	mi = sift_maint_inputs(index, &seg->entry, 1);
	written = write_unit(index, seg, 0, &out);
	sift_maint_replace_n(index, mi.gens, 1, &out, written ? 1 : 0, mi.release, mi.nrelease, "rewrote a column unit generation of", GetCurrentTimestamp(), live);
	SIFT_TEST_GATE(sift_test_col_unit_gate, 5);
	pfree(mi.gens);
	pfree(mi.release);
}
