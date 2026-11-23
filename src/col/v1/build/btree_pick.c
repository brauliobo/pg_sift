/*
 * col/v1/build/btree_pick.c - the path of a key_cut build of a table of more than a run: the heap read in the order of a btree on the key, or the key ranges
 * The btree reads a heap page for every entry whose page the cache no longer holds, and that is the heap read at random when the heap is not in the order of the
 * key; the key ranges read the heap once, in its order, and write and read back about a tenth of it. pg_stats.correlation does not say which is cheaper (a key
 * updated all day sits near 0.96 and reads most of the heap again), so the chooser walks the leaf level of every btree that can serve and plays the heap pages of its
 * entries through a cache of shared_buffers (LRU; the pages of a hash of the block number only when the cache is large, which keeps the hits and misses of the
 * whole stream in proportion). The costs are a stated time for a read and for a row of the key ranges, not the page costs of the planner, which a role may set to anything.
 * The walk also keeps keys of the leaf level at known counts of entries: the boundaries the participants of a parallel build split the btree at.
 */
#include "postgres.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "access/tableam.h"
#include "access/relscan.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "fmgr.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "storage/bufmgr.h"
#include "utils/hsearch.h"
#include "utils/rel.h"
#include "col/v1/build/build_int.h"
#include "core/memtrack.h"

#ifdef SIFT_TESTING
int			sift_test_col_pick_cache_blocks = -1;
int			sift_test_col_pick_entries = 0;
int			sift_test_col_pick_blocks = 0;
int			sift_test_col_pick_reads = 0;
int			sift_test_col_pick_random = 0;
int			sift_test_col_pick_spool_bytes = 0;
int			sift_test_col_pick_walk_bytes = 0;
#endif

#define SIM_MAX_BLOCKS	65536		/* the blocks the cache of the simulation holds at most: the cache is scaled down by a power of two to it */
#define SEQ_GAP			16			/* a block this far after the last one is read as a part of a sequential read */
/* microseconds: a MB of a temporary file in one direction (they are written and read once); the time of a read is measured (btree_probe.c), the CPU of a row by path is COLB_*_ROW_US */
#define TEMP_MB_US		1000.0
#define PAR_SCALING		1.0			/* the speed of a participant of a parallel scan against one alone, measured (docs/SIZING.md) */
#define SAMPLE_PAGES_MIN	4096
#define BOUND_SAMPLES	256			/* keys of the leaf level kept for the boundaries of the ranges of a parallel build */

typedef struct LruNode
{
	BlockNumber blk;
	struct LruNode *prev,
			   *next;
} LruNode;

typedef struct PickSim
{
	HTAB	   *lru;
	LruNode    *head,
			   *tail;
	uint32		cap,
				held;
	uint32		mask;			/* the blocks whose hash has these bits clear are played: 1 in mask + 1 */
	uint64		entries,
				blocks,			/* changes of the block along the entries: the items of the stream */
				seq_miss,
				rand_miss;
	BlockNumber prev;
	bool		have_prev;
	/* the keys of the leaf level every step entries */
	Relation	bt;
	FmgrInfo   *cmp;
	uint64		step,
				next_sample;
	int			nbound,
				maxbound;
	BtBound    *bound;
	Form_pg_attribute att;
} PickSim;

static void
lru_unlink(PickSim *s, LruNode *n)
{
	if (n->prev)
		n->prev->next = n->next;
	else
		s->head = n->next;
	if (n->next)
		n->next->prev = n->prev;
	else
		s->tail = n->prev;
}

static void
lru_push(PickSim *s, LruNode *n)
{
	n->prev = NULL;
	n->next = s->head;
	if (s->head)
		s->head->prev = n;
	s->head = n;
	if (s->tail == NULL)
		s->tail = n;
}

/* True when the block was not in the cache */
static bool
lru_touch(PickSim *s, BlockNumber blk)
{
	bool		found;
	LruNode    *n;

	if (s->cap == 0)
		return true;
	n = hash_search(s->lru, &blk, HASH_ENTER, &found);
	if (found)
	{
		lru_unlink(s, n);
		lru_push(s, n);
		return false;
	}
	n->blk = blk;
	lru_push(s, n);
	if (++s->held > s->cap)
	{
		LruNode    *old = s->tail;
		BlockNumber gone = old->blk;

		lru_unlink(s, old);
		hash_search(s->lru, &gone, HASH_REMOVE, NULL);
		s->held--;
	}
	return true;
}

