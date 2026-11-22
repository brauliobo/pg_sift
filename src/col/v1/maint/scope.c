/*
 * col/v1/maint/scope.c - the maintenance of the scope segments of a scope index (col/scope.h)
 * the scope segments a promotion writes beside its entity segment, in the same switch of the directory
 * the merge of the scope segments of one scope, by the range of its key
 *
 * The scope segments are copies of the rows of the entity segments, once for each scope key of the member of a row, with the address of the row. So a merge of entity segments
 * leaves them where they are (the addresses stay), a promotion must write them with the entity segment (the rows have no entries else), and a VACUUM ends the entries of a dead
 * address in every segment. The stamp the entries are exact as of is the floor of the index (col/v1/scope_floor.c), not held by its segments.
 */
#include "postgres.h"
#include "datatype/timestamp.h"
#include "utils/timestamp.h"
#include "core/ops.h"
#include "bm25/merge.h"
#ifdef SIFT_TESTING
#include "bm25/build.h"
#endif
#include "col/v1/build.h"
#include "col/v1/maint/maint_int.h"
#include "col/v1/order_sort.h"
#include "col/scope.h"
#include "core/hash.h"
#include "core/limits.h"
#include "core/memtrack.h"

#ifdef SIFT_TESTING
int			sift_test_col_scope_merges = 0;
int			sift_test_col_scope_merge_limit = 0;

static void
scope_merge_counted(void)
{
	if (sift_test_col_scope_merge_limit > 0 && sift_test_col_scope_merges >= sift_test_col_scope_merge_limit)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift test: more than %d merges of scope segments", sift_test_col_scope_merge_limit),
						errhint("The steps of the merge of the scope segments do not end: fix what makes a merge that writes as many segments as it read ask for another, not this limit.")));
	sift_test_col_scope_merges++;
}
#endif

/* The fill of the writers of a promotion: the found flags of the connections, and for an index with scopes the stamps of the fill that expands the rows (*sf; NULL for an index without) */
SiftColFill *
maint_scope_fill_begin(Relation index, MemoryContext cx, uint64 rows, struct SiftScopeFill **sf)
{
	SiftColFill *conn = sift_conn_fill_begin(index, cx, rows);

	*sf = sift_scope_fill_begin(index, cx, rows);
	return *sf != NULL ? sift_scope_fill_hook(*sf, conn, sift_col_rel_private(index, cx)->nconn) : conn;
}

/* The scope segments of a run of rows: for each scope the rows replicated by the keys of their member, held until the run is written */
struct ScopeRun
{
	Relation	index;
	struct SiftScopeFill *sf;
	const SiftColRel *r;
	MemoryContext cx;			/* the rows of the run, reset by the write */
	MemoryContext slice;		/* the entries of a slice: a slice has the entries of SCOPE_RUN_SLICE rows */
	SiftColBatch **rep;
	const int64 **members;
	uint32	   *row;
	int64	   *key;
	uint32		cap;
};

static void
run_open(ScopeRun *run)
{
	for (int s = 0; s < run->r->nscope; s++)
	{
		run->rep[s] = sift_cbatch_create(run->cx, run->r->schema);
		run->rep[s]->keyed = run->rep[s]->unsorted = true;
	}
}

ScopeRun *
maint_scope_run_begin(Relation index, struct SiftScopeFill *sf, MemoryContext parent)
{
	ScopeRun   *run = MemoryContextAllocZero(parent, sizeof(ScopeRun));

	run->index = index;
	run->sf = sf;
	run->r = sift_col_rel_private(index, parent);
	run->cx = AllocSetContextCreate(parent, "sift_col scope run", ALLOCSET_DEFAULT_SIZES);
	run->slice = AllocSetContextCreate(parent, "sift_col scope run slice", ALLOCSET_DEFAULT_SIZES);
	run->rep = MemoryContextAlloc(parent, run->r->nscope * sizeof(SiftColBatch *));
	run->members = MemoryContextAlloc(parent, run->r->nscope * sizeof(int64 *));
	run_open(run);
	return run;
}

void
maint_scope_run_end(ScopeRun *run)
{
	MemoryContextDelete(run->cx);
	MemoryContextDelete(run->slice);
}

