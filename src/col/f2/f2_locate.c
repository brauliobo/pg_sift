/*
 * col/f2/f2_locate.c - the lookups of rows by address over the part pins (col/f2/f2_gbuf.h): the parts of a manifest by the envelope of their addresses, the granules of a part by theirs, and the
 * granule of an address.
 *
 * check-memory: allow-allocs every array follows the parts of one manifest or the granules of one part, and the cache around them is bounded by its budget
 */
#include "postgres.h"

#include "access/xlog.h"
#include "catalog/pg_class_d.h"
#include "port/pg_bitutils.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/f2/delvec_delta.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_gbuf_int.h"
#include "col/f2/f2_slab_view.h"
#include "col/f2/f2_tid_dir.h"
#include "col/f2/f2_tid_hash.h"
#include "col/f2/l0_slab.h"
#include "col/f2/l0_stage.h"
#include "core/cachebudget.h"
#include "core/memtrack.h"

/*
 * Intervals [min, max] kept sorted by min with the greatest max of each subtree in a complete binary tree over them (leaf i is node leaves + i, a node holds the greater of its two): the
 * intervals that meet [lo, hi] are those of the first `first` ones (min <= hi) whose subtree holds a max of lo or more, found by a walk that leaves every other subtree unvisited, whatever
 * the intervals before are (one that spans every address does not make the walk linear).
 */
typedef struct IntervalTree
{
	uint32		leaves;			/* a power of two, at least n */
	uint64	   *max;			/* [2 * leaves] */
} IntervalTree;

static IntervalTree
tree_build(MemoryContext cx, const uint64 *maxes, int n)
{
	IntervalTree t = {.leaves = pg_nextpower2_32(Max(n, 1))};

	t.max = MemoryContextAllocZero(cx, 2 * (size_t) t.leaves * sizeof(uint64));
	for (int i = 0; i < n; i++)
		t.max[t.leaves + i] = maxes[i];
	for (uint32 v = t.leaves - 1; v >= 1; v--)
		t.max[v] = Max(t.max[2 * v], t.max[2 * v + 1]);
	return t;
}

typedef struct TreeWalk
{
	uint32		node;
	uint32		from;
	uint32		to;
} TreeWalk;

/* Calls hit(i, arg) for the intervals i < first whose max is lo or more, the last first; hit returns true to stop; true when it did */
static bool
tree_walk(const IntervalTree *t, int first, uint64 lo, bool (*hit) (int i, void *arg), void *arg)
{
	TreeWalk	stack[64];
	int			sp = 0;

	if (first <= 0 || t->max[1] < lo)
		return false;
	stack[sp++] = (TreeWalk) {1, 0, t->leaves};
	while (sp > 0)
	{
		TreeWalk	w = stack[--sp];
		uint32		mid = (w.from + w.to) / 2;

		if ((int) w.from >= first || t->max[w.node] < lo)
			continue;
		if (w.node >= t->leaves)
		{
			if (hit((int) (w.node - t->leaves), arg))
				return true;
			continue;
		}
		stack[sp++] = (TreeWalk) {2 * w.node, w.from, mid};
		stack[sp++] = (TreeWalk) {2 * w.node + 1, mid, w.to};
	}
	return false;
}

/* ---- the granules of a part by address ---- */

typedef struct GranSlot
{
	uint64		tid_min;
	uint64		tid_max;
	uint32		gid;
} GranSlot;

typedef struct SiftGranIntervalIdx
{
	int			n;
	GranSlot   *slots;			/* ascending by tid_min */
	IntervalTree tree;
} SiftGranIntervalIdx;

#define GRAN_INDEX_MIN_GRANULES	16
#define GRAN_DIR_CANDIDATES		4

static int
gran_slot_cmp(const void *a, const void *b)
{
	const GranSlot *x = a,
			   *y = b;

	return x->tid_min != y->tid_min ? (x->tid_min > y->tid_min) - (x->tid_min < y->tid_min) : (x->gid > y->gid) - (x->gid < y->gid);
}

