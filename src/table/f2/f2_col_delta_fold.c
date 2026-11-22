/*
 * table/f2/f2_col_delta_fold.c - the column deltas of a partition folded into the parts its compaction rewrites (the host side of SiftCompactFold, col/f2/compact.h).
 *
 * open reads the chain once and classifies it (f2_col_delta_compact.c): the records of the parts rewritten whose writers committed before the horizon are folds, applied to the rows
 * of a granule as the compaction reads it. Under the partition lock the chain is read again and classified with only the records seen by open allowed to fold (the others came while
 * the parts were written): what stays is written as the chain of the new parts, its records of a rewritten part sent to the row it is now.
 */
#include "postgres.h"

#include "access/transam.h"
#include "port/pg_bitutils.h"

#include "col/f2/compact.h"
#include "core/crash_point.h"
#include "core/u64set.h"
#include "core/xid_cache.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_tbl.h"

typedef struct Fold
{
	Relation	rel;
	const SiftF2Store *store;
	SiftXidHorizonCache hc;
	SiftColDeltaHorizon hz;
	uint64	   *gens;			/* of the parts rewritten, ascending */
	int			ngens;
	SiftColDeltaChain chain;
	SiftColDeltaCompactPlan plan;
	uint32		nrecords;
	bool	   *folded_leaf;
	uint64	   *folded_gens;	/* ascending */
	int			nfolded_gens;
} Fold;

/* The leaf a patch is of: a sift_col index names it (its attribute is the leaf plus one), a table by the number of its column; -1 for the address of the row */
static int
patch_leaf(const SiftF2Store *store, int16 attnum)
{
	if (attnum == SIFT_CD_ATT_TID_REMAP)
		return -1;
	return store->host->host_kind == SIFT_F2_HOST_INDEX ? attnum - 1 : store->att_first[attnum];
}

