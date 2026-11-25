/*
 * table/f2/col_delta/fold.c - the column deltas of a partition folded into the parts its compaction rewrites (the host side of SiftCompactFold, col/f2/compact/compact.h).
 *
 * open reads the chain once, a page at a time, and classifies it (f2_col_delta_compact.c): the records of the parts rewritten whose writers committed before the horizon are folds, and
 * what it keeps of them is which parts and leaves they patch and the pages the chain has. A granule is folded by reading the pages that can hold its records, again, as the compaction
 * reads it: a page at a time, the rows of its records that stay held back from the fold by the pass that classifies them. Under the partition lock the chain is read once more and classified
 * with only the records seen by open allowed to fold (the others came while the parts were written): what stays is written as the chain of the new parts, its records of a rewritten part
 * sent to the row it is now. Nothing of the chain is held but the page being read and the records that stay.
 */
#include "postgres.h"

#include "access/transam.h"
#include "port/pg_bitutils.h"
#include "storage/lmgr.h"

#include "col/f2/compact/compact.h"
#include "core/crash_point.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "core/part_lock.h"
#include "core/u64set.h"
#include "core/xid_cache.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/col_delta/dir.h"
#include "table/f2/tbl.h"

/* ---- the pages of a chain, one at a time ---- */

/* What is known of a page without reading it again: where its records start in the chain, how many there are, and the part and granules they are for (part_gen 0: several parts) */
typedef struct TrailPage
{
	BlockNumber blk;
	uint32		first_seq;
	uint32		nrecords;
	uint64		part_gen;
	uint32		granule_lo;
	uint32		granule_hi;
} TrailPage;

typedef struct Trail
{
	Relation	rel;
	uint64		part_id;
	SiftColDeltaDiskReader disk;
	SiftColDeltaPageReader reader;
	TrailPage  *pages;			/* oldest first */
	int			npages;
	BlockNumber *dir_blks;
	int			ndir;
	uint32		nrecords;
	FullTransactionId min_xid;	/* the oldest writer of the pages (invalid with none) */
	PGAlignedBlock page;		/* the page that was read last */
} Trail;

static void
trail_free(Trail *t)
{
	if (t->pages != NULL)
		pfree(t->pages);
	if (t->dir_blks != NULL)
		pfree(t->dir_blks);
	t->pages = NULL;
	t->dir_blks = NULL;
	t->npages = t->ndir = 0;
}

static const char *
trail_read(Trail *t, BlockNumber blk)
{
	const char *why = t->reader.read(t->reader.ctx, blk, t->page.data);

	return why != NULL ? why : sift_col_delta_page_check(t->page.data);
}

/* The blocks of a chain without a directory, from the newest page along the links */
static const char *
flat_blocks(Trail *t, const SiftColDeltaAnchor *anchor, BlockNumber *blks)
{
	BlockNumber blk = anchor->delta_head_blkno;
	const char *why = NULL;

	for (int i = anchor->npages - 1; i >= 0 && why == NULL; i--)
	{
		blks[i] = blk;
		why = trail_read(t, blk);
		blk = sift_col_delta_page_hdr(t->page.data)->prev_blk;
	}
	return why;
}

static const char *
trail_summarize(Trail *t, const BlockNumber *blks)
{
	const char *why = NULL;

	for (int i = 0; i < t->npages && why == NULL; i++)
	{
		SiftColDeltaBatch seen;
		FullTransactionId page_min;

		why = trail_read(t, blks[i]);
		if (why == NULL)
			why = sift_col_delta_check_link(t->page.data, i + 1, i == 0 ? InvalidBlockNumber : blks[i - 1]);
		if (why != NULL)
			break;
		page_min = sift_col_delta_page_hdr(t->page.data)->min_xid;
		if (FullTransactionIdIsValid(page_min) && (!FullTransactionIdIsValid(t->min_xid) || FullTransactionIdPrecedes(page_min, t->min_xid)))
			t->min_xid = page_min;
		sift_col_delta_page_summary(t->page.data, &seen);
		t->pages[i] = (TrailPage) {blks[i], t->nrecords, seen.nentries, seen.part_gen, seen.granule_lo, seen.granule_hi};
		t->nrecords += seen.nentries;
	}
	return why;
}