/* The batch whose rows maint_scope_run_add takes next */
void
maint_scope_run_batch(ScopeRun *run, const SiftColBatch *b)
{
	for (int s = 0; s < run->r->nscope; s++)
		run->members[s] = sift_scope_fill_members(run->sf, s, b);
}

/* Rows [from, to) of the batch of maint_scope_run_batch, from a multiple of SCOPE_RUN_SLICE, with a copy of each for each key of their member */
void
maint_scope_run_add(ScopeRun *run, const SiftColBatch *b, uint32 from, uint32 to)
{
	for (int s = 0; s < run->r->nscope; s++)
	{
		const SiftCVals *fk = &b->col[sift_scope_fill_fk_leaf(run->sf, s)];
		MemoryContext old = MemoryContextSwitchTo(run->slice);
		uint32		entries = sift_scope_fill_expand(run->sf, s, run->members[s] + from, fk->nulls ? fk->nulls + from / 64 : NULL, to - from,
													 &run->row, &run->key, &run->cap);

		MemoryContextSwitchTo(old);
		for (uint32 e = 0; e < entries; e++)
			sift_cbatch_add_scoped(run->rep[s], b, from + run->row[e], run->r->scope_leaf[s], run->key[e], run->r->scope_cols[s]);
	}
}

/* Rows [from, to) of the batch of maint_scope_run_batch, from a multiple of SCOPE_RUN_SLICE, to the sort of each scope (sorter[s]; count[s] rows so far) with a copy of each for each key of their member */
void
maint_scope_run_sort(ScopeRun *run, const SiftColBatch *b, uint32 from, uint32 to, SiftColSorter **sorter, uint64 *count)
{
	for (int s = 0; s < run->r->nscope; s++)
	{
		const SiftCVals *fk = &b->col[sift_scope_fill_fk_leaf(run->sf, s)];
		MemoryContext old = MemoryContextSwitchTo(run->slice);
		uint32		entries = sift_scope_fill_expand(run->sf, s, run->members[s] + from, fk->nulls ? fk->nulls + from / 64 : NULL, to - from,
													 &run->row, &run->key, &run->cap);

		MemoryContextSwitchTo(old);
		for (uint32 e = 0; e < entries; e++)
			sift_colsort_put_scoped(sorter[s], b, from + run->row[e], run->r->scope_leaf[s], run->key[e], run->r->scope_cols[s]);
		count[s] += entries;
	}
}

/* Rows [from, to) of the batch of maint_scope_run_batch as maint_scope_run_sort takes them, but about one entry in stride of each scope goes to its sample (sift_colsort_sample_put_scoped); entries[s] counts all of them */
static void
run_sample(ScopeRun *run, const SiftColBatch *b, uint32 from, uint32 to, SiftColSorter **sorter, uint64 *entries, uint32 stride)
{
	for (int s = 0; s < run->r->nscope; s++)
	{
		const SiftCVals *fk = &b->col[sift_scope_fill_fk_leaf(run->sf, s)];
		MemoryContext old = MemoryContextSwitchTo(run->slice);
		uint32		n = sift_scope_fill_expand(run->sf, s, run->members[s] + from, fk->nulls ? fk->nulls + from / 64 : NULL, to - from,
											   &run->row, &run->key, &run->cap);

		MemoryContextSwitchTo(old);
		for (uint32 e = 0; e < n; e = (uint32) sift_sample_next(e, stride))
			sift_colsort_sample_put_scoped(sorter[s], b, from + run->row[e], run->r->scope_leaf[s], run->key[e], run->r->scope_cols[s]);
		entries[s] += n;
	}
}

/*
 * The run written (a promotion's rows): for each scope its rows sorted by the cut of the scope and cut into units of one key, with the fill of maint_scope_fill_begin. out has room for one segment of each
 * scope; the number of them is returned (a scope none of whose rows has a pair has none). The run is empty after it.
 */
