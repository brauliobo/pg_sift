/*
 * col/maint/scope.c - the maintenance of the scope segments of a scope index (col/scope.h)
 * the scope segments a promotion writes beside its entity segment, in the same switch of the directory
 * the merge of the scope segments of one scope, by the range of its key
 *
 * The scope segments are copies of the rows of the entity segments, once for each scope key of the member of a row, with the address of the row. So a merge of entity segments
 * leaves them where they are (the addresses stay), a promotion must write them with the entity segment (the rows have no entries else), and a VACUUM ends the entries of a dead
 * address in every segment. The stamp the entries are exact as of is the floor of the index (col/scope_floor.c), not held by its segments.
 */
#include "postgres.h"
#include "datatype/timestamp.h"
#include "utils/timestamp.h"
#include "core/ops.h"
#include "bm25/merge.h"
#ifdef SIFT_TESTING
#include "bm25/build.h"
#endif
#include "col/build.h"
#include "col/maint/maint_int.h"
#include "col/scope.h"

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

Size
maint_scope_run_bytes(ScopeRun *run)
{
	return MemoryContextMemAllocated(run->cx, true);
}

/* The least a run holds, as the writer of a merge holds at least SIFT_COLW_MIN_BUDGET: below it the runs of a table are too many for the directory and for the merge that joins them */
#define SCOPE_RUN_MIN_BYTES	((Size) 16 << 20)

/* The bytes of replicated rows a run holds: half of maintenance_work_mem, at least SCOPE_RUN_MIN_BYTES */
static Size
run_bytes(void)
{
	return Max((Size) maintenance_work_mem * 1024 / 2, SCOPE_RUN_MIN_BYTES);
}

/* The merge of the runs holds two units of each (the one it reads and the one before it): a run is cut into units of the rows that make the runs there will be fit the bytes of a run, at least SCOPE_RUN_UNIT_MIN */
#define SCOPE_RUN_UNIT_MIN	256

static uint32
run_unit_rows(const ScopeRun *run, uint32 aim, uint32 runs)
{
	uint64		fit = run_bytes() / Max(runs, 1) / 2 / sift_colk_row_bytes(run->r->schema);

	return (uint32) Min(sift_colk_unit_rows(aim), Max(fit, SCOPE_RUN_UNIT_MIN));
}

/*
 * The run written: for each scope its rows sorted by the cut of the scope and cut into units of one key, with the fill of maint_scope_fill_begin. out has room for one segment of each
 * scope; the number of them is returned (a scope none of whose rows has a pair has none). The run is empty after it.
 */
int
maint_scope_run_write(ScopeRun *run, SiftWriteTarget *wt, SiftColFill *fill, uint64 index_rows, int format, uint32 runs, SiftSegEntry *out)
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
		w = sift_colw_begin(wt, sift_store_new_gen(run->index), r->schema, r->policy, format, run->cx);
		sift_colw_set_fill(w, fill);
		sift_colw_set_index_rows(w, index_rows);
		sift_colw_set_preagg(w, &pagg);
		sift_colw_set_keyed(w, aim, cut, ncut);
		sift_colk_write(w, &run->rep[s], 1, r->schema, cut, ncut, run_unit_rows(run, aim, runs), run->cx, 0);
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
	if (n + rf->nneu > SIFT_MAX_SEGMENTS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the directory has no room for the %d scope segments this refill wrote", rf->nneu),
						errhint("Merge the segments first (sift.col_merge()).")));
	sift_scope_floor_raise(meta, dir, rf->mine, rf->ents, rf->nents);
	memcpy(dir->segs + n, rf->neu, rf->nneu * sizeof(SiftSegEntry));
	dir->nsegs = n + rf->nneu;
	meta->flags &= ~SIFT_META_FAILURES_MASK;
}

/* The segments of the directory a refill leaves free for the merge that follows it */
#define REFILL_ROOM	16

/* The runs a refill writes for each scope: the rows of the table over the rows of the run in hand, as many as the directory has room for at the most */
static uint32
runs_of(uint64 rows, uint64 run_rows, int room)
{
	return (uint32) Min((rows + run_rows - 1) / run_rows, (uint64) room);
}

