/*
 * table/freeze.c - freezing of the groups with unfrozen rows, the rows of aborted writers, the live rows, and the merges of the column segments
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/skey.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/pg.h"
#include "col/seg.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "bm25/merge.h"
#include "core/ops.h"
#include "table/table.h"
#include "table/seg_int.h"

/* ---------- freezing, the rows of aborted writers, the live rows ---------- */

/* An insertion that is final for every snapshot: committed before oldest (the last answer is remembered) */
typedef struct XidFinal
{
	TransactionId oldest;
	TransactionId xid;
	bool		final;
} XidFinal;

static bool
xid_final(XidFinal *m, TransactionId xid)
{
	if (m->xid != xid || !TransactionIdIsValid(m->xid))
	{
		m->xid = xid;
		m->final = TransactionIdPrecedes(xid, m->oldest) && TransactionIdDidCommit(xid);
	}
	return m->final;
}

static bool
merge_freeze(void *arg, uint32 xmin, uint32 cmin)
{
	return xid_final(arg, xmin);
}

/* Does the unit have a live row (a group scan reads the liveness bits only of a unit with a dead one) */
static bool
unit_has_live(SiftColSegment *seg, uint32 u, uint64 *live)
{
	if (!sift_cseg_dirty(seg, u))
		return true;
	sift_cseg_live(seg, u, live);
	for (uint32 w = 0; w < SIFT_COL_NWORDS(seg->sbdir[u].nrows); w++)
		if (live[w])
			return true;
	return false;
}

/* Are all the live rows of the group committed before oldest (a group with a newer xid is not read further than its range) */
static bool
group_final(Relation rel, const SiftSegEntry *e, TransactionId oldest)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table freeze check", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColSegment *seg = sift_cseg_open(rel, e, cx);
	XidFinal	m = {.oldest = oldest};
	uint32	   *xm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32)),
			   *cm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	uint32		lo,
				hi;
	bool		final;

	sift_cseg_ver_range(seg, &lo, &hi);
	final = TransactionIdPrecedes(hi, oldest);
	for (uint32 u = 0; u < seg->nsb && final; u++)
	{
		SiftColVerSum sum;

		sift_cseg_ver_sum(seg, u, &sum);
		if (!sum.any || !unit_has_live(seg, u, live))
			continue;
		if (!sum.frozen_rows && sum.xmin_lo == sum.xmin_hi)
		{
			final = xid_final(&m, sum.xmin_lo);
			continue;
		}
		sift_cseg_ver_unit(seg, u, xm, cm);
		sift_cseg_live(seg, u, live);
		for (uint32 i = 0; i < seg->sbdir[u].nrows && final; i++)
			if (xm[i] != 0 && (live[i / 64] >> (i % 64) & 1))
				final = xid_final(&m, xm[i]);
		CHECK_FOR_INTERRUPTS();
	}
	sift_cseg_close(seg);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return final;
}

typedef struct FreezeArg
{
	const uint64 *gens;
	int			n;
} FreezeArg;

static void
freeze_groups_swap(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	const FreezeArg *a = arg;

	for (int i = 0; i < dir->nsegs; i++)
		for (int k = 0; k < a->n; k++)
			if (dir->segs[i].gen == a->gens[k])
			{
				if (dir->segs[i].format < SIFT_COL_FORMAT_VERS)
					dir->segs[i].reserved = 0;
				dir->segs[i].flags &= ~SIFT_SEG_VERS;
			}
}

int
sift_tbl_freeze_groups(Relation rel, SiftTblRel *st, TransactionId oldest)
{
	uint64		gens[SIFT_MAX_SEGMENTS];
	int			n = 0;

	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];

		if (sift_tbl_seg_unfrozen(e) && group_final(rel, e, oldest))
			gens[n++] = e->gen;
	}
	if (n == 0)
		return 0;
	sift_dir_update(rel, &(SiftDirChange) {.fn = freeze_groups_swap,.arg = &(FreezeArg) {gens, n},.extra = InvalidBuffer});
	sift_tbl_refresh(rel, st);
	return n;
}

static void
runs_add(SiftTblKeyRuns *r, SiftKey lo, SiftKey hi)
{
	if (r->n == r->cap)
	{
		r->cap = Max(64, r->cap * 2);
		r->run = r->run ? repalloc_huge(r->run, r->cap * sizeof(SiftKey[2])) : MemoryContextAlloc(r->mcxt, r->cap * sizeof(SiftKey[2]));
	}
	r->run[r->n][0] = lo;
	r->run[r->n++][1] = hi;
}

