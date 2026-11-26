/*
 * col/f2/uniq/slab.c - the hashes of the keys in the L0 slabs, for the writers of UNIQUE indexes (f2_uniq_slab.h).
 *
 * A slab of an index is appended to past its last reference and replaced as a whole when it is flushed, so what a backend knows of a slab stays right while the last reference it
 * folded is still the reference at that place of the chain: the new references are folded, and a chain whose reference there changed (the slab was flushed and began again) is folded from the
 * first. The stamp of the chain, read before its references, says whether anything was appended since: a stamp that is older than the references only makes the next look fold again.
 *
 * check-memory: allow-allocs the tables follow the references of the slabs of the index being written, at most SLAB_FILTERS of them, each at most SLAB_SLOTS wide and in the budget of core/cachebudget.h
 */
#include "postgres.h"

#include "access/tableam.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "col/f2/f2.h"
#include "col/f2/uniq/slab.h"
#include "col/f2/l0/slab.h"
#include "col/f2/l0/href_fetch.h"
#include "core/cachebudget.h"
#include "core/compat.h"
#include "core/f2/manifest/manifest.h"
#include "core/page_io.h"

#define SLAB_FILTERS	32
#define SLAB_SLOTS		16384		/* half of it holds the hashes of a slab: its references are at most two statements of SIFT_L0_HEAPREF_BATCH_MAX */
#define SLAB_SLOTS_MIN	256

typedef struct SlabFilter
{
	MemoryContext cx;
	RelFileLocator locator;
	uint64		part_id;
	uint64		used;			/* sift_cache_tick() of its last use */
	SiftChainStamp stamp;
	bool		stamped;
	BlockNumber tail;
	uint32		nrefs;			/* the references of the chain folded so far */
	SiftL0HeapRef last;			/* the last of them */
	uint32		count;			/* distinct hashes in tab, at most SLAB_SLOTS / 2: past that the slab is not filtered (full) */
	bool		full;
	uint32		nslots;			/* a power of two, SLAB_SLOTS_MIN while the slab is small, at most SLAB_SLOTS */
	uint64	   *tab;			/* open addressing on the hash, 0 for a free place */
} SlabFilter;

static SlabFilter *filters[SLAB_FILTERS];
static int	nfilters;

static void
tab_insert(uint64 *tab, uint32 nslots, uint64 fp)
{
	uint32		i = (uint32) fp & (nslots - 1);

	while (tab[i] != 0 && tab[i] != fp)
		i = (i + 1) & (nslots - 1);
	tab[i] = fp;
}

static void
tab_grow(SlabFilter *f)
{
	uint32		nslots = f->nslots * 2;
	uint64	   *tab;

	if (!sift_cache_room(nslots * sizeof(uint64)))
	{
		f->full = true;
		return;
	}
	tab = MemoryContextAllocZero(f->cx, nslots * sizeof(uint64));
	for (uint32 i = 0; i < f->nslots; i++)
		if (f->tab[i] != 0)
			tab_insert(tab, nslots, f->tab[i]);
	pfree(f->tab);
	f->tab = tab;
	f->nslots = nslots;
}

static void
tab_put(SlabFilter *f, uint64 fp)
{
	uint32		i = (uint32) fp & (f->nslots - 1);

	while (f->tab[i] != 0 && f->tab[i] != fp)
		i = (i + 1) & (f->nslots - 1);
	if (f->tab[i] != 0)
		return;
	f->tab[i] = fp;
	f->count++;
	if (f->count >= SLAB_SLOTS / 2)
		f->full = true;
	else if (f->count * 2 >= f->nslots)
		tab_grow(f);
}

static bool
tab_has(const SlabFilter *f, uint64 fp)
{
	for (uint32 i = (uint32) fp & (f->nslots - 1); f->tab[i] != 0; i = (i + 1) & (f->nslots - 1))
		if (f->tab[i] == fp)
			return true;
	return false;
}

static void
filter_reset(SlabFilter *f)
{
	memset(f->tab, 0, f->nslots * sizeof(uint64));
	f->count = 0;
	f->nrefs = 0;
	f->full = false;
}

static void
filter_drop(int i)
{
	MemoryContextDelete(filters[i]->cx);
	filters[i] = filters[--nfilters];
}

