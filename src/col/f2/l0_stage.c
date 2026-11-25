/*
 * col/l0_stage.c - primitive 1: the transaction's partition-routed columnar stage (col/l0_stage.h).
 */
#include "core/host.h"

#include "access/relation.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/lmgr.h"
#include "utils/snapmgr.h"

#include "col/f2/compact.h"
#include "col/f2/f2_batch.h"
#include "col/f2/delete.h"
#include "col/f2/f2_batch.h"
#include "col/f2/f2_store.h"
#include "col/f2/f2_uniq_shm.h"
#include "col/f2/f2_vacuum_run.h"
#include "col/f2/f2.h"
#include "col/f2/l0_href.h"
#include "col/f2/l0_stage_int.h"
#include "col/f2/part_vis.h"
#include "core/hash.h"
#include "core/maint_sched.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "core/quarantine.h"

/* A range of addresses this short is looked for by each address in the table, a longer one by a walk of the rows */
#define SIFT_STG_POINT_PROBES	16

static List *stages = NIL;
static bool callbacks_registered = false;
static uint64 stage_epoch = 0;

void
sift_stage_define_gucs(void)
{
	DefineCustomIntVariable("sift.dead_rewrite_pct",
							"The share of dead rows (percent) at which a part is rewritten without them.",
							NULL, &sift_dead_rewrite_pct, 15, 1, 100, PGC_SUSET, 0, NULL, NULL, NULL);
}