static void
sim_begin(PickSim *s, Relation bt, uint32 cache_blocks, uint64 est_entries)
{
	HASHCTL		ctl = {.keysize = sizeof(BlockNumber), .entrysize = sizeof(LruNode), .hcxt = CurrentMemoryContext};

	memset(s, 0, sizeof(*s));
	while (cache_blocks / (s->mask + 1) > SIM_MAX_BLOCKS)
		s->mask = s->mask * 2 + 1;
	s->cap = cache_blocks / (s->mask + 1);
	s->lru = hash_create("sift_col build heap blocks", Max(s->cap, 16), &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	s->bt = bt;
	s->cmp = index_getprocinfo(bt, 1, BTORDER_PROC);
	s->att = TupleDescAttr(RelationGetDescr(bt), 0);
	s->step = Max(est_entries / BOUND_SAMPLES, 1);
	s->maxbound = BOUND_SAMPLES * 2;
	s->bound = palloc(s->maxbound * sizeof(BtBound));
}

/* The key of an entry, kept when it is another than the last kept: the boundaries are ascending and distinct */
static void
sim_sample(PickSim *s, IndexTuple itup)
{
	bool		isnull;
	Datum		key = index_getattr(itup, 1, RelationGetDescr(s->bt), &isnull);

	s->next_sample = s->entries + s->step;
	if (isnull || s->nbound == s->maxbound)
		return;
	if (s->nbound > 0 && DatumGetInt32(FunctionCall2Coll(s->cmp, s->bt->rd_indcollation[0], s->bound[s->nbound - 1].key, key)) >= 0)
		return;
	s->bound[s->nbound++] = (BtBound) {datumCopy(key, s->att->attbyval, s->att->attlen), s->entries};
}

static void
sim_entry(PickSim *s, BlockNumber blk)
{
	bool		seq;

	s->entries++;
	if (s->have_prev && blk == s->prev)
		return;
	seq = s->have_prev && blk > s->prev && blk - s->prev <= SEQ_GAP;
	s->prev = blk;
	s->have_prev = true;
	s->blocks++;
	if ((hash_bytes_uint32(blk) & s->mask) == 0 && lru_touch(s, blk))
		*(seq ? &s->seq_miss : &s->rand_miss) += 1;
}

static void
sim_page(PickSim *s, Page page)
{
	BTPageOpaque opaque = BTPageGetOpaque(page);
	OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

	if (P_IGNORE(opaque) || !P_ISLEAF(opaque))
		return;
	for (OffsetNumber off = P_FIRSTDATAKEY(opaque); off <= maxoff; off++)
	{
		ItemId		id = PageGetItemId(page, off);
		IndexTuple	itup;

		if (ItemIdIsDead(id))
			continue;
		itup = (IndexTuple) PageGetItem(page, id);
		if (s->entries >= s->next_sample)
			sim_sample(s, itup);
		if (BTreeTupleIsPosting(itup))
			for (int i = 0; i < BTreeTupleGetNPosting(itup); i++)
				sim_entry(s, ItemPointerGetBlockNumber(BTreeTupleGetPostingN(itup, i)));
		else
			sim_entry(s, ItemPointerGetBlockNumber(&itup->t_tid));
	}
}

/* The heap blocks of the entries of the leaf level, in the order of the key */
static void
sim_leaves(PickSim *s, Relation bt)
{
	BufferAccessStrategy strategy = GetAccessStrategy(BAS_BULKREAD);
	Buffer		buf = _bt_get_endpoint(bt, 0, false);

	while (BufferIsValid(buf))
	{
		BlockNumber next = BTPageGetOpaque(BufferGetPage(buf))->btpo_next;

		sim_page(s, BufferGetPage(buf));
		UnlockReleaseBuffer(buf);
		if (next == P_NONE)
			break;
		buf = ReadBufferExtended(bt, MAIN_FORKNUM, next, RBM_NORMAL, strategy);
		LockBuffer(buf, BUFFER_LOCK_SHARE);
		CHECK_FOR_INTERRUPTS();
	}
	FreeAccessStrategy(strategy);
}

static uint32
cache_blocks(void)
{
#ifdef SIFT_TESTING
	if (sift_test_col_pick_cache_blocks >= 0)
		return (uint32) sift_test_col_pick_cache_blocks;
#endif
	return (uint32) NBuffers;
}

/* A btree that is large against the heap costs more to walk than it can save */
static bool
walkable(Relation heap, Relation bt)
{
	return RelationGetNumberOfBlocks(bt) <= Max(RelationGetNumberOfBlocks(heap) / 8, SAMPLE_PAGES_MIN);
}

/*
 * The time of both paths from the entries of the btree played through the cache, at the costs above, for a build of degree participants; NULL for a btree too
 * large to read before the build. The plan is made in the current context, the walk's own memory is freed.
 */
static BtPlan *
pick_cost(Relation heap, Relation bt, uint64 est_entries, double degree, const BtCosts *cost, MemoryContext plan_cx)
{
	BlockNumber heap_blocks = RelationGetNumberOfBlocks(heap),
				bt_blocks = RelationGetNumberOfBlocks(bt);
	MemoryContext walk_cx = CurrentMemoryContext;
	MemoryContext cx = AllocSetContextCreate(walk_cx, "sift_col build pick", ALLOCSET_DEFAULT_SIZES);
	BtPlan	   *c;
	PickSim		s;
	double		scale;

	MemoryContextSwitchTo(cx);
	sim_begin(&s, bt, cache_blocks(), est_entries);
	sim_leaves(&s, bt);
#ifdef SIFT_TESTING
	sift_test_col_pick_walk_bytes = (int) MemoryContextMemAllocated(cx, true);
#endif
	MemoryContextSwitchTo(plan_cx);
	c = palloc0(sizeof(BtPlan));
	scale = (double) s.mask + 1;
	c->btree = RelationGetRelid(bt);
	c->name = pstrdup(RelationGetRelationName(bt));
	c->entries = s.entries;
	c->random_reads = s.rand_miss * scale;
	c->reads = c->random_reads + s.seq_miss * scale;
	c->cost = *cost;
	c->seq_blocks = c->reads - c->random_reads + bt_blocks;
	c->degree = degree;
	c->heap_blocks = heap_blocks;
	c->btree_us = (c->random_reads * cost->random_us + c->seq_blocks * cost->seq_us + s.entries * COLB_BTREE_ROW_US) / degree;
	c->nbound = s.nbound;
	c->bound = palloc(s.nbound * sizeof(BtBound));
	for (int i = 0; i < s.nbound; i++)
		c->bound[i] = (BtBound) {datumCopy(s.bound[i].key, s.att->attbyval, s.att->attlen), s.bound[i].before};
#ifdef SIFT_TESTING
	sift_test_col_pick_entries = (int) s.entries;
	sift_test_col_pick_blocks = (int) s.blocks;
	sift_test_col_pick_reads = (int) c->reads;
	sift_test_col_pick_random = (int) c->random_reads;
#endif
	MemoryContextSwitchTo(walk_cx);
	MemoryContextDelete(cx);
	return c;
}

/* The files of the key ranges, priced from rows of the table (that is a read of the heap), against the btree's time: when they would be written, their time, and the cost of a random read at which both paths take as long */
static void
price_files(Relation heap, Relation index, IndexInfo *info, BtPlan *c, BtPricing *price)
{
	price->row_bytes = colb_spill_price(heap, index, info, c->entries);
	price->files_priced = true;
	c->spool_mb = (double) c->entries * price->row_bytes / 1048576.0;
	c->spool_us = c->heap_blocks * price->cost.seq_us + 2 * c->spool_mb * TEMP_MB_US + c->entries * COLB_HEAP_ROW_US;
	c->break_even_us = c->random_reads > 0 ?
		Min((c->spool_us * c->degree - c->seq_blocks * price->cost.seq_us - c->entries * COLB_BTREE_ROW_US) / c->random_reads, 1e6) : 1e6;
#ifdef SIFT_TESTING
	sift_test_col_pick_spool_bytes = (int) (c->spool_mb * 1048576.0);
#endif
}

/*
 * The cheapest btree to read the heap through among the candidates, costed at what a read of the heap costs on its disk: the memory of the walks and of the pricing is held in
 * one context, whose scope the tests assert. price->cost is what the walks were costed at (price->costed: a btree was walkable, so the disk was measured). The files of the key
 * ranges are priced, from rows of the heap, unless the index a REINDEX replaces (price->old_us) is cheaper than this btree and than the heap read at its least
 * (price->heap_floor_us): the build then reads no block of the heap, and its files are not priced.
 */
BtPlan *
colb_btree_best(Relation heap, Relation index, IndexInfo *info, List *cands, uint64 tuples, int participants, MemoryContext plan_cx, StringInfo names, BtPricing *price)
{
	MemoryContext walk_cx = CurrentMemoryContext;
	MemoryContext pcx = AllocSetContextCreate(plan_cx, "sift_col build choice", ALLOCSET_DEFAULT_SIZES);
	BtPlan	   *best = NULL;
	ListCell   *lc;

	SiftMemTracker mt_col_btree_pick;

	sift_mem_init_sub(&mt_col_btree_pick, "col_btree_pick", pcx, false, sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 1));
	MemoryContextSwitchTo(pcx);
	foreach(lc, cands)
	{
		Relation	bt = index_open(lfirst_oid(lc), AccessShareLock);

		if (walkable(heap, bt))
		{
			BtPlan	   *c;

			if (!price->costed)
			{
				price->cost = colb_btree_costs(heap);
				price->costed = true;
			}
			c = pick_cost(heap, bt, tuples, participants * PAR_SCALING, &price->cost, plan_cx);
			appendStringInfo(names, "%s%s %.0f reads", names->len ? ", " : "", RelationGetRelationName(bt), c->reads);
			if (best == NULL || c->btree_us < best->btree_us)
				best = c;
		}
		index_close(bt, NoLock);
	}
	if (best != NULL && !(price->old_us <= best->btree_us && price->old_us <= price->heap_floor_us))
		price_files(heap, index, info, best, price);
	sift_mem_assert_within(&mt_col_btree_pick);
	MemoryContextSwitchTo(walk_cx);
	MemoryContextDelete(pcx);
	return best;
}