static int
filter_oldest(void)
{
	int			victim = -1;

	for (int i = 0; i < nfilters; i++)
		if (!sift_cache_used_in_xact(filters[i]->used) && (victim < 0 || filters[i]->used < filters[victim]->used))
			victim = i;
	return victim;
}

static uint64
filters_oldest(void)
{
	int			i = filter_oldest();

	return i < 0 ? UINT64_MAX : filters[i]->used;
}

static bool
filters_evict_one(void)
{
	int			i = filter_oldest();

	if (i < 0)
		return false;
	filter_drop(i);
	return true;
}

static void
filters_evict_all(void)
{
	while (filters_evict_one())
		;
}

static const SiftCacheKind filters_kind = {"col_f2_uniq_slab", filters_oldest, filters_evict_one, filters_evict_all};

static SlabFilter *
filter_make(Relation index, uint64 part_id)
{
	MemoryContext cx = sift_cache_create_context("sift cache unique slab hashes");
	SlabFilter *f = MemoryContextAllocZero(cx, sizeof(SlabFilter));

	f->cx = cx;
	f->nslots = SLAB_SLOTS_MIN;
	f->tab = MemoryContextAllocZero(cx, SLAB_SLOTS_MIN * sizeof(uint64));
	f->locator = index->rd_locator;
	f->part_id = part_id;
	return f;
}

static int
filter_victim(void)
{
	int			lru = 0;

	for (int i = 1; i < nfilters; i++)
		lru = filters[i]->used < filters[lru]->used ? i : lru;
	return lru;
}

static SlabFilter *
filter_get(Relation index, uint64 part_id)
{
	static bool registered;
	SlabFilter *f;

	for (int i = 0; i < nfilters; i++)
		if (filters[i]->part_id == part_id && RelFileLocatorEquals(filters[i]->locator, index->rd_locator))
		{
			filters[i]->used = sift_cache_tick();
			return filters[i];
		}
	if (!registered)
	{
		sift_cache_register(&filters_kind);
		registered = true;
	}
	if (nfilters == SLAB_FILTERS)
		filter_drop(filter_victim());
	f = filter_make(index, part_id);
	f->used = sift_cache_tick();
	filters[nfilters++] = f;
	return f;
}

static bool
same_ref(const SiftL0HeapRef *a, const SiftL0HeapRef *b)
{
	return memcmp(&a->ctid, &b->ctid, sizeof(ItemPointerData)) == 0 && a->xmin == b->xmin;
}

bool
sift_uniq_fold_tids(Relation heap, const ItemPointerData *tids, uint32 n, SiftUniqSlabKeyFn key_fp, void *arg, SiftUniqFpPut put, void *put_arg)
{
	IndexFetchTableData *fetch = sift_table_index_fetch_begin(heap);
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	bool		room = true;

	for (uint32 i = 0; i < n && room; i++)
	{
		bool		again = false,
					dead = false;

		if (table_index_fetch_tuple(fetch, (ItemPointer) &tids[i], SnapshotAny, slot, &again, &dead))
		{
			uint64		fp = key_fp(arg, slot);

			room = fp == 0 || put(put_arg, fp);
		}
	}
	ExecDropSingleTupleTableSlot(slot);
	table_index_fetch_end(fetch);
	return room;
}

static bool
tab_put_fn(void *arg, uint64 fp)
{
	SlabFilter *f = arg;

	tab_put(f, fp);
	return !f->full;
}

/* Folds the tuples at tids[nrefs..n) */
static void
filter_fold(SlabFilter *f, Relation heap, const ItemPointerData *tids, uint32 n, SiftUniqSlabKeyFn key_fp, void *arg)
{
	if (!f->full)
		(void) sift_uniq_fold_tids(heap, tids + f->nrefs, n - f->nrefs, key_fp, arg, tab_put_fn, f);
	f->nrefs = n;
}

/* The slab of references: new references are folded, and the slab began again when the reference at the place of the last one folded is another */
static void
refresh_refs(SlabFilter *f, Relation index, Relation heap, BlockNumber tail, SiftUniqSlabKeyFn key_fp, void *arg)
{
	uint32		n;
	SiftL0HeapRef *refs = sift_l0_href_refs(index, f->part_id, tail, &n);
	ItemPointerData *tids = palloc(Max(n, 1) * sizeof(ItemPointerData));

	if (n < f->nrefs || (f->nrefs != 0 && !same_ref(&refs[f->nrefs - 1], &f->last)))
		filter_reset(f);
	for (uint32 i = 0; i < n; i++)
		tids[i] = refs[i].ctid;
	filter_fold(f, heap, tids, n, key_fp, arg);
	if (n > 0)
		f->last = refs[n - 1];
	pfree(tids);
	pfree(refs);
}