static SiftGranIntervalIdx *
gran_index(SiftF2PartPin *pin)
{
	if (pin->gidx == NULL)
	{
		uint32		ng = pin->part->hdr.ngranules;
		SiftGranIntervalIdx *ix = MemoryContextAlloc(pin->cx, sizeof(SiftGranIntervalIdx));
		uint64	   *maxes = palloc(ng * sizeof(uint64));

		ix->n = (int) ng;
		ix->slots = MemoryContextAlloc(pin->cx, ng * sizeof(GranSlot));
		for (uint32 g = 0; g < ng; g++)
			ix->slots[g] = (GranSlot) {pin->tidx[g].tid_min, pin->tidx[g].tid_max, g};
		qsort(ix->slots, ng, sizeof(GranSlot), gran_slot_cmp);
		for (uint32 g = 0; g < ng; g++)
			maxes[g] = ix->slots[g].tid_max;
		ix->tree = tree_build(pin->cx, maxes, (int) ng);
		pfree(maxes);
		pin->gidx = ix;
	}
	return pin->gidx;
}

typedef struct GranHits
{
	const SiftGranIntervalIdx *ix;
	uint32	   *gids;
	int			n;
} GranHits;

static bool
gran_hit(int i, void *arg)
{
	GranHits   *h = arg;

	h->gids[h->n++] = h->ix->slots[i].gid;
	return false;
}

static int
gid_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return (x > y) - (x < y);
}

/* The granules of the part that may hold an address of [lo, hi], ascending: *gids is a palloc'd array of them (NULL for none). The directory of the part (f2_tid_dir.h) tells those of one
 * address when it has one, and is made for a lookup that the envelopes leave to more than GRAN_DIR_CANDIDATES granules */
static int
part_granules(Relation rel, SiftF2PartPin *pin, uint64 lo, uint64 hi, uint32 **gids)
{
	const SiftGranIntervalIdx *ix = gran_index(pin);
	int			first = 0,
				last = ix->n;
	GranHits	h = {ix, NULL, 0};
	int			n;

	if (lo == hi && sift_f2_tid_dir_granules(rel, pin, lo, false, gids, &n))
		return n;
	while (first < last)
	{
		int			mid = first + (last - first) / 2;

		if (ix->slots[mid].tid_min <= hi)
			first = mid + 1;
		else
			last = mid;
	}
	h.gids = palloc(Max(first, 1) * sizeof(uint32));
	(void) tree_walk(&ix->tree, first, lo, gran_hit, &h);
	if (lo == hi && h.n > GRAN_DIR_CANDIDATES && sift_f2_tid_dir_granules(rel, pin, lo, true, gids, &n))
	{
		pfree(h.gids);
		return n;
	}
	qsort(h.gids, h.n, sizeof(uint32), gid_cmp);
	*gids = h.gids;
	return h.n;
}

/* ---- the parts of a manifest by address ---- */

/* The envelope of the addresses of a part (SiftPartDesc.tid_min, tid_max) */
typedef struct TidSlot
{
	uint64		tid_min;
	uint64		tid_max;
	int32		partition;
	int32		part;
} TidSlot;

/* The parts of the manifest of one version, by address; made when a lookup meets the version first and kept (one for each relation file of the backend) until another version comes */
typedef struct TidIndex
{
	RelFileLocator locator;		/* first: the hash key */
	Oid			relid;
	uint32		relhash;		/* sift_f2_rel_hash of the relation, taken when the index is made */
	uint64		manifest_version;
	uint64		tick;			/* sift_cache_tick() of its last use */
	MemoryContext cx;			/* what the index holds */
	int			n;
	TidSlot    *slots;			/* the parts with an envelope, ascending by tid_min */
	IntervalTree tree;
	int			nloose;
	TidSlot    *loose;			/* the parts without one: every address may be in them */
	dlist_node	lru_node;		/* in tid_lru, the one used longest ago first */
	dlist_node	rel_node;		/* in the list of its relation */
} TidIndex;

#define TID_INDEX_MAX	64

static struct
{
	HTAB	   *by_locator;
	HTAB	   *by_rel;
	MemoryContext cx;
	dlist_head	lru;
	int			visits;			/* lookups that walk an index now: it is not evicted under them */
} tids;

static int
slot_cmp(const void *a, const void *b)
{
	const TidSlot *x = a,
			   *y = b;

	return x->tid_min != y->tid_min ? (x->tid_min > y->tid_min) - (x->tid_min < y->tid_min) : (x->tid_max > y->tid_max) - (x->tid_max < y->tid_max);
}

