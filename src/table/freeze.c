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
#include "core/cmp.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "bm25/merge.h"
#include "core/ops.h"
#include "table/table.h"
#include "table/seg_int.h"

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

int			sift_tbl_dead_rewrite_pct = SIFT_TBL_DEAD_PCT;

/* The shared policy leaves the dead segments to merge_most_dead, which ranks them */
static bool
no_dead(Relation rel, const SiftSegEntry *e)
{
	return false;
}

/*
 * The segment of the current layout with the largest share of dead rows, above sift.table_dead_rewrite_pct percent: a rewrite copies the live
 * rows and frees the dead ones, so what it frees per byte it copies is that share, and a pass that stops early (the budget of the worker) has
 * freed the most it could. Cleared liveness bits only: the rows a tombstone says are dead count once a pass has cleared them.
 */
static int
merge_most_dead(Relation rel, SiftTblRel *st, SiftSegEntry *sel)
{
	const SiftTblLayout *l = layout_of(rel, st);
	uint64		best_dead = 0,
				best_docs = 1;
	int			n = 0;

	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];
		SiftColSegment *seg;
		uint64		dead;

		if (e->kind != SIFT_SEG_IMMUTABLE || !(e->flags & SIFT_SEG_DIRTY) || schema_of(rel, st, e)->hash != l->schema->hash)
			continue;
		seg = sift_cseg_open(rel, e, CurrentMemoryContext);
		dead = e->ndocs - sift_cseg_live_rows(seg);
		sift_cseg_close(seg);
		if (dead * 100 > (uint64) e->ndocs * sift_tbl_dead_rewrite_pct && dead * best_docs > best_dead * e->ndocs)
		{
			sel[0] = *e;
			best_dead = dead;
			best_docs = e->ndocs;
			n = 1;
		}
	}
	return n;
}

int
sift_tbl_cmp_sb_lo(const void *a, const void *b)
{
	return pg_cmp_u32(((const SiftSegEntry *) a)->sb_lo, ((const SiftSegEntry *) b)->sb_lo);
}

/*
 * The groups next to the one a rewrite picked (n = 1), or a run of them (n = 0), join it when they are small: a group a rewrite left
 * behind, or one a closed buffer made, costs an entry, a header and a zone array to every scan, and its neighbours in the key order are
 * the ones whose union stays one tight range. Small is under half the write buffer limit, and the groups added are at most that much.
 */
static int
merge_adjacent(Relation rel, SiftTblRel *st, SiftSegEntry *sel, int n)
{
	const SiftTblLayout *l = layout_of(rel, st);
	uint64		small = (uint64) sift_tbl_write_buffer_limit * 1024 / 2,
				room = 2 * small;
	SiftSegEntry *all = palloc(Max(st->dir.nsegs, 1) * sizeof(SiftSegEntry));
	int			na = 0,
				lo = -1,
				hi;

	for (int i = 0; i < st->dir.nsegs; i++)
		if (st->dir.segs[i].kind == SIFT_SEG_IMMUTABLE && schema_of(rel, st, &st->dir.segs[i])->hash == l->schema->hash)
			all[na++] = st->dir.segs[i];
	qsort(all, na, sizeof(SiftSegEntry), sift_tbl_cmp_sb_lo);
	for (int i = 0; i < na && lo < 0; i++)
		if (n == 1 ? all[i].gen == sel[0].gen : all[i].bytes < small && i + 1 < na && all[i + 1].bytes < small)
			lo = i;
	if (lo >= 0)
	{
		hi = lo;
		if (n == 0)
			room -= all[lo].bytes;
		while (hi + 1 < na && hi - lo + 1 < SIFT_MERGE_FANIN && all[hi + 1].bytes < small && all[hi + 1].bytes <= room)
			room -= all[++hi].bytes;
		while (lo > 0 && hi - lo + 1 < SIFT_MERGE_FANIN && all[lo - 1].bytes < small && all[lo - 1].bytes <= room)
			room -= all[--lo].bytes;
		if (hi > lo)
		{
			n = hi - lo + 1;
			memcpy(sel, all + lo, n * sizeof(SiftSegEntry));
		}
	}
	pfree(all);
	return n;
}

bool		sift_tbl_coalesce = false;

/* The next merge: segments of the same tier, else the most dead segment, else the smallest ones when the directory has too many; a worker's pass then adds the small neighbours */
static int
merge_pick(Relation rel, SiftTblRel *st, SiftSegEntry *sel)
{
	SiftMergePolicy pol = {(uint64) sift_tbl_write_buffer_limit * 1024, SIFT_MERGE_FANIN, no_dead};
	int			n = sift_merge_pick(rel, &pol, &st->dir, false, sel);

	if (n == 0)
		n = merge_most_dead(rel, st, sel);
	if (n <= 1 && sift_tbl_coalesce)
		n = merge_adjacent(rel, st, sel, n);
	return n > 0 || !dir_pressure(st) ? n : merge_smallest(rel, st, sel);
}

uint64
sift_tbl_merge(Relation rel, SiftTblRel *st, TransactionId oldest)
{
	const SiftTblLayout *l = layout_of(rel, st);
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	XidFinal	fin = {.oldest = oldest};
	SiftCPaggSpec spec;
	uint64		bytes = 0;
	int			n;

	sift_tbl_refresh(rel, st);
	n = merge_pick(rel, st, sel);
	for (int i = 0; i < n; i++)
	{
		if (schema_of(rel, st, &sel[i])->hash != l->schema->hash)
			return 0;
		bytes += sel[i].bytes;
	}
	if (n == 0)
		return 0;
	sift_tbl_pagg_spec(rel, l->schema, &spec);
	sift_col_merge_segments(rel, l->schema, layout_policy(rel, l), &spec, sel, n, tbl_col_format(any_vers(sel, n)), merge_freeze, &fin);
	sift_tbl_refresh(rel, st);
	return bytes;
}