int
sift_stage_route(const SiftManifestRead *m, const uint8 *key)
{
	int			lo = 0,
				hi = m->n;

	while (lo < hi)
	{
		int			mid = lo + ((hi - lo) >> 1);

		if (memcmp(m->entries[mid].key_lo, key, SIFT_BOUND_BYTES) <= 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return Max(lo - 1, 0);
}

int
sift_stage_addr_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

static SiftTxnStage *stage_of(Relation rel);

/* ---- the deletes in memory: found by address, and written as they pass half of work_mem ---- */

#define DEL_HTAB_MIN	64

static uint32
del_flush_cap(void)
{
	return Min(Max(((uint64) work_mem * 1024) / (2 * sizeof(SiftStageDel)), 4096), 65536);
}

static uint32
del_slot(const SiftTxnStage *st, uint64 tid)
{
	return (uint32) ((tid * UINT64CONST(0x9E3779B97F4A7C15)) >> 32) & (st->del_htab_cap - 1);
}

static void
del_htab_put(SiftTxnStage *st, int i)
{
	uint32		p = del_slot(st, st->dels[i].tid);

	while (st->del_htab[p] != 0)
		p = (p + 1) & (st->del_htab_cap - 1);
	st->del_htab[p] = i + 1;
}

/* The table again for the deletes of dels: after a subtransaction dropped some, a flush emptied them, or the table is over three quarters full */
static void
del_htab_rebuild(SiftTxnStage *st)
{
	uint32		cap = pg_nextpower2_32(Max((uint32) st->ndels * 2, DEL_HTAB_MIN));
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	if (st->del_htab != NULL && st->del_htab_cap != cap)
	{
		pfree(st->del_htab);
		st->del_htab = NULL;
	}
	if (st->del_htab == NULL)
		st->del_htab = palloc(cap * sizeof(uint32));
	MemoryContextSwitchTo(old);
	st->del_htab_cap = cap;
	memset(st->del_htab, 0, cap * sizeof(uint32));
	for (int i = 0; i < st->ndels; i++)
		del_htab_put(st, i);
}

bool
sift_stage_find_delete(Relation rel, uint64 tid, TransactionId *xmax, CommandId *cmax, bool *has_succ, uint64 *succ)
{
	const SiftTxnStage *st = stage_of(rel);

	if (st == NULL || st->ndels == 0)
		return false;
	for (uint32 p = del_slot(st, tid); st->del_htab[p] != 0; p = (p + 1) & (st->del_htab_cap - 1))
	{
		const SiftStageDel *d = &st->dels[st->del_htab[p] - 1];

		if (d->tid != tid)
			continue;
		*xmax = d->xmax;
		*cmax = d->cmax;
		*has_succ = d->is_update;
		*succ = d->succ_tid;
		return true;
	}
	return false;
}

/* The rows of the stage go to the partitions, and the keys of a UNIQUE index that the registry held for them with them: they are in the manifest now, and a stage that flushes at each full buffer must not fill the registry */
static void
flush_stage_rows(Relation rel, SiftTxnStage *st, bool at_end)
{
	sift_stage_flush_rows(rel, st, at_end);
	sift_uniq_shm_release(&rel->rd_locator);
}

/* The rows first, then the deletes of what is staged: each delete keeps the xid of the (sub)transaction that made it, so that a rollback to its savepoint hides it by the status of that xid */
static void
flush_deletes_chunk(SiftTxnStage *st)
{
	Relation	rel = relation_open(st->relid, RowExclusiveLock);

	flush_stage_rows(rel, st, false);
	sift_stage_flush_deletes(rel, st, false);
	relation_close(rel, NoLock);
	del_htab_rebuild(st);
}

static void
free_stage(SiftTxnStage *st)
{
	if (st->host_stage != NULL)
		st->host_event(NULL, st, SIFT_STAGE_FREE, 0, 0);
	sift_cdi_cache_free(st);
	sift_stage_reset_parts(st);
	st->ndels = 0;
}

void
sift_stage_discard(Oid relid)
{
	foreach_ptr(SiftTxnStage, st, stages)
		if (st->relid == relid)
		{
			free_stage(st);
			stages = list_delete_ptr(stages, st);
			stage_epoch++;
			return;
		}
}

uint64
sift_stage_epoch(void)
{
	return stage_epoch;
}

/* The stage of the relation as it is now: one made for a file the relation no longer has (TRUNCATE gave it another) holds rows that are not in it, and goes */
static SiftTxnStage *
stage_of(Relation rel)
{
	foreach_ptr(SiftTxnStage, st, stages)
		if (st->relid == RelationGetRelid(rel))
		{
			if (RelFileLocatorEquals(st->locator, rel->rd_locator))
				return st;
			sift_stage_discard(st->relid);
			return NULL;
		}
	return NULL;
}

/* The partitions that got parts to rewrite as parts of the cold tier (sift_part_needs_retier): the maintenance is asked for them once the transaction has committed, when the worker finds the rows settled */
typedef struct RetierAsk
{
	Oid			rel;
	uint64		part_id;
} RetierAsk;

static List *retier_asks = NIL;

void
sift_stage_ask_retier(Relation rel, uint64 part_id)
{
	MemoryContext old;
	RetierAsk  *ask;

	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP || RecoveryInProgress() || IsParallelWorker())
		return;
	foreach_ptr(RetierAsk, have, retier_asks)
		if (have->rel == RelationGetRelid(rel) && have->part_id == part_id)
			return;
	old = MemoryContextSwitchTo(TopTransactionContext);
	ask = palloc(sizeof(RetierAsk));
	*ask = (RetierAsk) {.rel = RelationGetRelid(rel),.part_id = part_id};
	retier_asks = lappend(retier_asks, ask);
	MemoryContextSwitchTo(old);
}

static void
request_retiers(void)
{
	foreach_ptr(RetierAsk, ask, retier_asks)
		(void) sift_maint_enqueue_here(&(SiftMaintReq) {.db = MyDatabaseId,.rel = ask->rel,.kind = SIFT_MK_COMPACT_PART,.tier = SIFT_MAINT_TIER_DEFAULT,.part_id = ask->part_id});
}

static void
stage_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PRE_PREPARE)
	{
		ListCell   *lc;

		PushActiveSnapshot(GetTransactionSnapshot());
		foreach(lc, stages)
		{
			SiftTxnStage *st = lfirst(lc);
			Relation	rel = try_relation_open(st->relid, RowExclusiveLock);

			if (rel == NULL)
				continue;
			if (RelFileLocatorEquals(st->locator, rel->rd_locator) && !sift_quarantine_skips(rel, SIFT_SKIP_WRITE))
			{
				flush_stage_rows(rel, st, true);
				sift_stage_flush_deletes(rel, st, true);
			}
			relation_close(rel, NoLock);
		}
		PopActiveSnapshot();
	}
	else if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE || event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT)
	{
		if (event == XACT_EVENT_COMMIT)
			request_retiers();
		retier_asks = NIL;
		stages = NIL;
		stage_epoch++;
	}
}