/* The index goes: it is on no list of a walk that is under way */
static void
tid_index_free(TidIndex *ix)
{
	RelFileLocator locator = ix->locator;

	dlist_delete(&ix->lru_node);
	MemoryContextDelete(ix->cx);
	hash_search(tids.by_locator, &locator, HASH_REMOVE, NULL);
}

static void
tid_index_drop(TidIndex *ix)
{
	rel_list_unlink(tids.by_rel, ix->relid, &ix->rel_node);
	tid_index_free(ix);
}

static TidIndex *
tid_index_victim(void)
{
	if (tids.by_locator == NULL || tids.visits > 0 || dlist_is_empty(&tids.lru))
		return NULL;
	return dlist_container(TidIndex, lru_node, dlist_head_node(&tids.lru));
}

static uint64
tid_index_oldest(void)
{
	TidIndex   *victim = tid_index_victim();

	return victim != NULL ? victim->tick : UINT64_MAX;
}

static bool
tid_index_evict_one(void)
{
	TidIndex   *victim = tid_index_victim();

	if (victim == NULL)
		return false;
	tid_index_drop(victim);
	return true;
}

/* Every index goes in one pass over the list (none while a visit reads one) */
static void
tid_index_evict_all(void)
{
	while (tid_index_evict_one())
		;
}

static const SiftCacheKind tid_index_kind = {"f2_tid_indexes", tid_index_oldest, tid_index_evict_one, tid_index_evict_all};

void
f2_locate_invalidate(Oid relid)
{
	dlist_head	mine;
	dlist_mutable_iter it;

	if (tids.by_locator == NULL || tids.visits > 0)
		return;
	if (!OidIsValid(relid))
	{
		tid_index_evict_all();
		return;
	}
	rel_list_detach(tids.by_rel, relid, &mine);
	dlist_foreach_modify(it, &mine)
		tid_index_free(dlist_container(TidIndex, rel_node, it.cur));
}

/* The index of a relation file whose generations start again must not be taken for one of the new rows: a visit that reads it keeps it, but no later lookup finds it */
void
f2_locate_forget(const RelFileLocator *locator)
{
	TidIndex   *ix = tids.by_locator != NULL ? hash_search(tids.by_locator, locator, HASH_FIND, NULL) : NULL;

	if (ix == NULL)
		return;
	if (tids.visits > 0)
		ix->manifest_version = UINT64_MAX;
	else
		tid_index_drop(ix);
}

void
f2_locate_invalidate_hits(uint32 hashvalue)
{
	if (tids.by_locator == NULL)
		return;
	foreach_oid(relid, rel_lists_hits(tids.by_rel, hashvalue))
		f2_locate_invalidate(relid);
}

static TidIndex
tid_index_build(const SiftPlanSnapshot *v, MemoryContext parent)
{
	TidIndex	ix = {.locator = v->locator,.manifest_version = v->manifest_version,.cx = sift_mem_allocset_create(parent, "sift cache f2 tid index")};
	int			total = 0;
	uint64	   *maxes;

	for (int i = 0; i < v->npartitions; i++)
		total += v->partlists[i].nparts;
	ix.slots = MemoryContextAlloc(ix.cx, sizeof(TidSlot) * Max(total, 1));
	ix.loose = MemoryContextAlloc(ix.cx, sizeof(TidSlot) * Max(total, 1));
	for (int i = 0; i < v->npartitions; i++)
		for (int j = 0; j < v->partlists[i].nparts; j++)
		{
			const SiftPartDesc *d = &v->partlists[i].parts[j];
			TidSlot		slot = {.tid_min = d->tid_min,.tid_max = d->tid_max,.partition = i,.part = j};

			if (d->tid_max != 0)
				ix.slots[ix.n++] = slot;
			else
				ix.loose[ix.nloose++] = slot;
		}
	qsort(ix.slots, ix.n, sizeof(TidSlot), slot_cmp);
	maxes = palloc(Max(ix.n, 1) * sizeof(uint64));
	for (int k = 0; k < ix.n; k++)
		maxes[k] = ix.slots[k].tid_max;
	ix.tree = tree_build(ix.cx, maxes, ix.n);
	pfree(maxes);
	return ix;
}