/* The pages the entry's chain has, checked as they are read; false when it has none */
static bool
trail_open(Trail *t, Relation rel, const SiftPartitionEntry *entry)
{
	SiftColDeltaAnchor anchor;
	BlockNumber *blks;
	const char *why;

	memset(t, 0, offsetof(Trail, page));
	t->rel = rel;
	t->part_id = entry->part_id;
	t->disk = (SiftColDeltaDiskReader) {.rel = rel,.part_id = (uint32) entry->part_id};
	t->reader = sift_col_delta_disk_reader(&t->disk);
	if ((entry->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return false;
	anchor = sift_manifest_read_col_delta(rel, entry);
	if (!sift_col_delta_anchor_has_chain(&anchor))
		return false;
	why = sift_col_delta_anchor_sane(&anchor) ? NULL : "the anchor of a chain is not one of this format";
	t->npages = anchor.npages;
	blks = palloc(sizeof(BlockNumber) * t->npages);
	t->pages = palloc(sizeof(TrailPage) * t->npages);
	if (why == NULL && (anchor.flags & SIFT_CDLA_INDEXED_DIR) != 0)
	{
		SiftColDeltaDirEntry *ent;

		why = sift_cddir_chain_entries(&anchor, &t->reader, &ent, &t->dir_blks, &t->ndir);
		for (int i = 0; why == NULL && i < t->npages; i++)
			blks[i] = ent[i].blkno;
		if (why == NULL)
			pfree(ent);
	}
	else if (why == NULL)
		why = flat_blocks(t, &anchor, blks);
	if (why == NULL)
		why = trail_summarize(t, blks);
	if (why != NULL)
		SIFT_CORRUPT(rel, "%s (partition %llu).", why, (unsigned long long) entry->part_id);
	pfree(blks);
	return true;
}

/* The page i of the chain, in the one buffer of the trail: it is there until the next is read */
static const char *
trail_page(void *ctx, int i, uint32 *nrecords)
{
	Trail	   *t = ctx;
	const char *why = trail_read(t, t->pages[i].blk);

	if (why != NULL)
		SIFT_CORRUPT(t->rel, "%s (partition %llu).", why, (unsigned long long) t->part_id);
	*nrecords = t->pages[i].nrecords;
	return t->page.data;
}

/* The data pages and the directory pages: what the chain frees when it goes */
static SiftExtent *
trail_extents(const Trail *t, int *n)
{
	SiftExtent *x = palloc(sizeof(SiftExtent) * Max(t->npages + t->ndir, 1));

	for (int i = 0; i < t->npages; i++)
		x[i] = (SiftExtent) {t->pages[i].blk, 1};
	for (int i = 0; i < t->ndir; i++)
		x[t->npages + i] = (SiftExtent) {t->dir_blks[i], 1};
	*n = t->npages + t->ndir;
	return x;
}

/* The records of one page, the first nrecords of them, numbered from seq */
typedef struct RecIter
{
	const uint8 *next;
	const uint8 *end;
	uint32		left;
	uint32		nextseq;
	const uint8 *rec;
	SiftColDeltaEntryHdr h;
	uint32		seq;
} RecIter;

static RecIter
rec_iter(const char *page, uint32 nrecords, uint32 first_seq)
{
	return (RecIter) {.next = sift_col_delta_page_first(page),.end = sift_col_delta_page_end(page),.left = nrecords,.nextseq = first_seq};
}

static bool
rec_next(RecIter *it)
{
	if (it->next >= it->end || it->left == 0)
		return false;
	it->rec = it->next;
	memcpy(&it->h, it->rec, sizeof(it->h));
	it->next += it->h.rec_bytes;
	it->left--;
	it->seq = it->nextseq++;
	return true;
}

/* The records of page i that count */
static RecIter
trail_iter(Trail *t, int i)
{
	uint32		count;
	const char *page = trail_page(t, i, &count);

	return rec_iter(page, count, t->pages[i].first_seq);
}

/* ---- the fold ---- */

typedef struct Fold
{
	Relation	rel;
	const SiftF2Store *store;
	SiftXidHorizonCache hc;
	SiftColDeltaHorizon hz;
	uint64	   *gens;			/* of the parts rewritten, ascending */
	int			ngens;
	uint64	   *live;			/* of every part of the partition, ascending */
	int			nlive;
	bool		had_chain;
	Trail		trail;			/* the chain as open read it */
	uint32		nrecords;		/* its records: the ones that may fold under the lock */
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

static int
gen_at(const uint64 *v, int n, uint64 x)
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
	return lo;
}

static bool
in_sorted(const uint64 *v, int n, uint64 x)
{
	int			at = gen_at(v, n, x);

	return at < n && v[at] == x;
}

static int
gen_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/* The columns of the record, as the patches they are for the row of the record */
static void
rec_patches(const RecIter *it, SiftColDeltaPatch *out, int k)
{
	const uint8 *p = it->rec + sizeof(SiftColDeltaEntryHdr);

	for (int c = 0; c <= k; c++)
	{
		p = sift_col_delta_decode_col(p, it->rec + it->h.rec_bytes, out);
		SIFT_INVARIANT(p != NULL, "a column delta page was read before it was checked");
	}
	out->row_ordinal = it->h.row_ordinal;
}

/* What the records that fold are: the leaves they patch and the parts they are of (has: a flag for each of the parts rewritten) */
static void
note_fold(Fold *f, const RecIter *it, bool *has)
{
	for (int k = 0; k < it->h.ncols; k++)
	{
		SiftColDeltaPatch pt;
		int			leaf;

		rec_patches(it, &pt, k);
		leaf = patch_leaf(f->store, pt.attnum);
		if (leaf >= 0)
			f->folded_leaf[leaf] = true;
	}
	has[gen_at(f->gens, f->ngens, it->h.part_gen)] = true;
}

/* One pass over the chain as open reads it: its records, and the leaves and parts of those that fold */
static void
survey(Fold *f)
{
	SiftColDeltaPass *pass = sift_col_delta_pass_begin(f->gens, f->ngens, f->live, f->nlive, &f->hz, UINT32_MAX);
	bool	   *has = palloc0(sizeof(bool) * Max(f->ngens, 1));

	for (int i = 0; i < f->trail.npages; i++)
	{
		RecIter		it = trail_iter(&f->trail, i);

		while (rec_next(&it))
		{
			bool		settled;
			FullTransactionId fxid;

			if (sift_col_delta_pass_class(pass, it.rec, it.seq, &settled, &fxid) == SIFT_CDC_FOLD)
				note_fold(f, &it, has);
		}
	}
	sift_col_delta_pass_end(pass);
	f->folded_gens = palloc(sizeof(uint64) * Max(f->ngens, 1));
	for (int g = 0; g < f->ngens; g++)
		if (has[g])
			f->folded_gens[f->nfolded_gens++] = f->gens[g];
	pfree(has);
}

static void *
fold_open(Relation rel, const SiftPartitionEntry *entry, const SiftPartDesc *parts, int nparts, const uint64 *gens, int ngens, FullTransactionId horizon)
{
	Fold	   *f = palloc0(sizeof(Fold));

	f->rel = rel;
	f->store = sift_f2_store_open(rel);
	sift_xid_horizon_init(&f->hc, horizon);
	f->hz = (SiftColDeltaHorizon) {.ctx = &f->hc,.status = sift_col_delta_horizon_status};
	f->gens = memcpy(palloc(sizeof(uint64) * Max(ngens, 1)), gens, sizeof(uint64) * ngens);
	f->ngens = ngens;
	qsort(f->gens, ngens, sizeof(uint64), gen_cmp);
	f->live = sift_col_delta_sorted_gens(parts, nparts);
	f->nlive = nparts;
	f->folded_leaf = palloc0(sizeof(bool) * Max(f->store->ncols, 1));
	f->had_chain = trail_open(&f->trail, rel, entry);
	if (f->had_chain)
		survey(f);
	f->nrecords = f->trail.nrecords;
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
	pfree(v.ext);
}

/* The record laid over the rows; a patch of a record later than another of the row is laid over it, the latest of a column is the one the row keeps */
static void
fold_record(Fold *f, const RecIter *it, SiftRowBuf *rows, SiftExtent **release, int *nrelease)
{
	for (int k = 0; k < it->h.ncols; k++)
	{
		SiftColDeltaPatch pt;
		int			leaf;
		const char *old;
		uint32		oldlen;

		rec_patches(it, &pt, k);
		leaf = patch_leaf(f->store, pt.attnum);
		if (pt.attnum == SIFT_CD_ATT_TID_REMAP)
			sift_rowbuf_set_tid(rows, pt.row_ordinal, (uint64) pt.val.i);
		else if (leaf >= 0 && sift_col_delta_fold_apply(rows, leaf, &pt, &old, &oldlen))
			release_wide(f->rel, old, oldlen, release, nrelease);
	}
}

/* The pages that can hold a record of the granule are read again, in the order of the chain as open saw it, and classified as open did: only the rows of the granule are held back */
static void
fold_apply(void *fold, uint64 part_gen, uint32 granule_id, SiftRowBuf *rows, SiftExtent **release, int *nrelease)
{
	Fold	   *f = fold;
	SiftColDeltaPass *pass;

	if (!fold_folds_part(f, part_gen))
		return;
	pass = sift_col_delta_pass_begin(f->gens, f->ngens, f->live, f->nlive, &f->hz, UINT32_MAX);
	for (int i = 0; i < f->trail.npages; i++)
	{
		const TrailPage *tp = &f->trail.pages[i];
		RecIter		it;

		if ((tp->part_gen != 0 && tp->part_gen != part_gen) || granule_id < tp->granule_lo || granule_id > tp->granule_hi)
			continue;
		it = trail_iter(&f->trail, i);
		while (rec_next(&it))
		{
			bool		settled;
			FullTransactionId fxid;

			if (it.h.part_gen == part_gen && it.h.granule_id == granule_id && sift_col_delta_pass_class(pass, it.rec, it.seq, &settled, &fxid) == SIFT_CDC_FOLD)
				fold_record(f, &it, rows, release, nrelease);
		}
	}
	sift_col_delta_pass_end(pass);
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
	/* MEM[MEM-TBL-02]: col_delta fold per-granule scratch ctx (reset each granule in place_rows) */
	MemoryContext pcx = sift_mem_allocset_create(CurrentMemoryContext, "sift column delta fold part"),
				gcx = sift_mem_allocset_create(CurrentMemoryContext, "sift column delta fold granule"),
				outer = CurrentMemoryContext;

	for (int k = 0; k < nout && nwant > 0; k++)
	{
		SiftPart   *part;
		SiftGranuleDesc *dir;
		uint64		len;
		const SiftTidIndexEntry *tix;

		MemoryContextSwitchTo(pcx);
		part = sift_part_open(rel, out[k].header_blk, out[k].part_gen, (uint32) part_id);
		dir = sift_part_granule_dir(rel, part);
		tix = sift_part_read_section(rel, part, SEC_TID_INDEX, &len);
		SIFT_INVARIANT(tix != NULL && len == (uint64) part->hdr.ngranules * sizeof(*tix), "a part written by a compaction has no TID index");
		for (uint32 g = 0; g < part->hdr.ngranules; g++)
		{
			uint64	   *tids;

			if (!wanted_in(want, nwant, tix[g].tid_min, tix[g].tid_max))
				continue;
			MemoryContextSwitchTo(gcx);
			tids = sift_part_read_tids(rel, part, dir, g);
			for (uint16 r = 0; r < dir[g].nrows; r++)
				if (in_sorted(want, nwant, tids[r]))
					pl.v[pl.n++] = (Placed) {tids[r], out[k].part_gen, g, r};
			MemoryContextReset(gcx);
			MemoryContextSwitchTo(pcx);
		}
		MemoryContextSwitchTo(outer);
		MemoryContextReset(pcx);
	}
	MemoryContextDelete(gcx);
	MemoryContextDelete(pcx);
	qsort(pl.v, pl.n, sizeof(Placed), placed_cmp);
	return pl;
}

static void
fold_commit(void *fold, const SiftPartitionEntry *entry, const SiftPartDesc *parts, int nparts, const SiftPartDesc *out, int nout, SiftManifestEdit *edit, SiftExtent **release,
			int *nrelease, FullTransactionId *stripped)
{
	Fold	   *f = fold;
	Trail		t;
	SiftColDeltaPageSource src = {.ctx = &t,.page = trail_page};
	SiftColDeltaCompactPlan plan;
	SiftColDeltaAnchor anchor;
	FullTransactionId next = ReadNextFullTransactionId();
	uint64	   *live = sift_col_delta_sorted_gens(parts, nparts);
	int			nfreed;
	SiftExtent *freed;
	Placement	pl;
	SiftColDeltaRemap remap;
	bool		has_chain;

	trail_free(&f->trail);
	has_chain = trail_open(&t, f->rel, entry);
	SIFT_INVARIANT(has_chain || !f->had_chain, "the chain of a partition went while its compaction held the lock");
	if (!has_chain)
		return;
	src.npages = t.npages;
	sift_xid_horizon_init(&f->hc, f->hc.horizon);
	if (!sift_col_delta_compact_changes(&src, f->gens, f->ngens, live, nparts, &f->hz, f->nrecords))
	{
		trail_free(&t);
		return;
	}
	sift_col_delta_compact_stream(&src, f->gens, f->ngens, live, nparts, &f->hz, f->nrecords, &plan);
	pl = place_rows(f->rel, entry->part_id, out, nout, plan.want, plan.nwant);
	remap = (SiftColDeltaRemap) {.ctx = &pl,.locate = placed_at};
	sift_col_delta_write_carry(f->rel, entry, &plan, &src, next, &remap, &anchor);
	pfree(pl.v);
	edit->set_col_delta = true;
	edit->new_col_delta = anchor;
	SIFT_CRASH_POINT(f2_col_delta_fold_pre_p2);
	freed = trail_extents(&t, &nfreed);
	*release = repalloc(*release, sizeof(SiftExtent) * (*nrelease + nfreed + 1));
	memcpy(*release + *nrelease, freed, sizeof(SiftExtent) * nfreed);
	*nrelease += nfreed;
	pfree(freed);
	*stripped = sift_fxid_max(*stripped, plan.stripped);
	sift_col_delta_compact_plan_free(&plan);
	trail_free(&t);
}

static const SiftCompactFold col_delta_fold = {
	.open = fold_open,.folded_leaves = fold_folded_leaves,.folds_part = fold_folds_part,.apply = fold_apply,.commit = fold_commit,
};

void
sift_col_delta_fold_install(void)
{
	sift_compact_fold = &col_delta_fold;
}

/* ---- the records that no part has a use for ---- */

static void
count_page(void *ctx, SiftColDeltaBatch *page)
{
	(*(int *) ctx)++;
	sift_col_delta_batch_free(page);
}

/* The records that stay, put on fewer pages than the chain has, in the one change of the manifest that replaces the chain; false when they would not take fewer */
static bool
shrink_chain(Relation rel, const SiftPartitionEntry *e, const Trail *t, const SiftColDeltaPageSource *src, const SiftColDeltaCompactPlan *plan)
{
	FullTransactionId next = ReadNextFullTransactionId();
	SiftColDeltaAnchor anchor;
	SiftExtent *release;
	int			pages = 0,
				nrelease;

	sift_col_delta_compact_carry(plan, src, next, NULL, count_page, &pages);
	if (pages >= t->npages)
		return false;
	sift_col_delta_write_carry(rel, e, plan, src, next, NULL, &anchor);
	release = trail_extents(t, &nrelease);
	sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = e->part_id,.key_lo = e->key_lo,.set_col_delta = true,.new_col_delta = anchor,.release = release,.nrelease = nrelease});
	pfree(release);
	return true;
}