static void
stage_subxact(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid, void *arg)
{
	ListCell   *lc;

	if (event != SUBXACT_EVENT_ABORT_SUB && event != SUBXACT_EVENT_COMMIT_SUB)
		return;
	foreach(lc, stages)
	{
		SiftTxnStage *st = lfirst(lc);

		int			kept = 0;

		for (int i = 0; i < st->nparts; i++)
			if (sift_rowbuf_drop_sub(st->parts[i].rb, mySubid, event == SUBXACT_EVENT_COMMIT_SUB ? parentSubid : 0) > 0)
			{
				sift_stage_part_changed(st, &st->parts[i]);
				sift_stage_tid_invalidate(st);
			}
		for (int i = 0; i < st->ndels; i++)
		{
			if (st->dels[i].subxid == mySubid && event == SUBXACT_EVENT_ABORT_SUB)
				continue;
			if (st->dels[i].subxid == mySubid)
				st->dels[i].subxid = parentSubid;
			st->dels[kept++] = st->dels[i];
		}
		st->ndels = kept;
		del_htab_rebuild(st);
		sift_cdi_cache_staged_changed(st);
		if (st->host_stage != NULL)
			st->host_event(NULL, st, event == SUBXACT_EVENT_COMMIT_SUB ? SIFT_STAGE_SUB_COMMIT : SIFT_STAGE_SUB_ABORT, mySubid, parentSubid);
	}
}

static bool
same_key_order(const SiftPartSpec *a, const SiftPartSpec *b)
{
	return a->nsort == b->nsort && memcmp(a->sort_cols, b->sort_cols, a->nsort * sizeof(uint16)) == 0 && memcmp(a->sort_flags, b->sort_flags, a->nsort) == 0;
}

/* A key_order set or reset after the stage was made keys the rows of the caller by the new order: the rows the stage holds are written under the order they were keyed by, and the stage is made again */
SiftTxnStage *
sift_stage_get(Relation rel, const SiftPartSpec *spec)
{
	MemoryContext old;
	SiftTxnStage *st = stage_of(rel);

	if (st != NULL && same_key_order(&st->spec, spec))
		return st;
	if (st != NULL)
	{
		flush_stage_rows(rel, st, false);
		sift_stage_discard(RelationGetRelid(rel));
	}
	if (!callbacks_registered)
	{
		RegisterXactCallback(stage_xact, NULL);
		RegisterSubXactCallback(stage_subxact, NULL);
		callbacks_registered = true;
	}
	old = MemoryContextSwitchTo(TopTransactionContext);
	st = palloc0(sizeof(SiftTxnStage));
	st->relid = RelationGetRelid(rel);
	st->locator = rel->rd_locator;
	st->spec = *spec;
	st->spec.kinds = memcpy(palloc(spec->ncols * sizeof(SiftCellKind)), spec->kinds, spec->ncols * sizeof(SiftCellKind));
	st->spec.sort_cols = memcpy(palloc(Max(spec->nsort, 1) * sizeof(uint16)), spec->sort_cols, spec->nsort * sizeof(uint16));
	st->spec.hints = spec->hints ? memcpy(palloc(spec->ncols), spec->hints, spec->ncols) : NULL;
	st->spec.scales = spec->scales ? memcpy(palloc(spec->ncols), spec->scales, spec->ncols) : NULL;
	st->spec.key_leaf = spec->key_leaf ? memcpy(palloc(spec->ncols), spec->key_leaf, spec->ncols) : NULL;
	st->spec.parent = spec->parent ? memcpy(palloc(spec->ncols * sizeof(int16)), spec->parent, spec->ncols * sizeof(int16)) : NULL;
	st->spec.col_ids = spec->col_ids ? memcpy(palloc(spec->ncol_ids * sizeof(SiftPartColIdentity)), spec->col_ids, spec->ncol_ids * sizeof(SiftPartColIdentity)) : NULL;
	st->spec.defaults = spec->defaults ? memcpy(palloc(spec->defaults_len), spec->defaults, spec->defaults_len) : NULL;
	st->routing = sift_manifest_read_range(rel, NULL, NULL);
	st->tid_min = UINT64_MAX;
	stages = lappend(stages, st);
	stage_epoch++;
	MemoryContextSwitchTo(old);
	return st;
}

static uint32
part_home(const SiftTxnStage *st, uint64 part_id)
{
	return (uint32) (sift_fmix64(part_id) & st->part_htab_mask);
}