/* The index of the version of the manifest the view has, kept for the next lookup; one of a view that lists micro-slabs is made for the lookup, in mcxt */
static const TidIndex *
tid_index_of(Relation rel, const SiftPlanSnapshot *v, MemoryContext mcxt)
{
	TidIndex   *e;
	TidIndex	built;

	if (tids.by_locator == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(RelFileLocator),.entrysize = sizeof(TidIndex),.hcxt = sift_cache_create_context("sift cache f2 tid indexes")};

		tids.cx = ctl.hcxt;
		tids.by_locator = hash_create("sift cache f2 tid indexes", TID_INDEX_MAX, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		tids.by_rel = rel_lists_create("sift cache f2 tid indexes by relation", tids.cx);
		dlist_init(&tids.lru);
		sift_cache_register(&tid_index_kind);
		f2_gbuf_register_invalidation();
	}
	if (v->slabs_in_memory)
	{
		TidIndex   *own = MemoryContextAlloc(mcxt, sizeof(TidIndex));

		*own = tid_index_build(v, mcxt);
		return own;
	}
	e = hash_search(tids.by_locator, &v->locator, HASH_FIND, NULL);
	if (e != NULL && e->manifest_version == v->manifest_version)
	{
		e->tick = sift_cache_tick();
		dlist_move_tail(&tids.lru, &e->lru_node);
		return e;
	}
	built = tid_index_build(v, tids.cx);
	built.relid = RelationGetRelid(rel);
	built.relhash = sift_f2_rel_hash(rel);
	built.tick = sift_cache_tick();
	if (e != NULL)
	{
		MemoryContextDelete(e->cx);
		e->cx = built.cx;
		e->slots = built.slots;
		e->loose = built.loose;
		e->tree = built.tree;
		e->n = built.n;
		e->nloose = built.nloose;
		e->manifest_version = built.manifest_version;
		e->tick = built.tick;
		dlist_move_tail(&tids.lru, &e->lru_node);
		return e;
	}
	if (hash_get_num_entries(tids.by_locator) >= TID_INDEX_MAX)
		tid_index_evict_one();
	e = hash_search(tids.by_locator, &v->locator, HASH_ENTER, NULL);
	*e = built;
	dlist_push_tail(&tids.lru, &e->lru_node);
	rel_list_link(tids.by_rel, e->relid, e->relhash, &e->rel_node);
	return e;
}

typedef bool (*TidSlotVisit) (const TidSlot *slot, void *arg);

typedef struct SlotWalk
{
	const TidIndex *ix;
	TidSlotVisit visit;
	void	   *arg;
} SlotWalk;

static bool
slot_hit(int i, void *arg)
{
	SlotWalk   *w = arg;

	return w->visit(&w->ix->slots[i], w->arg);
}

/* Calls visit for the parts that may hold an address of [lo, hi], the latest by tid_min first: by the envelope of each, in O(log parts + the parts that overlap), and the parts without an envelope; visit returns true to stop */
static void
tid_index_visit(const TidIndex *ix, uint64 lo, uint64 hi, TidSlotVisit visit, void *arg)
{
	int			first = 0,
				last = ix->n;
	SlotWalk	w = {ix, visit, arg};

	while (first < last)
	{
		int			mid = first + (last - first) / 2;

		if (ix->slots[mid].tid_min <= hi)
			first = mid + 1;
		else
			last = mid;
	}
	if (tree_walk(&ix->tree, first, lo, slot_hit, &w))
		return;
	for (int k = 0; k < ix->nloose; k++)
		if (visit(&ix->loose[k], arg))
			return;
}

/* ---- the manifest ---- */

/* The slabs of the partitions written out as parts; a standby writes nothing, so its view lists them as parts made in memory */
static const SiftPlanSnapshot *
slabs_flushed(Relation rel, SiftF2Store *store, const SiftPlanSnapshot *snap, MemoryContext mcxt)
{
	bool		flushed = false;

	for (int i = 0; i < snap->npartitions; i++)
		if (snap->partitions[i].l0_slab_blk != InvalidBlockNumber)
		{
			sift_l0_slab_flush(rel, snap->partitions[i].part_id, snap->partitions[i].key_lo, &store->spec);
			flushed = true;
		}
	return flushed ? sift_plan_cache_get(rel, mcxt) : snap;
}