/* The live rows of unit u whose writer aborted, as runs of consecutive rows; false when it has none */
static bool
unit_aborted_runs(SiftColSegment *seg, uint32 u, SiftTblKeyRuns *runs, uint32 *xm, uint32 *cm, uint64 *live, SiftKey *keys)
{
	uint32		n = seg->sbdir[u].nrows;
	TransactionId memo_xid = InvalidTransactionId;
	bool		memo_aborted = false,
				any = false,
				open = false;
	SiftKey		lo = 0,
				hi = 0;

	sift_cseg_ver_unit(seg, u, xm, cm);
	sift_cseg_live(seg, u, live);
	for (uint32 i = 0; i < n; i++)
	{
		bool		aborted;

		if (!(live[i / 64] >> (i % 64) & 1))
			continue;
		if (xm[i] != memo_xid)
		{
			memo_xid = xm[i];
			memo_aborted = TransactionIdIsNormal(xm[i]) && sift_tbl_xid_aborted(xm[i]);
		}
		aborted = memo_aborted;
		if (aborted && !any)
		{
			sift_cseg_unit_keys(seg, u, keys);
			any = true;
		}
		if (aborted)
		{
			if (!open)
				lo = keys[i];
			hi = keys[i];
			open = true;
		}
		else if (open)
		{
			runs_add(runs, lo, hi);
			open = false;
		}
	}
	if (open)
		runs_add(runs, lo, hi);
	return any;
}

void
sift_tbl_aborted_runs(Relation rel, SiftTblRel *st, SiftTblKeyRuns *runs, uint32 *sb_lo, uint32 *sb_hi)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table aborted rows", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	uint32	   *xm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32)),
			   *cm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	SiftKey    *keys = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(SiftKey));

	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];
		SiftColSegment *seg;

		if (!sift_tbl_seg_unfrozen(e))
			continue;
		seg = sift_cseg_open(rel, e, cx);
		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftColVerSum sum;

			sift_cseg_ver_sum(seg, u, &sum);
			if (!sum.any)
				continue;
			if (unit_aborted_runs(seg, u, runs, xm, cm, live, keys))
			{
				*sb_lo = Min(*sb_lo, seg->sbdir[u].sb);
				*sb_hi = Max(*sb_hi, seg->sbdir[u].sb);
			}
			CHECK_FOR_INTERRUPTS();
		}
		sift_cseg_close(seg);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

int
sift_tbl_drop_dead_groups(Relation rel, SiftTblRel *st)
{
	SiftSegEntry gone[SIFT_MAX_SEGMENTS];
	int			n = 0;
	SiftMaintInputs mi;

	sift_tbl_refresh(rel, st);
	for (int i = 0; i < st->dir.nsegs; i++)
		if (sift_tbl_seg_unfrozen(&st->dir.segs[i]))
		{
			SiftColSegment *seg = sift_cseg_open(rel, &st->dir.segs[i], CurrentMemoryContext);

			if (sift_cseg_live_rows(seg) == 0)
				gone[n++] = st->dir.segs[i];
			sift_cseg_close(seg);
		}
	if (n == 0)
		return 0;
	mi = sift_maint_inputs(rel, gone, n);
	sift_maint_replace(rel, mi.gens, n, NULL, mi.release, mi.nrelease, psprintf("dropped %d column segments without a live row of", n), GetCurrentTimestamp(), 0);
	sift_tbl_refresh(rel, st);
	return n;
}

/* The live rows of a group with unfrozen rows that count: inserted by a transaction that committed or is this one */
static double
unfrozen_live_rows(Relation rel, const SiftSegEntry *e)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table live rows", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColSegment *seg = sift_cseg_open(rel, e, cx);
	uint32	   *xm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32)),
			   *cm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	double		n = 0;
	TransactionId memo_xid = InvalidTransactionId;
	bool		memo_counts = true;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftColVerSum sum;
		uint32		rows = seg->sbdir[u].nrows;

		sift_cseg_ver_sum(seg, u, &sum);
		sift_cseg_live(seg, u, live);
		if (sum.any)
			sift_cseg_ver_unit(seg, u, xm, cm);
		for (uint32 i = 0; i < rows; i++)
		{
			if (!(live[i / 64] >> (i % 64) & 1))
				continue;
			if (sum.any && xm[i] != memo_xid)
			{
				memo_xid = xm[i];
				memo_counts = !TransactionIdIsNormal(xm[i]) || TransactionIdIsCurrentTransactionId(xm[i]) || TransactionIdDidCommit(xm[i]);
			}
			n += !sum.any || memo_counts;
		}
		CHECK_FOR_INTERRUPTS();
	}
	sift_cseg_close(seg);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return n;
}