static void
part_htab_rebuild(SiftTxnStage *st)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	if (st->part_htab != NULL)
		pfree(st->part_htab);
	st->part_htab_mask = pg_nextpower2_32(Max(4 * (uint32) st->nparts, 16)) - 1;
	st->part_htab = palloc0((st->part_htab_mask + 1) * sizeof(uint32));
	MemoryContextSwitchTo(old);
	for (int i = 0; i < st->nparts; i++)
	{
		uint32		h = part_home(st, st->parts[i].part_id);

		while (st->part_htab[h] != 0)
			h = (h + 1) & st->part_htab_mask;
		st->part_htab[h] = i + 1;
	}
}

SiftStagePart *
sift_stage_part_get(SiftTxnStage *st, const SiftPartitionEntry *e)
{
	MemoryContext old;
	SiftStagePart *p;

	if (st->part_htab != NULL)
		for (uint32 h = part_home(st, e->part_id); st->part_htab[h] != 0; h = (h + 1) & st->part_htab_mask)
			if (st->parts[st->part_htab[h] - 1].part_id == e->part_id)
				return &st->parts[st->part_htab[h] - 1];
	old = MemoryContextSwitchTo(TopTransactionContext);
	if (st->nparts == st->cap_parts)
	{
		st->cap_parts = st->cap_parts > 0 ? st->cap_parts * 2 : 8;
		st->parts = st->parts ? repalloc(st->parts, st->cap_parts * sizeof(SiftStagePart)) : palloc(st->cap_parts * sizeof(SiftStagePart));
	}
	p = &st->parts[st->nparts];
	*p = (SiftStagePart) {.part_id = e->part_id,.rb = sift_rowbuf_new(&st->spec),.tid_min = UINT64_MAX};
	memcpy(p->key_lo, e->key_lo, SIFT_BOUND_BYTES);
	MemoryContextSwitchTo(old);
	st->nparts++;
	if (st->part_htab == NULL || (uint32) st->nparts * 2 > st->part_htab_mask + 1)
		part_htab_rebuild(st);
	else
	{
		uint32		h = part_home(st, e->part_id);

		while (st->part_htab[h] != 0)
			h = (h + 1) & st->part_htab_mask;
		st->part_htab[h] = st->nparts;
	}
	sift_stage_part_changed(st, p);
	return p;
}

void
sift_stage_part_changed(SiftTxnStage *st, SiftStagePart *p)
{
	st->mem_bytes += p->rb->mem_bytes - p->mem_seen;
	st->total_rows += (uint64) p->rb->n - p->rows_seen;
	p->mem_seen = p->rb->mem_bytes;
	p->rows_seen = p->rb->n;
}

void
sift_stage_reset_parts(SiftTxnStage *st)
{
	for (int i = 0; i < st->nparts; i++)
		sift_rowbuf_free(st->parts[i].rb);
	sift_mem_release_os(st->mem_bytes);
	st->nparts = 0;
	st->mem_bytes = 0;
	st->total_rows = 0;
	st->tid_min = UINT64_MAX;
	st->tid_max = 0;
	if (st->part_htab != NULL)
		memset(st->part_htab, 0, (st->part_htab_mask + 1) * sizeof(uint32));
	sift_stage_tid_invalidate(st);
}

void
sift_stage_renew_part(SiftTxnStage *st, SiftStagePart *p)
{
	MemoryContext old;
	Size		freed = p->rb->mem_bytes;

	sift_rowbuf_free(p->rb);
	sift_mem_release_os(freed);
	old = MemoryContextSwitchTo(TopTransactionContext);
	p->rb = sift_rowbuf_new(&st->spec);
	MemoryContextSwitchTo(old);
	p->tid_min = UINT64_MAX;
	p->tid_max = 0;
	sift_stage_part_changed(st, p);
	sift_stage_tid_invalidate(st);
}

/* ---- the rows in memory by address ---- */

static uint32
tid_home(const SiftTxnStage *st, uint64 tid)
{
	return (uint32) (sift_fmix64(tid) & st->tid_htab_mask);
}

/* An empty table for live rows, at most half full: the slots are all ones, which is an empty slot */
static void
tid_htab_alloc(SiftTxnStage *st, uint32 live)
{
	uint32		cap = pg_nextpower2_32(Max(2 * live, 64));

	if (st->tid_htab != NULL)
		pfree(st->tid_htab);
	st->tid_htab = MemoryContextAlloc(TopTransactionContext, cap * sizeof(SiftStageTidSlot));
	memset(st->tid_htab, 0xFF, cap * sizeof(SiftStageTidSlot));
	st->tid_htab_mask = cap - 1;
	st->tid_htab_used = 0;
	st->tid_htab_live = 0;
}