/* The view and what writing the slabs takes are mcxt's: a statement that locates a row for each of its rows does not keep them in its executor context */
static const SiftPlanSnapshot *
view_slabs_flushed(Relation rel, SiftF2Store *store, MemoryContext mcxt)
{
	MemoryContext caller = MemoryContextSwitchTo(mcxt);
	const SiftPlanSnapshot *snap = sift_plan_cache_get(rel, mcxt);

	snap = RecoveryInProgress() ? sift_f2_slab_view(rel, store, snap, mcxt) : slabs_flushed(rel, store, snap, mcxt);
	MemoryContextSwitchTo(caller);
	return snap;
}

const SiftPlanSnapshot *
sift_f2_view(Relation rel, SiftF2Store *store, MemoryContext mcxt)
{
	if (sift_stage_has_pending(rel))
		sift_stage_flush(rel);
	return view_slabs_flushed(rel, store, mcxt);
}

typedef struct VisitPart
{
	Relation	rel;
	SiftF2Store *store;
	const SiftPlanSnapshot *view;
	uint64		lo;
	uint64		hi;
	bool		one_block;
	SiftF2GranuleLoader load;
	SiftF2GranuleVisit cb;
	void	   *arg;
} VisitPart;

/* Whether the visit of the granule stops */
static bool
visit_granule(const VisitPart *vp, SiftF2PartPin *pin, const SiftPartitionEntry *entry, uint32 g)
{
	const SiftTidIndexEntry *t = &pin->tidx[g];
	SiftF2GranuleBuf *gb;
	bool		stop;

	if (t->tid_max < vp->lo || t->tid_min > vp->hi || (vp->one_block && (t->blk_bloom & sift_tid_block_bit(vp->lo)) == 0))
		return false;
	gb = vp->load(vp->rel, vp->store, pin, g);
	stop = vp->cb(gb, entry, vp->arg);
	sift_f2_gbuf_unref(gb);
	return stop;
}

/* The granules the address hash of the part names for the address, ascending (n of them) */
static int
hashed_granules(VisitPart *vp, SiftF2PartPin *pin, uint32 **gids)
{
	SiftTidHashHit *hits;
	int			nhits = sift_tid_hash_find(vp->rel, pin->part, pin->desc.nrows, vp->lo, &hits),
				n = 0;

	for (int i = 0; i < nhits; i++)
	{
		const SiftTidHashHit *h = &hits[i];

		if (h->granule_id >= pin->part->hdr.ngranules ||
			(h->row_idx != SIFT_TID_HASH_NO_ROW && (h->row_idx < pin->gdir[h->granule_id].row_base || h->row_idx - pin->gdir[h->granule_id].row_base >= pin->gdir[h->granule_id].nrows)))
			SIFT_DAMAGE_DATA(vp->rel, errmsg("sift: an address hash entry names a row outside its granule"));
	}
	*gids = palloc(Max(nhits, 1) * sizeof(uint32));
	for (int i = 0; i < nhits; i++)
		if (n == 0 || (*gids)[n - 1] != hits[i].granule_id)
			(*gids)[n++] = hits[i].granule_id;
	pfree(hits);
	return n;
}

static bool
visit_part(const TidSlot *slot, void *arg)
{
	VisitPart  *vp = arg;
	const SiftPartitionEntry *entry = &vp->view->partitions[slot->partition];
	SiftF2PartPin *pin = sift_f2_pin_open(vp->rel, vp->store, &vp->view->partlists[slot->partition].parts[slot->part], (uint32) entry->part_id);
	uint32		ng = pin->part->hdr.ngranules;
	bool		stop = false;

	PG_TRY();
	{
		bool		hashed = vp->lo == vp->hi && sift_part_section(pin->part, SEC_TID_HASH) != NULL;

		if (pin->tid_max >= vp->lo && pin->tid_min <= vp->hi && (hashed || ng > GRAN_INDEX_MIN_GRANULES))
		{
			uint32	   *gids;
			int			n = hashed ? hashed_granules(vp, pin, &gids) : part_granules(vp->rel, pin, vp->lo, vp->hi, &gids);

			for (int k = 0; k < n && !stop; k++)
				stop = visit_granule(vp, pin, entry, gids[k]);
			pfree(gids);
		}
		else
			for (uint32 g = 0; g < ng && !stop && pin->tid_max >= vp->lo && pin->tid_min <= vp->hi; g++)
				stop = visit_granule(vp, pin, entry, g);
	}
	PG_FINALLY();
	{
		sift_f2_pin_release(pin);
	}
	PG_END_TRY();
	return stop;
}