int
maint_scope_run_write(ScopeRun *run, SiftWriteTarget *wt, SiftColFill *fill, uint64 index_rows, int format, SiftSegEntry *out)
{
	const SiftColRel *r = run->r;
	uint32		aim = sift_colk_aim_rows(run->index);
	MemoryContext old = MemoryContextSwitchTo(run->cx);
	int			n = 0;

	for (int s = 0; s < r->nscope; s++)
	{
		SiftCPaggSpec pagg = r->pagg;
		SiftColSegWriter *w;
		int			cut[SIFT_COL_CUT_MAX],
					ncut = sift_scope_cut(r, s, cut);

		if (run->rep[s]->nrows == 0)
			continue;
		pagg.lean = true;
		sift_cpagg_spec_keep(&pagg, r->schema, r->scope_cols[s]);
		w = sift_colw_begin(wt, sift_store_new_gen(run->index), r->schema, r->policy, format, run->cx, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 2));
		sift_colw_set_fill(w, fill);
		sift_colw_set_index_rows(w, index_rows);
		sift_colw_set_preagg(w, &pagg);
		sift_colw_set_keyed(w, aim, cut, ncut);
		sift_colk_write(w, &run->rep[s], 1, r->schema, cut, ncut, sift_colk_unit_rows(aim), run->cx, 0);
		sift_colw_finish(w, &out[n]);
		out[n].flags |= SIFT_SEG_SCOPE;
		out[n++].pad = s;
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(run->cx);
	run_open(run);
	return n;
}

/*
 * Is the floor of the stamps of the index something the rewrite is to make again: the relation of the pairs changed since it past what a read patches or what the refresh costs
 * (sift_scope_floor_stale), whether the index has scope segments or not (the rows it stores may have no entry).
 */
bool
maint_scope_refill_due(Relation index)
{
	SiftScopeStale *st = sift_scope_stale_begin(index, CurrentMemoryContext);
	bool		due;

	if (st == NULL)
		return false;
	due = sift_scope_floor_stale(st);
	sift_scope_stale_end(st);
	return due;
}

typedef struct Refill
{
	const uint64 *old;			/* the scope segments the new ones replace */
	int			nold;
	const SiftSegEntry *neu;
	int			nneu;
	const uint64 *ents;			/* the entity segments whose rows the new ones hold the entries of */
	int			nents;
	const SiftScopeStamps *mine;	/* the stamps they were expanded under */
} Refill;

static bool
in_gens(const uint64 *gen, int n, uint64 g)
{
	for (int i = 0; i < n; i++)
		if (gen[i] == g)
			return true;
	return false;
}

/* The one switch of the directory of a refill: the scope segments are replaced and the floor of the stamps is the stamp they were made under */
static void
refill_switch(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	Refill	   *rf = arg;
	int			n = 0;

	for (int i = 0; i < dir->nsegs; i++)
	{
		if (in_gens(rf->old, rf->nold, dir->segs[i].gen))
			continue;
		dir->segs[n++] = dir->segs[i];
	}
	if (n + rf->nneu > SIFT_DIR_WIDE_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the directory has no room for the %d scope segments this refill wrote", rf->nneu),
						errhint("Merge the segments first (sift.col_merge()).")));
	sift_scope_floor_raise(meta, dir, rf->mine, rf->ents, rf->nents);
	memcpy(dir->segs + n, rf->neu, rf->nneu * sizeof(SiftSegEntry));
	dir->nsegs = n + rf->nneu;
	meta->flags &= ~SIFT_META_FAILURES_MASK;
}

#define scope_sort_kb()		(maintenance_work_mem / 2)

/*
 * The sorts of a refill go by key ranges when their rows would not fit their memory (col/v1/order_sort.c): the keys the ranges are cut at, and the size of the rows, are those of a sample, the rows
 * of REFILL_SAMPLE_UNITS units spread over the entity segments (REFILL_SAMPLE_ENTRIES of the entries of each go to the sample), before the rows are read; and the sample is not made when even
 * the smallest entries of one for each row would fit (sift_colsort_may_range).
 */
#define REFILL_SAMPLE_UNITS		24
#define REFILL_SAMPLE_ENTRIES	256

