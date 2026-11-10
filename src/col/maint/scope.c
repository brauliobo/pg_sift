/*
 * col/maint/scope.c - the maintenance of the scope segments of a scope index (col/scope.h)
 * the scope segments a promotion writes beside its entity segment, in the same switch of the directory
 * the merge of the scope segments of one scope, by the range of its key
 *
 * The scope segments are copies of the rows of the entity segments, once for each scope key of the member of a row, with the address of the row. So a merge of entity segments
 * leaves them where they are (the addresses stay), a promotion must write them with the entity segment (the rows have no entries else), and a VACUUM ends the entries of a dead
 * address in every segment. The entity segments that have their entries say so (SIFT_SEG_SCOPED): the stored read of a scope is exact only while every one does.
 */
#include "postgres.h"
#include "datatype/timestamp.h"
#include "utils/timestamp.h"
#include "core/ops.h"
#include "bm25/merge.h"
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

/*
 * The scope segments of the rows of batches[0..nb) (a promotion's), written with the fill of maint_scope_fill_begin: for each scope the rows replicated by the keys of their member,
 * sorted by the cut of the scope and cut into units of one key. out has room for one segment of each scope; the number of them is returned (a scope none of whose rows has a pair has none).
 */
int
maint_scope_write(Relation index, SiftWriteTarget *wt, struct SiftScopeFill *sf, SiftColFill *fill, SiftColBatch *const *batches, int nb, uint64 index_rows,
				  int format, MemoryContext cx, SiftSegEntry *out)
{
	const SiftColRel *r = sift_col_rel_private(index, cx);
	SiftCPaggSpec pagg = r->pagg;
	uint32		aim = sift_colk_aim_rows(index),
				cap = 0;
	uint32	   *row = NULL;
	int64	   *key = NULL;
	int			n = 0;

	pagg.lean = true;
	for (int s = 0; s < r->nscope; s++)
	{
		SiftColBatch *rep = sift_cbatch_create(cx, r->schema);
		SiftColSegWriter *w;
		int			cut[SIFT_COL_CUT_MAX],
					ncut = sift_scope_cut(r, s, cut);

		rep->keyed = rep->unsorted = true;
		for (int i = 0; i < nb; i++)
		{
			const SiftCVals *fk = &batches[i]->col[sift_scope_fill_fk_leaf(sf, s)];
			uint32		entries = sift_scope_fill_expand(sf, s, fk->key, fk->nulls, batches[i]->nrows, &row, &key, &cap);

			for (uint32 e = 0; e < entries; e++)
				sift_cbatch_add_scoped(rep, batches[i], row[e], r->scope_leaf[s], key[e]);
		}
		if (rep->nrows == 0)
			continue;
		w = sift_colw_begin(wt, sift_store_new_gen(index), r->schema, r->policy, format, cx);
		sift_colw_set_fill(w, fill);
		sift_colw_set_index_rows(w, index_rows);
		sift_colw_set_preagg(w, &pagg);
		sift_colw_set_keyed(w, aim, cut[0]);
		sift_colk_write(w, &rep, 1, r->schema, cut, ncut, sift_colk_unit_rows(aim), cx, 0);
		sift_colw_finish(w, &out[n]);
		out[n].flags |= SIFT_SEG_SCOPE;
		out[n++].pad = s;
	}
	return n;
}

/* Are the scope entries something the rewrite is to make again: an entity segment has none, or the journal of the relation says they are stale (sift_scope_stale) */
bool
maint_scope_refill_due(Relation index)
{
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	SiftScopeStale *st = sift_scope_stale_begin(index, CurrentMemoryContext);
	bool		due = false;

	for (int s = 0; s < sift_col_rel(index)->nscope && !due; s++)
		due = !sift_scope_covered(index, s);
	sift_dir_read(index, dir);
	for (int i = 0; i < dir->nsegs && !due; i++)
		if (sift_seg_is_scope(&dir->segs[i]))
		{
			SiftColSegment *seg = sift_cseg_open(index, &dir->segs[i], CurrentMemoryContext);

			due = sift_scope_stale(st, seg);
			sift_cseg_close(seg);
		}
	sift_scope_stale_end(st);
	pfree(dir);
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
} Refill;