/* The slab of batches is read whole again when it changed: its rows carry their heap addresses */
static void
refresh_batches(SlabFilter *f, Relation index, Relation heap, BlockNumber head, SiftUniqSlabKeyFn key_fp, void *arg)
{
	const SiftPartSpec *spec = &sift_f2_rel(index)->spec;
/* check-memory: allow bounded to one micro-slab: SIFT_L0_SLAB_MAX_PAGES pages */
	SiftRowBuf *rb = sift_rowbuf_new(spec);
	ItemPointerData *tids;

	sift_l0_slab_read(index, f->part_id, head, rb, spec, GetActiveSnapshot(), NULL, NULL);
	tids = palloc(Max(rb->n, 1) * sizeof(ItemPointerData));
	for (int i = 0; i < rb->n; i++)
		sift_f2_tid_set(&tids[i], rb->tid[i]);
	filter_reset(f);
	filter_fold(f, heap, tids, rb->n, key_fp, arg);
	pfree(tids);
	sift_rowbuf_free(rb);
}

/*
 * What the chain at tail holds now: for a slab of references the page of its tail says (an open page has no checksum, which the stamp of col/f2/l0/slab.h would refuse); the tail
 * is the page that every append changes. A slab of batches has the stamp of its pages.
 */
static SiftChainStamp
chain_stamp(Relation index, uint64 part_id, BlockNumber tail, bool *href)
{
	Buffer		buf = sift_blk_read(index, tail, BUFFER_LOCK_SHARE);
	Page		page = BufferGetPage(buf);
	const SiftL0SlabPageData *sl = (const SiftL0SlabPageData *) SiftPageGetData(page);
	SiftChainStamp st = {0};

	sift_page_expect(index, page, tail, SIFT_PT_L0_SLAB, 0, (uint32) part_id);
	*href = sift_l0_href_is(page);
	if (*href)
	{
		st.lsn = PageGetLSN(page);
		st.count = sl->chain_rows;
		st.pages = sl->chain_pages;
	}
	UnlockReleaseBuffer(buf);
	return *href ? st : sift_l0_slab_stamp(index, part_id, tail);
}

/* Brings the filter of the slab at tail up to what the chain holds now */
static void
filter_refresh(SlabFilter *f, Relation index, Relation heap, BlockNumber tail, SiftUniqSlabKeyFn key_fp, void *arg)
{
	bool		href;
	SiftChainStamp st = chain_stamp(index, f->part_id, tail, &href);

	if (f->stamped && f->tail == tail && st.lsn == f->stamp.lsn && st.count == f->stamp.count && st.pages == f->stamp.pages)
		return;
	if (href)
		refresh_refs(f, index, heap, tail, key_fp, arg);
	else
		refresh_batches(f, index, heap, tail, key_fp, arg);
	f->stamp = st;
	f->tail = tail;
	f->stamped = true;
}

const SiftPlanSnapshot *
sift_uniq_slab_prune(Relation index, Relation heap, const SiftPlanSnapshot *plan, uint64 fp64, SiftUniqSlabKeyFn key_fp, void *arg, MemoryContext cx)
{
	SiftPlanSnapshot *pruned = NULL;

	for (int i = 0; i < plan->npartitions; i++)
	{
		const SiftPartitionEntry *e = &plan->partitions[i];
		SlabFilter *f;

		if (e->l0_slab_blk == InvalidBlockNumber)
			continue;
		f = filter_get(index, e->part_id);
		filter_refresh(f, index, heap, e->l0_slab_blk, key_fp, arg);
		if (f->full || tab_has(f, fp64))
			continue;
		if (pruned == NULL)
		{
			pruned = MemoryContextAlloc(cx, sizeof(SiftPlanSnapshot));
			*pruned = *plan;
			pruned->partitions = MemoryContextAlloc(cx, plan->npartitions * sizeof(SiftPartitionEntry));
			memcpy(pruned->partitions, plan->partitions, plan->npartitions * sizeof(SiftPartitionEntry));
		}
		pruned->partitions[i].l0_slab_blk = InvalidBlockNumber;
	}
	return pruned != NULL ? pruned : plan;
}