static bool
has_slab(const SiftPlanSnapshot *snap)
{
	for (int i = 0; i < snap->npartitions; i++)
		if (snap->partitions[i].l0_slab_blk != InvalidBlockNumber)
			return true;
	return false;
}

static void
tid_visit_done(void *arg pg_attribute_unused())
{
	tids.visits--;
}

/* A visit reads an index while it opens pins, which may ask the budget for room: no index is dropped until the context of the visit is deleted */
static void
tid_visit_begin(MemoryContext visit)
{
	MemoryContextCallback *done = MemoryContextAlloc(visit, sizeof(MemoryContextCallback));

	*done = (MemoryContextCallback) {tid_visit_done, NULL, NULL};
	MemoryContextRegisterResetCallback(visit, done);
	tids.visits++;
}

/* Visits the granules that may hold the addresses lo..hi, each made by load; false when it left the micro-slabs of the partitions unflushed (flush false) and some partition has one */
static bool
visit_view(Relation rel, SiftF2Store *store, uint64 lo, uint64 hi, SiftF2GranuleLoader load, SiftF2GranuleVisit cb, void *arg, bool flush)
{
	MemoryContext pins = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 visit");
	const SiftPlanSnapshot *v;
	bool		slabs_left;
	VisitPart	vp;

	tid_visit_begin(pins);
	v = flush ? view_slabs_flushed(rel, store, pins) : sift_plan_cache_get(rel, pins);
	slabs_left = !flush && has_slab(v);
	vp = (VisitPart) {rel, store, v, lo, hi, (lo >> 16) == (hi >> 16), load, cb, arg};
	tid_index_visit(tid_index_of(rel, v, pins), lo, hi, visit_part, &vp);
	MemoryContextDelete(pins);
	return slabs_left;
}

static bool
visit_granules(Relation rel, SiftF2Store *store, uint64 lo, uint64 hi, SiftF2GranuleLoader load, SiftF2GranuleVisit cb, void *arg, bool flush)
{
	if (sift_stage_holds_in(rel, lo, hi))
		sift_stage_flush(rel);
	return visit_view(rel, store, lo, hi, load, cb, arg, flush);
}

/* The parts of the partitions as they are published, the stage of the transaction left alone: its rows are the caller's to read */
static void
visit_settled(Relation rel, SiftF2Store *store, uint64 lo, uint64 hi, SiftF2GranuleVisit cb, void *arg)
{
	(void) visit_view(rel, store, lo, hi, sift_f2_gbuf_load, cb, arg, true);
}

void
sift_f2_visit_granules_via(Relation rel, SiftF2Store *store, uint64 lo, uint64 hi, SiftF2GranuleLoader load, SiftF2GranuleVisit cb, void *arg)
{
	(void) visit_granules(rel, store, lo, hi, load, cb, arg, true);
}

void
sift_f2_visit_granules(Relation rel, SiftF2Store *store, uint64 lo, uint64 hi, SiftF2GranuleVisit cb, void *arg)
{
	sift_f2_visit_granules_via(rel, store, lo, hi, sift_f2_gbuf_load, cb, arg);
}

typedef struct LocateArg
{
	uint64		tid;
	SiftF2Loc  *loc;
	bool		found;
	bool		live_only;
} LocateArg;

/* An index has no versions of its own: after a VACUUM the heap gives an address to a later row, so a part may hold a row of the address that is dead already and a live one after it */
static bool
row_removed(const SiftF2GranuleBuf *gb, int row)
{
	const SiftDvGran *dg = gb->pin->dv != NULL ? sift_dv_granule(gb->pin->dv, gb->granule_id) : NULL;
	bool		dead = false;

	if (dg != NULL)
		(void) sift_dv_find(dg, (uint16) row, &dead);
	return dead;
}