double
sift_tbl_group_live_rows(Relation rel, SiftTblRel *st)
{
	double		n = 0;

	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];

		if (e->kind != SIFT_SEG_IMMUTABLE)
			continue;
		if (sift_tbl_seg_unfrozen(e))
			n += unfrozen_live_rows(rel, e);
		else
		{
			SiftColSegment *seg = sift_cseg_open(rel, e, CurrentMemoryContext);

			n += sift_cseg_live_rows(seg);
			sift_cseg_close(seg);
		}
	}
	return n;
}

int64
sift_tbl_group_bulkdelete(Relation rel, SiftTblRel *st, IndexBulkDeleteCallback cb, void *state, uint32 sb_lo, uint32 sb_hi)
{
	IndexBulkDeleteResult stats = {0};

	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];
		SiftColSegment *seg;

		if (e->kind != SIFT_SEG_IMMUTABLE || e->sb_hi < sb_lo || e->sb_lo > sb_hi)
			continue;
		seg = sift_cseg_open(rel, e, CurrentMemoryContext);
		sift_cseg_bulkdelete(seg, cb, state, &stats);
		sift_cseg_close(seg);
	}
	return stats.tuples_removed;
}

/* The smallest groups when the directory is filling and the policy waits for more of a size (the least a merge frees: fanin - 1 entries) */
static int
merge_smallest(Relation rel, SiftTblRel *st, SiftSegEntry *sel)
{
	const SiftTblLayout *l = layout_of(rel, st);
	SiftSegEntry *cand = palloc(Max(st->dir.nsegs, 1) * sizeof(SiftSegEntry));
	int			n = 0;

	for (int i = 0; i < st->dir.nsegs; i++)
		if (st->dir.segs[i].kind == SIFT_SEG_IMMUTABLE && schema_of(rel, st, &st->dir.segs[i])->hash == l->schema->hash)
			cand[n++] = st->dir.segs[i];
	if (n >= 2)
	{
		qsort(cand, n, sizeof(SiftSegEntry), sift_seg_bytes_cmp);
		n = Min(n, SIFT_MERGE_FANIN);
		memcpy(sel, cand, n * sizeof(SiftSegEntry));
	}
	else
		n = 0;
	pfree(cand);
	return n;
}

/* The next merge: segments of the same tier, else the smallest ones when the directory has too many */
static int
merge_pick(Relation rel, SiftTblRel *st, SiftSegEntry *sel)
{
	SiftMergePolicy pol = {(uint64) sift_tbl_write_buffer_limit * 1024, SIFT_MERGE_FANIN, sift_tbl_mostly_dead};
	int			n = sift_merge_pick(rel, &pol, &st->dir, false, sel);

	return n > 0 || !dir_pressure(st) ? n : merge_smallest(rel, st, sel);
}

bool
sift_tbl_merge(Relation rel, SiftTblRel *st, TransactionId oldest)
{
	const SiftTblLayout *l = layout_of(rel, st);
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	XidFinal	fin = {.oldest = oldest};
	int			n;

	sift_tbl_refresh(rel, st);
	n = merge_pick(rel, st, sel);
	for (int i = 0; i < n; i++)
		if (schema_of(rel, st, &sel[i])->hash != l->schema->hash)
			return false;
	if (n == 0)
		return false;
	sift_col_merge_segments(rel, l->schema, layout_policy(rel, l), NULL, sel, n, tbl_col_format(any_vers(sel, n)), merge_freeze, &fin);
	sift_tbl_refresh(rel, st);
	return true;
}

/*
 * Writes the column segment e again; the writer takes the free blocks lowest in the file first, so a segment above them moves down.
 * False when e was written for another column layout (before an ALTER TABLE), which only a rewrite of the table converts.
 */
bool
sift_tbl_rewrite(Relation rel, SiftTblRel *st, const SiftSegEntry *e)
{
	const SiftTblLayout *l = layout_of(rel, st);

	if (schema_of(rel, st, e)->hash != l->schema->hash)
		return false;
	sift_col_merge_segments(rel, l->schema, layout_policy(rel, l), NULL, e, 1, tbl_col_format(any_vers(e, 1)), NULL, NULL);
	sift_tbl_refresh(rel, st);
	return true;
}

/* ---------- dead rows of the column segments ---------- */

int			sift_tbl_dead_rewrite_pct = SIFT_MERGE_DEAD_PCT;

bool
sift_tbl_mostly_dead(Relation rel, const SiftSegEntry *e)
{
	SiftColSegment *seg;
	bool		dead;

	if (!(e->flags & SIFT_SEG_DIRTY))
		return false;
	seg = sift_cseg_open(rel, e, CurrentMemoryContext);
	dead = sift_cseg_live_rows(seg) * 100 < e->ndocs * (100 - sift_tbl_dead_rewrite_pct);
	sift_cseg_close(seg);
	return dead;
}