static void
refill_sample(Relation index, const SiftColRel *r, const SiftSegEntry *ents, int nents, uint64 rows, ScopeRun *run, SiftColSorter **sorter, MemoryContext cx, MemoryContext ucx)
{
	uint64		entries[SIFT_SCOPE_MAX] = {0},
				sampled = 0;

	for (int i = 0; i < nents; i++)
	{
		int			take = (int) ((int64) REFILL_SAMPLE_UNITS * (i + 1) / nents - (int64) REFILL_SAMPLE_UNITS * i / nents);
		SiftColSegment *seg;
		uint32		last = PG_UINT32_MAX;

		if (take == 0)
			continue;
		seg = sift_cseg_open(index, &ents[i], cx);
		for (int j = 0; j < take && seg->nsb > 0; j++)
		{
			uint32		u = (uint32) ((uint64) (2 * j + 1) * seg->nsb / (2 * take));
			SiftColBatch *b;

			if (u == last)
				continue;
			last = u;
			MemoryContextReset(ucx);
			b = sift_cbatch_create(ucx, r->schema);
			MemoryContextSwitchTo(ucx);
			sift_cseg_decode_unit(seg, u, b);
			MemoryContextSwitchTo(cx);
			maint_scope_run_batch(run, b);
			for (uint32 from = 0; from < b->nrows; from += SCOPE_RUN_SLICE)
				run_sample(run, b, from, Min(from + SCOPE_RUN_SLICE, b->nrows), sorter, entries, Max(b->nrows / REFILL_SAMPLE_ENTRIES, 1));
			sampled += b->nrows;
		}
		sift_cseg_close(seg);
	}
	for (int s = 0; s < r->nscope && sampled > 0; s++)
		sift_colsort_sample_end(sorter[s], (uint64) ((double) entries[s] * rows / sampled));
}

/* The segments a refill writes of one scope: the rows come sorted, and a segment is done when its writer holds its budget of memory */
typedef struct ScopeOut
{
	Relation	index;
	const SiftColRel *r;
	int			scope;
	int			format;
	uint64		index_rows;
	Size		budget;
	bool		roots;			/* every entity segment the rows come from has SIFT_SEG_ROOTS */
	SiftColFill *fill;
	SiftCPaggSpec pagg;
	int			cut[SIFT_COL_CUT_MAX];
	int			ncut;
	SiftWriteTarget *wt;
	SiftColSegWriter *w;
	MemoryContext cx;			/* the parent of the context of each segment */
	MemoryContext pcx;
	SiftSegEntry *out;
	int			n;
	int			cap;
} ScopeOut;

/* The bytes of the segments seg[n] of one scope (-1: all of them) */
static uint64
bytes_of(const SiftSegEntry *seg, int n, int scope)
{
	uint64		sum = 0;

	for (int i = 0; i < n; i++)
		sum += scope < 0 || seg[i].pad == scope ? seg[i].bytes : 0;
	return sum;
}

static Size
scope_out_budget(void)
{
#ifdef SIFT_TESTING
	if (sift_test_col_segment_kb > 0)
		return (Size) sift_test_col_segment_kb * 1024;
#endif
	return sift_col_merge_writer_budget(1);
}

static void
scope_out_finish(ScopeOut *o)
{
	if (o->n == o->cap)
	{
		o->cap = Max(o->cap * 2, 8);
		o->out = o->out ? repalloc(o->out, o->cap * sizeof(SiftSegEntry)) : MemoryContextAlloc(o->cx, o->cap * sizeof(SiftSegEntry));
	}
	sift_colw_finish(o->w, &o->out[o->n]);
	o->out[o->n].flags |= SIFT_SEG_SCOPE | (o->roots ? SIFT_SEG_ROOTS : 0);
	o->out[o->n++].pad = o->scope;
	sift_wt_finish(o->wt);
	o->w = NULL;
	MemoryContextDelete(o->pcx);
}

static SiftColSegWriter *
scope_out_next(void *arg, SiftColSegWriter *w, bool open)
{
	ScopeOut   *o = arg;

	if (w == NULL)
	{
		o->pcx = AllocSetContextCreate(o->cx, "sift_col scope refill segment", ALLOCSET_DEFAULT_SIZES);
		o->wt = sift_wt_begin(o->index, SIFT_WRITE_SHARED);
		o->w = sift_colw_begin(o->wt, sift_store_new_gen(o->index), o->r->schema, o->r->policy, o->format, o->pcx, o->budget);
		sift_colw_set_fill(o->w, o->fill);
		sift_colw_set_index_rows(o->w, o->index_rows);
		sift_colw_set_preagg(o->w, &o->pagg);
		sift_colw_set_keyed(o->w, sift_colk_aim_rows(o->index), o->cut, o->ncut);
	}
	else if (sift_colw_full(w))
		scope_out_finish(o);
	return o->w;
}