static void
tid_htab_put(SiftTxnStage *st, uint64 tid, uint32 part_idx, uint32 row_idx)
{
	uint32		h = tid_home(st, tid);

	while (st->tid_htab[h].part_idx < SIFT_STG_TID_TOMB)
		h = (h + 1) & st->tid_htab_mask;
	st->tid_htab_used += st->tid_htab[h].part_idx == SIFT_STG_TID_EMPTY;
	st->tid_htab[h] = (SiftStageTidSlot) {.tid = tid,.part_idx = part_idx,.row_idx = row_idx};
	st->tid_htab_live++;
}

static void
tid_htab_build(SiftTxnStage *st)
{
	tid_htab_alloc(st, (uint32) st->total_rows);
	for (int i = 0; i < st->nparts; i++)
		for (int r = 0; r < st->parts[i].rb->n; r++)
			tid_htab_put(st, st->parts[i].rb->tid[r], i, r);
	st->tid_htab_valid = true;
}

static void
tid_htab_ensure(SiftTxnStage *st)
{
	if (!st->tid_htab_valid)
		tid_htab_build(st);
}

void
sift_stage_tid_invalidate(SiftTxnStage *st)
{
	st->tid_htab_valid = false;
}

static void
tid_htab_add(SiftTxnStage *st, uint64 tid, uint32 part_idx, uint32 row_idx)
{
	if ((uint64) (st->tid_htab_used + 1) * 10 > (uint64) (st->tid_htab_mask + 1) * 6)
		tid_htab_build(st);
	else
		tid_htab_put(st, tid, part_idx, row_idx);
}

/* The slot of the row of the buffer at the place (the table is valid, the row is staged) */
static SiftStageTidSlot *
tid_htab_find(SiftTxnStage *st, uint64 tid, uint32 part_idx, uint32 row_idx)
{
	for (uint32 h = tid_home(st, tid);; h = (h + 1) & st->tid_htab_mask)
	{
		SiftStageTidSlot *s = &st->tid_htab[h];

		if (s->part_idx == SIFT_STG_TID_EMPTY)
			return NULL;
		if (s->tid == tid && s->part_idx == part_idx && s->row_idx == row_idx)
			return s;
	}
}

static bool
tid_htab_has(const SiftTxnStage *st, uint64 tid)
{
	for (uint32 h = tid_home(st, tid); st->tid_htab[h].part_idx != SIFT_STG_TID_EMPTY; h = (h + 1) & st->tid_htab_mask)
		if (st->tid_htab[h].tid == tid && st->tid_htab[h].part_idx < SIFT_STG_TID_TOMB)
			return true;
	return false;
}

void
sift_stage_row_added(SiftTxnStage *st, SiftStagePart *p, uint64 tid)
{
	sift_stage_part_changed(st, p);
	st->tid_min = Min(st->tid_min, tid);
	st->tid_max = Max(st->tid_max, tid);
	p->tid_min = Min(p->tid_min, tid);
	p->tid_max = Max(p->tid_max, tid);
	if (st->tid_htab_valid)
		tid_htab_add(st, tid, (uint32) (p - st->parts), (uint32) (p->rb->n - 1));
}

static void
stage_row(Relation rel, SiftTxnStage *st, SiftStagePart *p, const uint8 *key, uint64 tid, uint32 xmin, uint32 cmin, SubTransactionId sub, const SiftCell *cells, const SiftCell *const *elem)
{
	MemoryContext old = MemoryContextSwitchTo(p->rb->cx);

	sift_rowbuf_add(p->rb, tid, xmin, cmin, sub, key, cells, elem);
	MemoryContextSwitchTo(old);
	sift_stage_row_added(st, p, tid);
	if (st->bulk_rows > 0)
		sift_stage_bulk_check(rel, st, p);
	else if (sift_stage_mem(st) >= (uint64) work_mem * 1024)
		sift_stage_flush(rel);
}

void
sift_stage_add(Relation rel, SiftTxnStage *st, const uint8 *key, uint64 tid, uint32 xmin, uint32 cmin, const SiftCell *cells, const SiftCell *const *elem)
{
	SiftStagePart *p = sift_stage_part_get(st, &st->routing.entries[sift_stage_route(&st->routing, key)]);

	stage_row(rel, st, p, key, tid, xmin, cmin, GetCurrentSubTransactionId(), cells, elem);
}