/*
 * The scope segments again, from the live rows of the entity segments, under a stamp taken now: the rows of each entity segment are decoded unit by unit, replicated by the keys of
 * their member and written as runs of the scope's cut, which the merge of the scope segments joins. The old scope segments go and the floor of the stamps becomes this stamp in
 * one switch of the directory, so a crash leaves the index as it was; with no pair left it writes no segment and still moves the floor. This is what the rewrite does when the floor is
 * stale (the relation changed since it). False: no entity segment.
 */
bool
maint_scope_refill(Relation index)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col scope refill", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftColRel *r = sift_col_rel_private(index, cx);
	MemoryContext ucx = AllocSetContextCreate(cx, "sift_col scope refill unit", ALLOCSET_DEFAULT_SIZES);	/* the rows of the unit decoded */
	SiftSegEntry ents[SIFT_MAX_SEGMENTS],
				scopes[SIFT_MAX_SEGMENTS],
			   *out = palloc(SIFT_MAX_SEGMENTS * sizeof(SiftSegEntry));
	uint64		egen[SIFT_MAX_SEGMENTS],
				rows = 0,
				held_rows = 0,
				chunk;
	int			nents = 0,
				nscopes = 0,
				nout = 0,
				room;
	TimestampTz start = GetCurrentTimestamp();
	struct SiftScopeFill *sf;
	ScopeRun   *run;
	SiftColFill *fill;
	SiftWriteTarget *wt;
	SiftMaintInputs mi;
	SiftScopeStamps stamps;
	Refill		rf;

	sift_dir_read(index, snap);
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
	/* a run holds half of maintenance_work_mem of replicated rows (at least SCOPE_RUN_MIN_BYTES), and at least the share of the rows that makes as many runs for each scope as the directory has room for */
	room = Max((SIFT_MAX_SEGMENTS - REFILL_ROOM - nents) / Max(r->nscope, 1), 1);
	chunk = rows / room + 1;
	fill = maint_scope_fill_begin(index, cx, rows, &sf);
	run = maint_scope_run_begin(index, sf, cx);
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	for (int i = 0; i < nents; i++)
	{
		SiftColSegment *seg = sift_cseg_open(index, &ents[i], cx);

		for (uint32 u = 0; u < seg->nsb; u++)
		{
			SiftColBatch *b;

			MemoryContextReset(ucx);
			b = sift_cbatch_create(ucx, r->schema);
			MemoryContextSwitchTo(ucx);
			sift_cseg_decode_unit(seg, u, b);
			MemoryContextSwitchTo(cx);
			maint_scope_run_batch(run, b);
			for (uint32 from = 0; from < b->nrows; from += SCOPE_RUN_SLICE)
			{
				uint32		to = Min(from + SCOPE_RUN_SLICE, b->nrows);

				maint_scope_run_add(run, b, from, to);
				held_rows += to - from;
				sift_colb_peak_note();
				if (held_rows >= chunk && maint_scope_run_bytes(run) >= run_bytes())
				{
					nout += maint_scope_run_write(run, wt, fill, rows, sift_col_format_minor, runs_of(rows, held_rows, room), out + nout);
					held_rows = 0;
				}
			}
		}
		sift_cseg_close(seg);
	}
	if (held_rows > 0)
		nout += maint_scope_run_write(run, wt, fill, rows, sift_col_format_minor, runs_of(rows, held_rows, room), out + nout);
	maint_scope_run_end(run);
	sift_scope_fill_stamps(sf, &stamps);
	sift_conn_fill_end(fill);
	sift_wt_finish(wt);
	mi = sift_maint_inputs(index, scopes, nscopes);
	rf = (Refill) {mi.gens, nscopes, out, nout, egen, nents, &stamps};
	sift_dir_update(index, &(SiftDirChange) {.fn = refill_switch,.arg = &rf,.release = mi.release,.nrelease = mi.nrelease,.extra = InvalidBuffer});
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
	if (maint_scope_refill(index))
		while (maint_scope_merge_step(index, true))
			;
}