/*
 * The scope segments again, from the live rows of the entity segments, under a stamp taken now: the rows of each entity segment are decoded unit by unit, replicated by the keys of
 * their member and sorted by the cut of the scope (a sort that holds half of maintenance_work_mem and writes the rest to temporary files), and the sorted rows are written as segments of the scope. The old scope segments go and the floor of the stamps becomes this stamp in
 * one switch of the directory, so a crash leaves the index as it was; with no pair left it writes no segment and still moves the floor. This is what the rewrite does when the floor is
 * stale (the relation changed since it). False: no entity segment. report: the build of the index is not over (a concurrent one, whose first pass wrote no scope segment): each scope says what it costs, or is
 * refused above its limit, as the end of a build does (sift_scope_report).
 */
bool
maint_scope_refill(Relation index, bool report)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col scope refill", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftDirSnapshot *snap;
	const SiftColRel *r = sift_col_rel_private(index, cx);
	MemoryContext ucx = AllocSetContextCreate(cx, "sift_col scope refill unit", ALLOCSET_DEFAULT_SIZES);	/* the rows of the unit decoded */
	SiftMemTracker mt_scope_refill;
	SiftSegEntry *ents,
			   *scopes,
			   *out;
	SiftColSorter *sorter[SIFT_SCOPE_MAX];
	uint64	   *egen,
				rows = 0,
				count[SIFT_SCOPE_MAX] = {0};
	int			nents = 0,
				nscopes = 0,
				nout = 0;
	TimestampTz start = GetCurrentTimestamp();
	struct SiftScopeFill *sf;
	ScopeRun   *run;
	SiftColFill *fill;
	SiftMaintInputs mi;
	SiftScopeStamps stamps;
	Refill		rf;

	sift_mem_init_sub(&mt_scope_refill, "col_scope_refill", cx, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
	snap = sift_dir_read(index);
	ents = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));
	scopes = palloc(Max(snap->nsegs, 1) * sizeof(SiftSegEntry));
	egen = palloc(Max(snap->nsegs, 1) * sizeof(uint64));
	for (int i = 0; i < snap->nsegs; i++)
	{
		if (sift_seg_is_scope(&snap->segs[i]))
			scopes[nscopes++] = snap->segs[i];
		else if (snap->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			egen[nents] = snap->segs[i].gen;
			rows += snap->segs[i].ndocs;
			ents[nents++] = snap->segs[i];
		}
	}
	if (nents == 0)
	{
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cx);
		return false;
	}
	/* the rows of each scope are sorted by a sort that holds half of maintenance_work_mem between the scopes and writes the rest to temporary files, and come out in the order of the cut of the scope */
	out = palloc(0);
	fill = maint_scope_fill_begin(index, cx, rows, &sf);
	run = maint_scope_run_begin(index, sf, cx);
	for (int s = 0; s < r->nscope; s++)
	{
		int			cut[SIFT_COL_CUT_MAX];

		sorter[s] = sift_colsort_begin(r->schema, cut, sift_scope_cut(r, s, cut), scope_sort_kb() / r->nscope);
	}
	if (sift_colsort_may_range(rows, scope_sort_kb() / r->nscope))
		refill_sample(index, r, ents, nents, rows, run, sorter, cx, ucx);
	for (int i = 0; i < nents; i++)
	{
		MemoryContext scx = AllocSetContextCreate(cx, "sift_col scope refill input", ALLOCSET_DEFAULT_SIZES);
		SiftColSegment *seg = sift_cseg_open(index, &ents[i], scx);

		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftColBatch *b;

			MemoryContextReset(ucx);
			b = sift_cbatch_create(ucx, r->schema);
			MemoryContextSwitchTo(ucx);
			sift_cseg_decode_unit(seg, u, b);
			MemoryContextSwitchTo(cx);
			maint_scope_run_batch(run, b);
			sift_mem_assert_within(&mt_scope_refill);
			for (uint32 from = 0; from < b->nrows; from += SCOPE_RUN_SLICE)
			{
				maint_scope_run_sort(run, b, from, Min(from + SCOPE_RUN_SLICE, b->nrows), sorter, count);
				sift_colb_peak_note();
			}
		}
		sift_cseg_close(seg);
		MemoryContextDelete(scx);
	}
	maint_scope_run_end(run);
	for (int s = 0; s < r->nscope; s++)
	{
		MemoryContext mcx = AllocSetContextCreate(cx, "sift_col scope refill merge", ALLOCSET_DEFAULT_SIZES);
		ScopeOut	o = {.index = index, .cx = mcx, .r = r, .scope = s, .format = sift_col_format_minor, .index_rows = rows, .fill = fill, .pagg = r->pagg,
					 .budget = scope_out_budget(), .roots = maint_segs_roots(ents, nents)};

		if (count[s] == 0)
		{
			sift_colsort_end(sorter[s]);
			MemoryContextDelete(mcx);
			continue;
		}
		o.pagg.lean = true;
		sift_cpagg_spec_keep(&o.pagg, r->schema, r->scope_cols[s]);
		o.ncut = sift_scope_cut(r, s, o.cut);
		sift_colsort_done(sorter[s]);
		sift_colb_peak_note();
		sift_colk_merge(NULL, NULL, NULL, 0, NULL, NULL, r->schema, o.cut, o.ncut, sift_colk_unit_rows(sift_colk_aim_rows(index)), maintenance_work_mem, mcx, scope_out_next, &o, 0, 1, 0, NULL, NULL, sorter[s], NULL);
		if (o.w != NULL)
			scope_out_finish(&o);
		out = repalloc(out, (nout + o.n) * sizeof(SiftSegEntry));
		memcpy(out + nout, o.out, o.n * sizeof(SiftSegEntry));
		nout += o.n;
		MemoryContextDelete(mcx);
	}
	for (int s = 0; report && s < r->nscope; s++)
		sift_scope_report(index, r, s, rows, count[s], bytes_of(ents, nents, -1), bytes_of(out, nout, s));
	sift_scope_fill_stamps(sf, &stamps);
	sift_conn_fill_end(fill);
	mi = sift_maint_inputs(index, scopes, nscopes);
	rf = (Refill) {mi.gens, nscopes, out, nout, egen, nents, &stamps};
	sift_dir_update(index, &(SiftDirChange) {.fn = refill_switch,.arg = &rf,.release = mi.release,.nrelease = mi.nrelease,.nadd = nout,.extra = InvalidBuffer});
	sift_log_maint(index, "refilled the scope segments of", start, rows, 0);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return true;
}