void
sift_stage_add_batch(Relation rel, SiftTxnStage *st, int n, const uint8 (*keys)[SIFT_BOUND_BYTES], const uint64 *tids, uint32 xmin, uint32 cmin, const SiftCell *cells,
						const SiftCell *const *elem)
{
	SubTransactionId sub = GetCurrentSubTransactionId();

	for (int i = 0; i < n; i++)
	{
		SiftStagePart *p = sift_stage_part_get(st, &st->routing.entries[sift_stage_route(&st->routing, keys[i])]);

		stage_row(rel, st, p, keys[i], tids[i], xmin, cmin, sub, cells + (size_t) i * st->spec.ncols,
			  elem != NULL ? elem + (size_t) i * st->spec.ncols : NULL);
	}
}

void
sift_stage_set_bulk(SiftTxnStage *st, uint32 flush_rows, uint64 budget_bytes)
{
	st->bulk_rows = flush_rows;
	st->bulk_budget = budget_bytes;
}

static void
add_delete(SiftTxnStage *st, const uint8 *key, uint64 tid, uint64 succ_tid, TransactionId xmax, bool is_update)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	if (st->ndels == st->capdels)
	{
		st->capdels = Min(Max(64, st->capdels * 2), (int) del_flush_cap());
		st->dels = st->dels ? repalloc(st->dels, st->capdels * sizeof(SiftStageDel)) : palloc(st->capdels * sizeof(SiftStageDel));
	}
	MemoryContextSwitchTo(old);
	st->dels[st->ndels] = (SiftStageDel) {.tid = tid,.succ_tid = succ_tid,.xmax = xmax,.cmax = GetCurrentCommandId(false),
		.subxid = GetCurrentSubTransactionId(),.is_update = is_update};
	memcpy(st->dels[st->ndels++].key, key, SIFT_BOUND_BYTES);
	if (st->del_htab == NULL || (uint32) st->ndels * 4 > st->del_htab_cap * 3)
		del_htab_rebuild(st);
	else
		del_htab_put(st, st->ndels - 1);
	if ((uint32) st->ndels >= del_flush_cap())
		flush_deletes_chunk(st);
}

void
sift_stage_delete(SiftTxnStage *st, const uint8 *key, uint64 tid)
{
	add_delete(st, key, tid, 0, GetCurrentTransactionId(), false);
}

void
sift_stage_delete_update(SiftTxnStage *st, const uint8 *key, uint64 old_tid, uint64 new_tid)
{
	add_delete(st, key, old_tid, new_tid, GetCurrentTransactionId(), true);
}

void
sift_stage_delete_spec_abort(SiftTxnStage *st, const uint8 *key, uint64 tid)
{
	add_delete(st, key, tid, 0, FrozenTransactionId, false);
}

/* The row at the place goes (the table is valid): the table follows the rows the buffer moves */
static void
drop_row(SiftTxnStage *st, uint32 part_idx, uint32 row_idx)
{
	SiftStagePart *p = &st->parts[part_idx];
	SiftRowBuf *rb = p->rb;
	SiftStageTidSlot *gone = tid_htab_find(st, rb->tid[row_idx], part_idx, row_idx);
	uint32		last = rb->n - 1;
	uint64		moved = rb->tid[last];

	gone->part_idx = SIFT_STG_TID_TOMB;
	st->tid_htab_live--;
	sift_rowbuf_drop_at(rb, row_idx);
	if (!sift_rowbuf_drops_move_last(rb))
		for (uint32 r = row_idx; r < (uint32) rb->n; r++)
			tid_htab_find(st, rb->tid[r], part_idx, r + 1)->row_idx = r;
	else if (row_idx != last)
		tid_htab_find(st, moved, part_idx, last)->row_idx = row_idx;
	sift_stage_part_changed(st, p);
}

/* The first buffer that holds a row at the address and the last row of it there, or false */
static bool
tid_last_row(const SiftTxnStage *st, uint64 tid, uint32 *part_idx, uint32 *row_idx)
{
	bool		found = false;

	for (uint32 h = tid_home(st, tid); st->tid_htab[h].part_idx != SIFT_STG_TID_EMPTY; h = (h + 1) & st->tid_htab_mask)
	{
		const SiftStageTidSlot *s = &st->tid_htab[h];

		if (s->tid != tid || s->part_idx >= SIFT_STG_TID_TOMB)
			continue;
		if (!found || s->part_idx < *part_idx || (s->part_idx == *part_idx && s->row_idx > *row_idx))
		{
			*part_idx = s->part_idx;
			*row_idx = s->row_idx;
			found = true;
		}
	}
	return found;
}