/*
 * Are the segments of scope s clean and does no key lie in two of them (the partitions of a merge, which a merge of all of them would write again as they are)? They are the end state of a
 * merge of everything: it ends the steps of the rewrite after a build and of sift.col_merge.
 */
static bool
scope_apart(Relation index, const SiftSegEntry *sel, int n, int s)
{
	int64		lo[SIFT_MAX_SEGMENTS],
				hi[SIFT_MAX_SEGMENTS];
	int			leaf = sift_col_rel(index)->scope_leaf[s];
	bool		apart = true;

	for (int i = 0; i < n && apart; i++)
	{
		SiftColSegment *seg;

		apart = !(sel[i].flags & SIFT_SEG_DIRTY);
		if (!apart)
			break;
		seg = sift_cseg_open(index, &sel[i], CurrentMemoryContext);
		maint_seg_key_range(seg, leaf, &lo[i], &hi[i]);
		sift_cseg_close(seg);
	}
	for (int i = 0; i < n && apart; i++)
		for (int j = i + 1; j < n && apart; j++)
			apart = !((lo[i] < hi[j] && lo[j] < hi[i]) || (lo[i] == hi[i] && lo[j] == hi[j] && lo[i] == lo[j]));
	return apart;
}

/* The segments of scope s among those of the directory, and which of them the policy merges: by the tiers, or all of them (force) */
static int
scope_pick(Relation index, const SiftDirSnapshot *all, int s, bool force, SiftSegEntry *sel)
{
	SiftDirSnapshot *fam = palloc(sizeof(SiftDirSnapshot));
	SiftMergePolicy pol = {.tier_base = (uint64) sift_col_options(index)->write_buffer_limit * 1024, .fanin = SIFT_MERGE_FANIN, .mostly_dead = sift_col_mostly_dead};
	int			n;

	fam->nsegs = 0;
	for (int i = 0; i < all->nsegs; i++)
		if (sift_seg_is_scope(&all->segs[i]) && sift_seg_scope_ordinal(&all->segs[i]) == s)
			fam->segs[fam->nsegs++] = all->segs[i];
	maint_use_partitions(index, fam);
	n = sift_merge_pick(index, &pol, fam, force, sel);
	pfree(fam);
	return force && n > 1 && scope_apart(index, sel, n, s) ? 0 : n;
}

/*
 * One step of the merge of the scope segments (col/scope.h): the segments of the first scope that has some to merge, by the tiers of the policy (all of them when force is asked, or
 * after a parallel build, whose participants each left a run of every key), into one segment or partitions of the range of the key. The entities are not merged with them. The stamp of
 * the output is the oldest of the inputs'. False: nothing to do.
 */
bool
maint_scope_merge_step(Relation index, bool force)
{
	SiftDirSnapshot *all = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	bool		done = false;

	sift_dir_read(index, all);
	for (int s = 0; s < r->nscope && !done; s++)
	{
		int			n = scope_pick(index, all, s, force, sel);

		if (n == 0)
			continue;
		maint_merge_segments(index, r->schema, r->policy, &r->pagg, sel, n, sift_col_format_minor, NULL, NULL,
					   maint_part_kb > 0 ? (uint64) maint_part_kb * 1024 : 0, Max(SIFT_MAX_SEGMENTS - 16 - (all->nsegs - n), 1), NULL);
		done = true;
	}
	pfree(all);
	return done;
}

/* Would a step of the merge of the scope segments find something to do (the rewrite is due for it) */
bool
maint_scope_merge_due(Relation index, bool force)
{
	SiftDirSnapshot *all = palloc(sizeof(SiftDirSnapshot));
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	int			nscope = sift_col_rel(index)->nscope;
	bool		due = false;

	sift_dir_read(index, all);
	for (int s = 0; s < nscope && !due; s++)
		due = scope_pick(index, all, s, force, sel) > 0;
	pfree(all);
	return due;
}