/*
 * The validation of a CREATE INDEX CONCURRENTLY (or REINDEX CONCURRENTLY) of a scope index, which holds ShareUpdateExclusiveLock only: the segments of the build were written before the
 * journal of the first scope index of the relation began, so they carry no stamp. The journal began at the end of the build, which committed, and every writer that could not see it was waited
 * for: the stamp taken now is vouched for, and the writers of the tables go on while the segments are filled as the rewrite fills a stale set and merged as the rewrite after a build does.
 */
void
sift_scope_stamp_concurrent(Relation index)
{
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_COL_GATE_FILL);
#endif
	if (maint_scope_refill(index, true))
		while (maint_scope_merge_step(index, true))
			;
}

/* What a set of scope segments is to the merge of everything: the bytes and rows it would write again, and whether a key lies alone in several of the segments */
typedef struct ScopeSet
{
	uint64		bytes;
	uint64		rows;
	bool		runs;
} ScopeSet;

/*
 * Are the segments of scope s clean, and do the ranges of their keys share at most a boundary key? They are the partitions of a merge of everything, which a merge of all of them would
 * write again as they are, but for the runs: a key that is alone in several of them.
 */
static bool
scope_chained(Relation index, const SiftSegEntry *sel, int n, int s, ScopeSet *set)
{
	int64	   *lo = palloc(Max(n, 1) * sizeof(int64)),
			   *hi = palloc(Max(n, 1) * sizeof(int64));
	int			leaf = sift_col_rel(index)->scope_leaf[s];
	MemoryContext scx = maint_segment_cx();
	bool		chained = true;

	*set = (ScopeSet) {0};
	for (int i = 0; i < n && chained; i++)
	{
		SiftColSegment *seg;

		chained = !(sel[i].flags & SIFT_SEG_DIRTY);
		if (!chained)
			break;
		set->bytes += sel[i].bytes;
		set->rows += sel[i].ndocs;
		seg = sift_cseg_open(index, &sel[i], scx);
		maint_seg_key_range(seg, leaf, &lo[i], &hi[i]);
		sift_cseg_close(seg);
		MemoryContextReset(scx);
	}
	MemoryContextDelete(scx);
	for (int i = 0; i < n && chained; i++)
		for (int j = i + 1; j < n && chained; j++)
		{
			chained = !(lo[i] < hi[j] && lo[j] < hi[i]);
			set->runs |= lo[i] == hi[i] && lo[j] == hi[j] && lo[i] == lo[j];
		}
	pfree(lo);
	pfree(hi);
	return chained;
}