/*
 * Every record is an aborted writer's or a gone part's: the pages go and the partition has no chain, and no row of any part is written. False when a record stays; the records that stay
 * are then written onto fewer pages when the chain holds dead ones.
 */
bool
sift_col_delta_gc(Relation rel, uint64 part_id, const uint8 *key_lo, FullTransactionId oldest_fx)
{
	SiftPartitionEntry e;
	Trail		t;
	SiftColDeltaPageSource src = {.ctx = &t,.page = trail_page};
	SiftColDeltaCompactPlan plan;
	SiftPartDesc *parts;
	SiftXidHorizonCache hc;
	SiftColDeltaHorizon hz = {.ctx = &hc,.status = sift_col_delta_horizon_status};
	uint64	   *live;
	int			nparts;
	bool		gone;

	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	e = sift_col_delta_fresh_entry(rel, part_id, key_lo);
	if (!trail_open(&t, rel, &e))
	{
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		return false;
	}
	src.npages = t.npages;
	parts = sift_manifest_read_parts(rel, &e, &nparts);
	live = sift_col_delta_sorted_gens(parts, nparts);
	sift_xid_horizon_init(&hc, oldest_fx);
	sift_col_delta_compact_stream(&src, NULL, 0, live, nparts, &hz, UINT32_MAX, &plan);
	gone = plan.ncarry == 0;
	if (gone)
	{
		int			nrelease;
		SiftExtent *release = trail_extents(&t, &nrelease);

		sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = part_id,.key_lo = e.key_lo,.set_col_delta = true,.new_col_delta = sift_col_delta_anchor_none(),.release = release,
													   .nrelease = nrelease});
		pfree(release);
	}
	/* STOR[STOR-TBL-08]: partial col_delta chain compaction (reclaim up to 31/32 dead 8KiB pages when ncarry>0) */
	else if (plan.ndropped > 0)
		(void) shrink_chain(rel, &e, &t, &src, &plan);
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	sift_col_delta_compact_plan_free(&plan);
	trail_free(&t);
	pfree(live);
	pfree(parts);
	return gone;
}