static bool
in_gens(const uint64 *gen, int n, uint64 g)
{
	for (int i = 0; i < n; i++)
		if (gen[i] == g)
			return true;
	return false;
}

/* The one switch of the directory of a refill: the scope segments are replaced and the entity segments they were made from say they are covered */
static void
refill_switch(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	Refill	   *rf = arg;
	int			n = 0;

	for (int i = 0; i < dir->nsegs; i++)
	{
		if (in_gens(rf->old, rf->nold, dir->segs[i].gen))
			continue;
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE && in_gens(rf->ents, rf->nents, dir->segs[i].gen))
			dir->segs[i].flags |= SIFT_SEG_SCOPED;
		dir->segs[n++] = dir->segs[i];
	}
	if (n + rf->nneu > SIFT_MAX_SEGMENTS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the directory has no room for the %d scope segments this refill wrote", rf->nneu),
						errhint("Merge the segments first (sift.col_merge()).")));
	memcpy(dir->segs + n, rf->neu, rf->nneu * sizeof(SiftSegEntry));
	dir->nsegs = n + rf->nneu;
	meta->flags &= ~SIFT_META_FAILURES_MASK;
}

/* The runs a refill writes for each scope: a run holds a sixteenth of the rows at least, so that they fit the directory */
#define REFILL_RUNS	16

/*
 * The scope segments again, from the live rows of the entity segments, under a stamp taken now: the rows of each entity segment are decoded unit by unit, replicated by the keys of
 * their member and written as runs of the scope's cut, which the merge of the scope segments joins. The old scope segments go and the entity segments say they are covered in
 * one switch of the directory, so a crash leaves the index as it was. This is what the rewrite does when the entries are stale (the relation changed since their stamp, or an
 * entity segment lost its flag). False: no entity segment.
 */
bool
maint_scope_refill(Relation index)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col scope refill", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftColRel *r = sift_col_rel_private(index, cx);
	MemoryContext hcx = AllocSetContextCreate(cx, "sift_col scope refill run", ALLOCSET_DEFAULT_SIZES);	/* the rows of a run and its writer */
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
				nheld = 0,
				heldcap = 16;
	SiftColBatch **held = palloc(heldcap * sizeof(SiftColBatch *));
	TimestampTz start = GetCurrentTimestamp();
	struct SiftScopeFill *sf;
	SiftColFill *fill;
	SiftWriteTarget *wt;
	SiftMaintInputs mi;
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
	chunk = Max((uint64) maintenance_work_mem * 4, rows / REFILL_RUNS + 1);
	fill = maint_scope_fill_begin(index, cx, rows, &sf);
	wt = sift_wt_begin(index, SIFT_WRITE_SHARED);
	for (int i = 0; i <= nents; i++)
	{
		SiftColSegment *seg = i < nents ? sift_cseg_open(index, &ents[i], cx) : NULL;

		for (uint32 u = 0; seg != NULL && u < seg->nsb; u++)
		{
			SiftColBatch *b = sift_cbatch_create(hcx, r->schema);

			sift_cseg_decode_unit(seg, u, b);
			if (nheld == heldcap)
				held = repalloc(held, (heldcap *= 2) * sizeof(SiftColBatch *));
			held[nheld++] = b;
			held_rows += b->nrows;
			if (held_rows >= chunk)
			{
				nout += maint_scope_write(index, wt, sf, fill, held, nheld, rows, sift_col_format_minor, hcx, out + nout);
				nheld = 0;
				held_rows = 0;
				MemoryContextReset(hcx);
			}
		}
		if (seg != NULL)
			sift_cseg_close(seg);
		if (i == nents && nheld > 0)
			nout += maint_scope_write(index, wt, sf, fill, held, nheld, rows, sift_col_format_minor, hcx, out + nout);
	}
	sift_conn_fill_end(fill);
	sift_wt_finish(wt);
	mi = sift_maint_inputs(index, scopes, nscopes);
	rf = (Refill) {mi.gens, nscopes, out, nout, egen, nents};
	sift_dir_update(index, &(SiftDirChange) {.fn = refill_switch,.arg = &rf,.release = mi.release,.nrelease = mi.nrelease,.extra = InvalidBuffer});
	sift_log_maint(index, "refilled the scope segments of", start, rows, 0);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return true;
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