/* The first row of the address that is not dead: the rows of the address are the adjacent ones of a granule whose addresses ascend, else those the table of the granule names in order */
static int
live_row_of(SiftF2GranuleBuf *gb, uint64 tid)
{
	for (int row = sift_f2_gbuf_row_of(gb, tid); row >= 0; row = sift_f2_gbuf_row_next(gb, tid, row))
		if (!row_removed(gb, row))
			return row;
	return -1;
}

static bool
locate_cb(SiftF2GranuleBuf *gb, const SiftPartitionEntry *entry, void *arg)
{
	LocateArg  *a = arg;
	int			row = a->live_only ? live_row_of(gb, a->tid) : sift_f2_gbuf_row_of(gb, a->tid);

	if (row < 0)
		return false;
	a->loc->entry = *entry;
	a->loc->gb = sift_f2_gbuf_ref(gb);
	a->loc->row = (uint16) row;
	a->found = true;
	return true;
}

bool
sift_f2_locate_settled(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc)
{
	LocateArg	a = {.tid = tid,.loc = loc,.live_only = rel->rd_rel->relkind == RELKIND_INDEX};

	visit_settled(rel, store, tid, tid, locate_cb, &a);
	return a.found;
}

bool
sift_f2_locate_published(Relation rel, SiftF2Store *store, uint64 tid, SiftF2Loc *loc)
{
	MemoryContext pinned = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 visit");
	LocateArg	a = {.tid = tid,.loc = loc,.live_only = rel->rd_rel->relkind == RELKIND_INDEX};
	const SiftPlanSnapshot *v = sift_plan_cache_get(rel, pinned);
	VisitPart	vp = {rel, store, v, tid, tid, true, sift_f2_gbuf_load, locate_cb, &a};

	tid_visit_begin(pinned);
	tid_index_visit(tid_index_of(rel, v, pinned), tid, tid, visit_part, &vp);
	MemoryContextDelete(pinned);
	return a.found;
}

/* The first part that holds the address: its place in the manifest and its granule, as the key of the lookup of rows in the order they lie */
static bool
order_part(const TidSlot *slot, void *arg)
{
	VisitPart  *vp = arg;
	SiftF2PartPin *pin = sift_f2_pin_open(vp->rel, vp->store, &vp->view->partlists[slot->partition].parts[slot->part], (uint32) vp->view->partitions[slot->partition].part_id);
	bool		found = false;

	PG_TRY();
	{
		uint32	   *gids = NULL;
		const bool	in_part = pin->tid_max >= vp->lo && pin->tid_min <= vp->hi;
		const bool	indexed = pin->part->hdr.ngranules > GRAN_INDEX_MIN_GRANULES;
		int			n = in_part && indexed ? part_granules(vp->rel, pin, vp->lo, vp->hi, &gids) : in_part;

		if (n > 0)
		{
			*(uint64 *) vp->arg = (uint64) slot->partition << 48 | (uint64) slot->part << 32 | (indexed ? gids[0] : 0);
			found = true;
		}
	}
	PG_FINALLY();
	{
		sift_f2_pin_release(pin);
	}
	PG_END_TRY();
	return found;
}

uint64
sift_f2_locate_order(Relation rel, SiftF2Store *store, uint64 tid)
{
	MemoryContext pinned = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 visit");
	const SiftPlanSnapshot *v = sift_plan_cache_get(rel, pinned);
	uint64		key = UINT64_MAX;
	VisitPart	vp = {rel, store, v, tid, tid, true, sift_f2_gbuf_load, NULL, &key};

	tid_visit_begin(pinned);
	tid_index_visit(tid_index_of(rel, v, pinned), tid, tid, order_part, &vp);
	MemoryContextDelete(pinned);
	return key;
}

/* The granule of an address, each granule made by load: the parts are searched as they are, and the micro-slabs are flushed into parts only when the address is in none (it is a row of a slab), so that a point lookup of a row in a part writes nothing */
bool
sift_f2_locate_via(Relation rel, SiftF2Store *store, uint64 tid, SiftF2GranuleLoader load, SiftF2Loc *loc)
{
	LocateArg	a = {.tid = tid,.loc = loc,.live_only = rel->rd_rel->relkind == RELKIND_INDEX};

	if (visit_granules(rel, store, tid, tid, load, locate_cb, &a, false) && !a.found)
		(void) visit_granules(rel, store, tid, tid, load, locate_cb, &a, true);
	return a.found;
}