/*
 * Are the segments of scope s the end state of a merge of everything? They are chained, and the runs of a key among them are what the merge writes for their bytes: a key of more rows
 * than a segment holds is in the fewest segments that hold it, and the runs of the participants of a build, which are more, are merged.
 */
static bool
scope_apart(Relation index, const SiftSegEntry *sel, int n, int s)
{
	ScopeSet	set;

	return scope_chained(index, sel, n, s, &set) &&
		(!set.runs || n <= maint_merge_parts(set.bytes, set.rows, maint_part_kb > 0 ? (uint64) maint_part_kb * 1024 : 0, true));
}

/* The segments of scope s among those of the directory, and which of them the policy merges: by the tiers (of those that are not partitions), or all of them (force) */
static int
scope_pick(Relation index, const SiftDirSnapshot *all, int s, bool force, SiftSegEntry *sel)
{
	SiftDirSnapshot *fam;
	SiftMergePolicy pol = {.tier_base = (uint64) sift_col_options(index)->write_buffer_limit * 1024, .fanin = SIFT_MERGE_FANIN, .mostly_dead = sift_col_mostly_dead};
	int			n;

	fam = sift_dir_alloc(all->nsegs);
	for (int i = 0; i < all->nsegs; i++)
		if (sift_seg_is_scope(&all->segs[i]) && sift_seg_scope_ordinal(&all->segs[i]) == s)
			fam->segs[fam->nsegs++] = all->segs[i];
	maint_use_partitions(index, fam);
	if (!force)
		maint_keep_overlays(fam);
	n = sift_merge_pick(index, &pol, fam, force, sel);
	pfree(fam);
	if (force)
		return n > 1 && scope_apart(index, sel, n, s) ? 0 : n;
	return n > 1 && maint_merge_settled(index, sel, n) ? sift_merge_pick_dead(index, &pol, fam, sel) : n;
}

/*
 * One step of the merge of the scope segments (col/scope.h): the segments of the first scope that has some to merge, by the tiers of the policy (all of them when force is asked, or
 * after a parallel build, whose participants each left a run of every key), into one segment or partitions of the range of the key. The entities are not merged with them. The stamp of
 * the output is the oldest of the inputs'. False: nothing to do, or nothing that makes progress: a merge of segments that were chained and clean writes them again when it writes as many,
 * so the step goes on to the next scope. A scope that is merged and is not chained is chained after, one that is chained has fewer segments after, so a step that makes progress takes
 * the pair (segments not chained or dirty, segments) of its scope down, and the steps end.
 */
bool
maint_scope_merge_step(Relation index, bool force)
{
	SiftDirSnapshot *all;
	SiftSegEntry *sel;
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	bool		progress = false;

	all = sift_dir_read(index);
	sel = palloc(Max(all->nsegs, 1) * sizeof(SiftSegEntry));
	for (int s = 0; s < r->nscope && !progress; s++)
	{
		int			n = scope_pick(index, all, s, force, sel);
		ScopeSet	set;

		if (n == 0)
			continue;
		progress = !scope_chained(index, sel, n, s, &set);
#ifdef SIFT_TESTING
		scope_merge_counted();
#endif
		progress |= maint_merge_segments(index, r->schema, r->policy, &r->pagg, sel, n, sift_col_format_minor, NULL, NULL,
						   maint_part_kb > 0 ? (uint64) maint_part_kb * 1024 : 0, 1, NULL) < n;
	}
	pfree(sel);
	pfree(all);
	return progress;
}

/* Would a step of the merge of the scope segments find something to do (the rewrite is due for it) */
bool
maint_scope_merge_due(Relation index, bool force)
{
	SiftDirSnapshot *all;
	SiftSegEntry *sel;
	int			nscope = sift_col_rel(index)->nscope;
	bool		due = false;

	all = sift_dir_read(index);
	sel = palloc(Max(all->nsegs, 1) * sizeof(SiftSegEntry));
	for (int s = 0; s < nscope && !due; s++)
		due = scope_pick(index, all, s, force, sel) > 0;
	pfree(sel);
	pfree(all);
	return due;
}