static bool
in_sorted(const uint64 *v, int n, uint64 x)
{
	int			lo = 0,
				hi = n;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (v[mid] < x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < n && v[lo] == x;
}

static int
gen_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

static void
note_folds(Fold *f)
{
	f->folded_leaf = palloc0(sizeof(bool) * Max(f->store->ncols, 1));
	f->folded_gens = palloc(sizeof(uint64) * Max(f->plan.nfolds, 1));
	for (int i = 0; i < f->plan.nfolds; i++)
	{
		const SiftColDeltaFold *d = &f->plan.folds[i];
		int			leaf = patch_leaf(f->store, d->patch.attnum);

		if (leaf >= 0)
			f->folded_leaf[leaf] = true;
		if (f->nfolded_gens == 0 || f->folded_gens[f->nfolded_gens - 1] != d->part_gen)
			f->folded_gens[f->nfolded_gens++] = d->part_gen;
	}
}

static void *
fold_open(Relation rel, const SiftPartitionEntry *entry, const SiftPartDesc *parts, int nparts, const uint64 *gens, int ngens, FullTransactionId horizon)
{
	Fold	   *f;
	uint64	   *live;

	if ((entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return NULL;
	f = palloc0(sizeof(Fold));
	if (!sift_col_delta_load_chain(rel, entry, 0, &f->chain))
		return NULL;
	f->rel = rel;
	f->store = sift_f2_store_open(rel);
	sift_xid_horizon_init(&f->hc, horizon);
	f->hz = (SiftColDeltaHorizon) {.ctx = &f->hc,.status = sift_col_delta_horizon_status};
	f->gens = memcpy(palloc(sizeof(uint64) * Max(ngens, 1)), gens, sizeof(uint64) * ngens);
	f->ngens = ngens;
	qsort(f->gens, ngens, sizeof(uint64), gen_cmp);
	live = sift_col_delta_sorted_gens(parts, nparts);
	sift_col_delta_compact_plan(&f->chain, f->gens, f->ngens, live, nparts, &f->hz, UINT32_MAX, &f->plan);
	f->nrecords = (uint32) f->plan.nrecords;
	note_folds(f);
	return f;
}

static const bool *
fold_folded_leaves(void *fold)
{
	return ((Fold *) fold)->folded_leaf;
}

static bool
fold_folds_part(void *fold, uint64 part_gen)
{
	Fold	   *f = fold;

	return in_sorted(f->folded_gens, f->nfolded_gens, part_gen);
}

static void
release_wide(Relation rel, const char *ref, uint32 len, SiftExtent **release, int *nrelease)
{
	SiftExtVec	v = {0};

	sift_tbl_blob_release_ref(rel, ref, len, &v);
	*release = repalloc(*release, sizeof(SiftExtent) * pg_nextpower2_32(Max(16, *nrelease + v.n + 1)));
	memcpy(*release + *nrelease, v.ext, sizeof(SiftExtent) * v.n);
	*nrelease += v.n;
}

static void
fold_apply(void *fold, uint64 part_gen, uint32 granule_id, SiftRowBuf *rows, SiftExtent **release, int *nrelease)
{
	Fold	   *f = fold;
	int			lo,
				hi;

	sift_col_delta_fold_slice(&f->plan, part_gen, granule_id, &lo, &hi);
	for (int i = lo; i < hi; i++)
	{
		int			leaf = patch_leaf(f->store, f->plan.folds[i].patch.attnum);
		const char *old;
		uint32		oldlen;

		if (f->plan.folds[i].patch.attnum == SIFT_CD_ATT_TID_REMAP)
			rows->tid[f->plan.folds[i].patch.row_ordinal] = (uint64) f->plan.folds[i].patch.val.i;
		else if (leaf >= 0 && sift_col_delta_fold_apply(rows, leaf, &f->plan.folds[i], &old, &oldlen))
			release_wide(f->rel, old, oldlen, release, nrelease);
	}
}

/* ---- where the rows went ---- */

typedef struct Placed
{
	uint64		tid;
	uint64		part_gen;
	uint32		granule_id;
	uint16		row;
} Placed;

typedef struct Placement
{
	Placed	   *v;
	int			n;
} Placement;

static int
placed_cmp(const void *a, const void *b)
{
	uint64		x = ((const Placed *) a)->tid,
				y = ((const Placed *) b)->tid;

	return x < y ? -1 : x > y;
}

static bool
placed_at(void *ctx, uint64 tid, uint64 *part_gen, uint32 *granule_id, uint16 *row)
{
	const Placement *pl = ctx;
	Placed		key = {.tid = tid};
	const Placed *p = bsearch(&key, pl->v, pl->n, sizeof(Placed), placed_cmp);

	if (p == NULL)
		return false;
	*part_gen = p->part_gen;
	*granule_id = p->granule_id;
	*row = p->row;
	return true;
}

/* The addresses the records that move name: sorted, no duplicate */
static uint64 *
wanted_tids(const SiftColDeltaCompactPlan *plan, int *n)
{
	uint64	   *v = palloc(sizeof(uint64) * Max(plan->ncarry, 1));
	int			k = 0;

	for (int i = 0; i < plan->ncarry; i++)
	{
		SiftColDeltaEntryHdr h;

		if (!plan->carry[i].remap)
			continue;
		memcpy(&h, plan->carry[i].rec, sizeof(h));
		v[k++] = sift_col_delta_hdr_tid(&h);
	}
	qsort(v, k, sizeof(uint64), gen_cmp);
	*n = 0;
	for (int i = 0; i < k; i++)
		if (*n == 0 || v[*n - 1] != v[i])
			v[(*n)++] = v[i];
	return v;
}

static bool
wanted_in(const uint64 *want, int n, uint64 lo, uint64 hi)
{
	int			a = 0,
				b = n;

	while (a < b)
	{
		int			mid = a + (b - a) / 2;

		if (want[mid] < lo)
			a = mid + 1;
		else
			b = mid;
	}
	return a < n && want[a] <= hi;
}

/* Every row of the outputs that a record to move names, found by the TID index of each part and the addresses of the granules it cannot exclude */
static Placement
place_rows(Relation rel, uint64 part_id, const SiftPartDesc *out, int nout, const uint64 *want, int nwant)
{
	Placement	pl = {.v = palloc(sizeof(Placed) * Max(nwant, 1))};

	for (int k = 0; k < nout && nwant > 0; k++)
	{
		SiftPart   *part = sift_part_open(rel, out[k].header_blk, out[k].part_gen, (uint32) part_id);
		SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);
		uint64		len;
		const SiftTidIndexEntry *tix = sift_part_read_section(rel, part, SEC_TID_INDEX, &len);

		SIFT_INVARIANT(tix != NULL && len == (uint64) part->hdr.ngranules * sizeof(*tix), "a part written by a compaction has no TID index");
		for (uint32 g = 0; g < part->hdr.ngranules; g++)
		{
			uint64	   *tids;

			if (!wanted_in(want, nwant, tix[g].tid_min, tix[g].tid_max))
				continue;
			tids = sift_part_read_tids(rel, part, dir, g);
			for (uint16 r = 0; r < dir[g].nrows; r++)
				if (in_sorted(want, nwant, tids[r]))
					pl.v[pl.n++] = (Placed) {tids[r], out[k].part_gen, g, r};
		}
	}
	qsort(pl.v, pl.n, sizeof(Placed), placed_cmp);
	return pl;
}

static void
fold_commit(void *fold, const SiftPartitionEntry *entry, const SiftPartDesc *parts, int nparts, const SiftPartDesc *out, int nout, SiftManifestEdit *edit, SiftExtent **release,
			int *nrelease, FullTransactionId *stripped)
{
	Fold	   *f = fold;
	SiftColDeltaChain chain;
	SiftColDeltaCompactPlan plan;
	SiftColDeltaBatch *batches;
	SiftColDeltaAnchor anchor;
	FullTransactionId next = ReadNextFullTransactionId();
	uint64	   *live = sift_col_delta_sorted_gens(parts, nparts),
			   *want;
	int			nwant,
				nbatches;
	Placement	pl;
	SiftColDeltaRemap remap;
	bool		has_chain = sift_col_delta_load_chain(f->rel, entry, 0, &chain);

	SIFT_INVARIANT(has_chain, "the chain of a partition went while its compaction held the lock");
	sift_col_delta_compact_plan(&chain, f->gens, f->ngens, live, nparts, &f->hz, f->nrecords, &plan);
	want = wanted_tids(&plan, &nwant);
	pl = place_rows(f->rel, entry->part_id, out, nout, want, nwant);
	remap = (SiftColDeltaRemap) {.ctx = &pl,.locate = placed_at};
	batches = sift_col_delta_compact_carry(&plan, next, &remap, &nbatches);
	sift_col_delta_write_chain(f->rel, entry, batches, nbatches, next, &anchor);
	edit->set_col_delta = true;
	edit->new_col_delta = anchor;
	SIFT_CRASH_POINT(f2_col_delta_fold_pre_p2);
	*release = repalloc(*release, sizeof(SiftExtent) * (*nrelease + chain.npages + 1));
	for (int i = 0; i < chain.npages; i++)
		(*release)[(*nrelease)++] = (SiftExtent) {chain.blks[i], 1};
	*stripped = sift_fxid_max(*stripped, plan.stripped);
}

static const SiftCompactFold col_delta_fold = {
	.open = fold_open,.folded_leaves = fold_folded_leaves,.folds_part = fold_folds_part,.apply = fold_apply,.commit = fold_commit,
};

void
sift_col_delta_fold_install(void)
{
	sift_compact_fold = &col_delta_fold;
}

/* ---- the parts VACUUM sends to the compaction ---- */

int
sift_col_delta_fold_gens(Relation rel, const SiftPartitionEntry *entry, const SiftPartDesc *parts, int nparts, FullTransactionId horizon, uint64 *gens, int ngens)
{
	SiftColDeltaChain chain;
	SiftColDeltaCompactPlan plan;
	SiftXidHorizonCache hc;
	SiftColDeltaHorizon hz = {.ctx = &hc,.status = sift_col_delta_horizon_status};
	uint64	   *live = sift_col_delta_sorted_gens(parts, nparts);
	SiftU64Set	listed;

	if (!sift_col_delta_load_chain(rel, entry, 0, &chain))
		return ngens;
	sift_xid_horizon_init(&hc, horizon);
	sift_col_delta_compact_plan(&chain, live, nparts, live, nparts, &hz, UINT32_MAX, &plan);
	/* a part in object storage is not rewritten: its deltas stay in the chain and are applied when it is read */
	sift_u64set_init(&listed, nparts + ngens + plan.nfolds);
	for (int k = 0; k < ngens; k++)
		sift_u64set_add(&listed, gens[k]);
	for (int i = 0; i < nparts; i++)
		if (parts[i].ext_flags & SIFT_PART_EXT_REMOTE)
			sift_u64set_add(&listed, parts[i].part_gen);
	for (int i = 0; i < plan.nfolds; i++)
		if (sift_u64set_add(&listed, plan.folds[i].part_gen))
			gens[ngens++] = plan.folds[i].part_gen;
	sift_u64set_free(&listed);
	sift_col_delta_compact_plan_free(&plan);
	sift_col_delta_chain_free(&chain);
	return ngens;
}