FullTransactionId
sift_col_delta_min_xid(Relation rel, const SiftPartitionEntry *entry)
{
	Trail		t;
	FullTransactionId lo;

	if (!trail_open(&t, rel, entry))
		return InvalidFullTransactionId;
	lo = t.min_xid;
	trail_free(&t);
	return lo;
}

/* ---- the parts VACUUM sends to the compaction ---- */

int
sift_col_delta_fold_gens(Relation rel, const SiftPartitionEntry *entry, const SiftPartDesc *parts, int nparts, FullTransactionId horizon, bool aggressive, uint64 *gens, int ngens)
{
	Trail		t;
	SiftXidHorizonCache hc;
	SiftColDeltaHorizon hz = {.ctx = &hc,.status = sift_col_delta_horizon_status};
	uint64	   *live = sift_col_delta_sorted_gens(parts, nparts),
			   *found = palloc(sizeof(uint64) * Max(nparts, 1));
	SiftColDeltaPass *pass;
	SiftU64Set	listed;
	int			nfound = 0;
	bool		stays = false;

	if (!trail_open(&t, rel, entry))
		return ngens;
	pass = sift_col_delta_pass_begin(live, nparts, live, nparts, &hz, UINT32_MAX);
	sift_xid_horizon_init(&hc, horizon);
	/* a part in object storage is not rewritten: its deltas stay in the chain and are applied when it is read */
	sift_u64set_init(&listed, nparts + ngens);
	for (int k = 0; k < ngens; k++)
		sift_u64set_add(&listed, gens[k]);
	for (int i = 0; i < nparts; i++)
		if (parts[i].ext_flags & SIFT_PART_EXT_REMOTE)
			sift_u64set_add(&listed, parts[i].part_gen);
	for (int i = 0; i < t.npages && !stays; i++)
	{
		RecIter		it = trail_iter(&t, i);

		while (rec_next(&it))
		{
			bool		settled;
			FullTransactionId fxid;
			SiftColDeltaClass cls = sift_col_delta_pass_class(pass, it.rec, it.seq, &settled, &fxid);

			if (cls == SIFT_CDC_FOLD && sift_u64set_add(&listed, it.h.part_gen))
				found[nfound++] = it.h.part_gen;
			stays |= (cls == SIFT_CDC_CARRY_REMAP || cls == SIFT_CDC_CARRY_KEEP) && !aggressive && t.npages < SIFT_COL_DELTA_FOLD_PAGES;
		}
	}
	/* STOR[STOR-TBL-09]: a short chain is folded only when the fold empties it: the parts it patches are rewritten once and the partition has no chain left */
	if (!stays)
		for (int i = 0; i < nfound; i++)
			gens[ngens++] = found[i];
	sift_u64set_free(&listed);
	sift_col_delta_pass_end(pass);
	trail_free(&t);
	return ngens;
}