bool
sift_stage_cancel_tid(SiftTxnStage *st, uint64 tid)
{
	uint32		part_idx,
				row_idx;
	uint32		first;

	if (st->total_rows == 0 || tid < st->tid_min || tid > st->tid_max)
		return false;
	tid_htab_ensure(st);
	if (!tid_last_row(st, tid, &first, &row_idx))
		return false;
	do
		drop_row(st, first, row_idx);
	while (tid_last_row(st, tid, &part_idx, &row_idx) && part_idx == first);
	return true;
}

bool
sift_stage_cancel_insert(SiftTxnStage *st, uint64 tid)
{
	return sift_stage_cancel_tid(st, tid);
}

static bool
part_holds_in(const SiftStagePart *p, uint64 lo, uint64 hi)
{
	if (p->rb->n == 0 || hi < p->tid_min || lo > p->tid_max)
		return false;
	for (int r = 0; r < p->rb->n; r++)
		if (p->rb->tid[r] >= lo && p->rb->tid[r] <= hi)
			return true;
	return false;
}

bool
sift_stage_holds_in(Relation rel, uint64 lo, uint64 hi)
{
	SiftTxnStage *st = stage_of(rel);

	if (st == NULL)
		return false;
	if (st->host_pending > 0 && (st->host_holds_in == NULL || st->host_holds_in(st->host_stage, lo, hi)))
		return true;
	if (st->total_rows == 0 || lo > hi || hi < st->tid_min || lo > st->tid_max)
		return false;
	if (hi - lo > SIFT_STG_POINT_PROBES)
	{
		for (int i = 0; i < st->nparts; i++)
			if (part_holds_in(&st->parts[i], lo, hi))
				return true;
		return false;
	}
	tid_htab_ensure(st);
	for (uint64 tid = lo;; tid++)
	{
		if (tid_htab_has(st, tid))
			return true;
		if (tid == hi)
			return false;
	}
}

void
sift_stage_rows_among(Relation rel, const uint64 *tids, int n, bool *held)
{
	SiftTxnStage *st = stage_of(rel);

	if (st == NULL || n == 0 || st->total_rows == 0)
		return;
	tid_htab_ensure(st);
	for (int i = 0; i < n; i++)
		if (tid_htab_has(st, tids[i]))
			held[i] = true;
}

const SiftTxnStage *
sift_stage_peek(Relation rel)
{
	return stage_of(rel);
}

void
sift_stage_rebind(Relation rel, const RelFileLocator *locator)
{
	SiftTxnStage *st = stage_of(rel);

	if (st != NULL)
		st->locator = *locator;
}

uint64 *
sift_stage_dead(Relation rel, Snapshot snap, int *n)
{
	const SiftTxnStage *st = stage_of(rel);
	uint64	   *dead = NULL;

	*n = 0;
	if (st == NULL || st->ndels == 0)
		return NULL;
	dead = palloc(st->ndels * sizeof(uint64));
	for (int i = 0; i < st->ndels; i++)
		if (sift_vis_deleted(st->dels[i].xmax, st->dels[i].cmax, snap))
			dead[(*n)++] = st->dels[i].tid;
	qsort(dead, *n, sizeof(uint64), sift_stage_addr_cmp);
	return dead;
}

void
sift_stage_flush(Relation rel)
{
	SiftTxnStage *st = stage_of(rel);

	if (st != NULL)
		flush_stage_rows(rel, st, false);
}

void
sift_stage_flush_all(void)
{
	foreach_ptr(SiftTxnStage, st, stages)
	{
		Relation	rel = relation_open(st->relid, RowExclusiveLock);

		if (RelFileLocatorEquals(st->locator, rel->rd_locator))
			flush_stage_rows(rel, st, false);
		relation_close(rel, NoLock);
	}
}

uint64
sift_stage_rows(Relation rel)
{
	const SiftTxnStage *st = stage_of(rel);

	return st != NULL ? st->total_rows : 0;
}

bool
sift_stage_has_pending(Relation rel)
{
	const SiftTxnStage *st = stage_of(rel);

	return st != NULL && (st->ndels > 0 || st->host_pending > 0 || st->total_rows > 0);
}
