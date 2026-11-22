/*
 * col/l0_stage.c - primitive 1: the transaction's partition-routed columnar stage (col/l0_stage.h).
 */
#include "core/host.h"

#include "access/relation.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/lmgr.h"

#include "col/f2/compact.h"
#include "col/f2/f2_batch.h"
#include "col/f2/delete.h"
#include "col/f2/f2_batch.h"
#include "col/f2/f2_store.h"
#include "col/f2/f2_vacuum_run.h"
#include "col/f2/f2.h"
#include "col/f2/l0_href.h"
#include "col/f2/l0_stage.h"
#include "col/f2/part_vis.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "core/quarantine.h"

static List *stages = NIL;
static bool callbacks_registered = false;
static uint64 stage_epoch = 0;

void
sift_stage_define_gucs(void)
{
	DefineCustomIntVariable("sift.dead_rewrite_pct",
							"The share of dead rows (percent) at which a part is rewritten without them.",
							NULL, &sift_dead_rewrite_pct, 15, 1, 100, PGC_USERSET, 0, NULL, NULL, NULL);
}

static int
route(const SiftManifestRead *m, const uint8 *key)
{
	int			i = 0;

	while (i + 1 < m->n && memcmp(m->entries[i + 1].key_lo, key, SIFT_BOUND_BYTES) <= 0)
		i++;
	return i;
}

static int
del_cmp(const void *a, const void *b)
{
	const SiftStageDel *x = a,
			   *y = b;
	int			c = memcmp(x->key, y->key, SIFT_BOUND_BYTES);

	return c != 0 ? c : (x->tid > y->tid) - (x->tid < y->tid);
}

/* The deletes of a partition (the entry of the manifest read it was cut by): dels[from, to) of the sorted deletes of the transaction */
typedef struct DelRun
{
	int			entry;
	int			from;
	int			to;
	bool		done;
} DelRun;

/* The records of a run: one for a delete, the adjacent pair of the old address and its successor for an update (nrecs of them); the top xid ends them at the commit, each its own while the transaction runs */
static SiftPendDelRec *
run_recs(const SiftTxnStage *st, const DelRun *r, int *nrecs, bool at_end)
{
	SiftPendDelRec *recs = palloc(2 * (r->to - r->from) * sizeof(SiftPendDelRec));
	TransactionId top = GetTopTransactionId();
	int			n = 0;

	for (int i = r->from; i < r->to; i++)
	{
		const SiftStageDel *d = &st->dels[i];

		recs[n++] = (SiftPendDelRec) {.tid = d->tid | (d->is_update ? SIFT_PD_TID_UPDATED : 0),.xmax = at_end ? top : d->xmax,.cmax = d->cmax};
		if (d->is_update)
			recs[n++] = (SiftPendDelRec) {.tid = d->succ_tid | SIFT_PD_TID_SUCC,.xmax = InvalidTransactionId,.cmax = 0};
	}
	*nrecs = n;
	return recs;
}

/* The deletes of the transaction sorted by key, cut into the runs of the partitions they belong to */
static DelRun *
del_runs(SiftTxnStage *st, const SiftManifestRead *mr, int *nruns)
{
	DelRun	   *runs = palloc(Max(st->ndels, 1) * sizeof(DelRun));

	*nruns = 0;
	qsort(st->dels, st->ndels, sizeof(SiftStageDel), del_cmp);
	for (int i = 0, j; i < st->ndels; i = j)
	{
		int			p = route(mr, st->dels[i].key);

		for (j = i; j < st->ndels && route(mr, st->dels[j].key) == p; j++)
			;
		runs[(*nruns)++] = (DelRun) {.entry = p,.from = i,.to = j};
	}
	return runs;
}

static bool
tid_drops(SiftRowBuf *rb, int r, void *arg)
{
	return rb->tid[r] == *(const uint64 *) arg;
}

bool
sift_stage_cancel_tid(SiftTxnStage *st, uint64 tid)
{
	for (int i = 0; i < st->nparts; i++)
		if (sift_rowbuf_drop_if(st->parts[i].rb, tid_drops, &tid) > 0)
			return true;
	return false;
}

/* The deletes of the transaction for the partitions they belong to, after its rows; at the end the transaction's top xid ends them, the command stays */
static void
write_deletes(Relation rel, SiftTxnStage *st, bool at_end)
{
	SiftManifestRead mr;
	DelRun	   *runs;
	int			nruns;

	if (st->ndels == 0)
		return;
	mr = sift_manifest_read_range(rel, NULL, NULL);
	runs = del_runs(st, &mr, &nruns);
	for (int i = 0; i < nruns; i++)
	{
		const SiftPartitionEntry *e = &mr.entries[runs[i].entry];
		int			nrecs;
		SiftPendDelRec *recs = run_recs(st, &runs[i], &nrecs, at_end);

		sift_del_route(rel, e->part_id, e->key_lo, recs, nrecs, &st->spec);
		pfree(recs);
	}
	st->ndels = 0;
}

static void
flush_deletes(Relation rel, SiftTxnStage *st, bool at_end)
{
	MemoryContext scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift stage deletes");
	MemoryContext caller = MemoryContextSwitchTo(scratch);

	write_deletes(rel, st, at_end);
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(scratch);
}

/* Drops the deletes of the runs that were written with the rows of their partition */
static void
drop_done_deletes(SiftTxnStage *st, const DelRun *runs, int nruns)
{
	int			kept = 0;

	for (int r = 0; r < nruns; r++)
		for (int i = runs[r].from; i < runs[r].to && !runs[r].done; i++)
			st->dels[kept++] = st->dels[i];
	st->ndels = kept;
}

static void flush_stage(Relation rel, SiftTxnStage *st, bool at_end);
static void bulk_check(Relation rel, SiftTxnStage *st, SiftStagePart *p);
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

/* The rows first, then the deletes of what is staged: each delete keeps the xid of the (sub)transaction that made it, so that a rollback to its savepoint hides it by the status of that xid */
static void
flush_deletes_chunk(SiftTxnStage *st)
{
	Relation	rel = relation_open(st->relid, RowExclusiveLock);

	flush_stage(rel, st, false);
	flush_deletes(rel, st, false);
	relation_close(rel, NoLock);
	del_htab_rebuild(st);
}

static void
free_stage(SiftTxnStage *st)
{
	if (st->host_stage != NULL)
		st->host_event(NULL, st, SIFT_STAGE_FREE, 0, 0);
	for (int i = 0; i < st->nparts; i++)
		sift_rowbuf_free(st->parts[i].rb);
	st->nparts = 0;
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

static void
stage_xact(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PRE_PREPARE)
	{
		ListCell   *lc;

		foreach(lc, stages)
		{
			SiftTxnStage *st = lfirst(lc);
			Relation	rel = relation_open(st->relid, RowExclusiveLock);

			if (RelFileLocatorEquals(st->locator, rel->rd_locator) && !sift_quarantine_skips(rel, SIFT_SKIP_WRITE))
			{
				flush_stage(rel, st, true);
				flush_deletes(rel, st, true);
			}
			relation_close(rel, NoLock);
		}
	}
	else if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_PREPARE || event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT)
	{
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
			(void) sift_rowbuf_drop_sub(st->parts[i].rb, mySubid, event == SUBXACT_EVENT_COMMIT_SUB ? parentSubid : 0);
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
		if (st->host_stage != NULL)
			st->host_event(NULL, st, event == SUBXACT_EVENT_COMMIT_SUB ? SIFT_STAGE_SUB_COMMIT : SIFT_STAGE_SUB_ABORT, mySubid, parentSubid);
	}
}

SiftTxnStage *
sift_stage_get(Relation rel, const SiftPartSpec *spec)
{
	MemoryContext old;
	SiftTxnStage *st = stage_of(rel);

	if (st != NULL)
		return st;
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
	stages = lappend(stages, st);
	stage_epoch++;
	MemoryContextSwitchTo(old);
	return st;
}

static SiftStagePart *
stage_part(SiftTxnStage *st, const SiftPartitionEntry *e)
{
	MemoryContext old;

	for (int i = 0; i < st->nparts; i++)
		if (st->parts[i].part_id == e->part_id)
			return &st->parts[i];
	old = MemoryContextSwitchTo(TopTransactionContext);
	st->parts = st->parts ? repalloc(st->parts, (st->nparts + 1) * sizeof(SiftStagePart)) : palloc((st->nparts + 1) * sizeof(SiftStagePart));
	st->parts[st->nparts] = (SiftStagePart) {.part_id = e->part_id,.rb = sift_rowbuf_new(&st->spec)};
	memcpy(st->parts[st->nparts].key_lo, e->key_lo, SIFT_BOUND_BYTES);
	MemoryContextSwitchTo(old);
	return &st->parts[st->nparts++];
}

static uint64
stage_mem(const SiftTxnStage *st)
{
	uint64		m = 0;

	for (int i = 0; i < st->nparts; i++)
		m += st->parts[i].rb->mem_bytes;
	return m;
}

void
sift_stage_add(Relation rel, SiftTxnStage *st, const uint8 *key, uint64 tid, uint32 xmin, uint32 cmin, const SiftCell *cells, const SiftCell *const *elem)
{
	SiftStagePart *p = stage_part(st, &st->routing.entries[route(&st->routing, key)]);
	MemoryContext old = MemoryContextSwitchTo(p->rb->cx);

	sift_rowbuf_add(p->rb, tid, xmin, cmin, GetCurrentSubTransactionId(), key, cells, elem);
	MemoryContextSwitchTo(old);
	if (st->bulk_rows > 0)
		bulk_check(rel, st, p);
	else if (stage_mem(st) >= (uint64) work_mem * 1024)
		sift_stage_flush(rel);
}

void
sift_stage_set_bulk(SiftTxnStage *st, uint32 flush_rows, uint64 budget_bytes)
{
	st->bulk_rows = flush_rows;
	st->bulk_budget = budget_bytes;
}

static void
add_delete(SiftTxnStage *st, const uint8 *key, uint64 tid, uint64 succ_tid, bool is_update)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	if (st->ndels == st->capdels)
	{
		st->capdels = Min(Max(64, st->capdels * 2), (int) del_flush_cap());
		st->dels = st->dels ? repalloc(st->dels, st->capdels * sizeof(SiftStageDel)) : palloc(st->capdels * sizeof(SiftStageDel));
	}
	MemoryContextSwitchTo(old);
	st->dels[st->ndels] = (SiftStageDel) {.tid = tid,.succ_tid = succ_tid,.xmax = GetCurrentTransactionId(),.cmax = GetCurrentCommandId(false),
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
	add_delete(st, key, tid, 0, false);
}

void
sift_stage_delete_update(SiftTxnStage *st, const uint8 *key, uint64 old_tid, uint64 new_tid)
{
	add_delete(st, key, old_tid, new_tid, true);
}

void
sift_stage_delete_spec_abort(SiftTxnStage *st, const uint8 *key, uint64 tid)
{
	add_delete(st, key, tid, 0, false);
	st->dels[st->ndels - 1].xmax = FrozenTransactionId;
}

static bool
drop_address(SiftRowBuf *rb, int r, void *arg)
{
	return rb->tid[r] == *(const uint64 *) arg;
}

bool
sift_stage_cancel_insert(SiftTxnStage *st, uint64 tid)
{
	for (int i = 0; i < st->nparts; i++)
		if (sift_rowbuf_drop_if(st->parts[i].rb, drop_address, &tid) > 0)
			return true;
	return false;
}

static int
addr_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

bool
sift_stage_holds_in(Relation rel, uint64 lo, uint64 hi)
{
	const SiftTxnStage *st = stage_of(rel);

	if (st != NULL && st->host_pending > 0)
		return true;
	for (int i = 0; st != NULL && i < st->nparts; i++)
		for (int r = 0; r < st->parts[i].rb->n; r++)
			if (st->parts[i].rb->tid[r] >= lo && st->parts[i].rb->tid[r] <= hi)
				return true;
	return false;
}

typedef struct TidAt
{
	uint64		tid;
	int			at;
} TidAt;

static int
tid_at_cmp(const void *a, const void *b)
{
	return addr_cmp(&((const TidAt *) a)->tid, &((const TidAt *) b)->tid);
}

void
sift_stage_rows_among(Relation rel, const uint64 *tids, int n, bool *held)
{
	const SiftTxnStage *st = stage_of(rel);
	TidAt	   *sorted;

	if (st == NULL || n == 0)
		return;
	sorted = palloc(n * sizeof(TidAt));
	for (int i = 0; i < n; i++)
		sorted[i] = (TidAt) {tids[i], i};
	qsort(sorted, n, sizeof(TidAt), tid_at_cmp);
	for (int p = 0; p < st->nparts; p++)
		for (uint32 r = 0; r < st->parts[p].rb->n; r++)
		{
			uint64		tid = st->parts[p].rb->tid[r];
			int			lo = 0,
						hi = n;

			while (lo < hi)
			{
				int			mid = (lo + hi) / 2;

				if (sorted[mid].tid < tid)
					lo = mid + 1;
				else
					hi = mid;
			}
			for (; lo < n && sorted[lo].tid == tid; lo++)
				held[sorted[lo].at] = true;
		}
	pfree(sorted);
}

const SiftTxnStage *
sift_stage_peek(Relation rel)
{
	return stage_of(rel);
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
	qsort(dead, *n, sizeof(uint64), addr_cmp);
	return dead;
}

/* The wide values of the rows go to their own extents here and nowhere sooner: a transaction that aborts has written none */
static void
spill_wide_blobs(Relation rel, SiftRowBuf *rb)
{
	const SiftF2HostOps *host = sift_f2_store_open(rel)->host;

	if (host->spill_wide_blobs != NULL)
		host->spill_wide_blobs(rel, rb);
}

static bool
fused_run(const SiftStagePart *p, const DelRun *run)
{
	return run != NULL && p->rb->n < SIFT_L0_FUSE_ROWS && run->to - run->from < SIFT_BULK_DEL_MIN;
}

/* The rows of a partition of an index over a heap as references to the tuples the heap holds: the rows are not written */
static bool
append_refs(Relation rel, SiftTxnStage *st, SiftStagePart *p, DelRun *run)
{
	uint32		n = p->rb->n;
	SiftL0HeapRef *refs = palloc(n * sizeof(SiftL0HeapRef));
	bool		done = false;

	for (uint32 r = 0; r < n; r++)
	{
		refs[r] = (SiftL0HeapRef) {.xmin = p->rb->xmin[r]};
		sift_f2_tid_set(&refs[r].ctid, p->rb->tid[r]);
	}
	if (fused_run(p, run))
	{
		int			nrecs;
		SiftPendDelRec *recs = run_recs(st, run, &nrecs, true);

		run->done = sift_l0_href_append_deleting(rel, p->part_id, p->key_lo, refs, n, recs, nrecs);
		pfree(recs);
		done = run->done;
	}
	done = done || sift_l0_href_append(rel, p->part_id, p->key_lo, refs, n);
	pfree(refs);
	return done;
}

/* Writes the rows of one partition as a part (or as references or a slab batch when they are few) */
static void
flush_part(Relation rel, SiftTxnStage *st, SiftStagePart *p, DelRun *run)
{
	SiftColIn  *cols;
	SiftRows	r;
	SiftPartDesc d;
	SiftManifestEdit e = {.part_id = p->part_id,.key_lo = p->key_lo};

	if (p->rb->n == 0)
		return;
	if (!st->by_value && sift_f2_store_open(rel)->host->host_kind == SIFT_F2_HOST_INDEX && p->rb->n <= SIFT_L0_HEAPREF_BATCH_MAX && append_refs(rel, st, p, run))
		return;
	spill_wide_blobs(rel, p->rb);
	if (p->rb->n < SIFT_L0_MICRO_MIN)
	{
		sift_rowbuf_sort(p->rb, &st->spec);
		if (fused_run(p, run))
		{
			int			nrecs;
			SiftPendDelRec *recs = run_recs(st, run, &nrecs, true);

			run->done = sift_l0_slab_append_deleting(rel, p->part_id, p->key_lo, p->rb, recs, nrecs, &st->spec);
			pfree(recs);
			if (run->done)
				return;
		}
		if (sift_l0_slab_append(rel, p->part_id, p->key_lo, p->rb, &st->spec))
			return;
	}
	bool		cold = st->bulk_rows > 0 && p->rb->n >= SIFT_BATCH_COLD_MIN_ROWS;
	uint16		level = cold ? SIFT_MAX_PART_LEVEL : p->rb->n >= SIFT_L0_TO_L1_DIRECT_ROWS ? 1 : 0;
	SiftCompressTier tier = cold ? SIFT_TIER_COLD_L2 : level > 0 ? SIFT_TIER_WARM_L1 : SIFT_TIER_HOT_L0;

	cols = palloc(Max(st->spec.ncols, 1) * sizeof(SiftColIn));
	sift_rowbuf_sort(p->rb, &st->spec);
	sift_rowbuf_cluster(p->rb, &st->spec, tier);
	r = sift_rowbuf_rows(p->rb, cols, &st->spec);
	r.bulk_tail = cold;
	d = sift_part_write_rows(rel, (uint32) p->part_id, sift_manifest_new_gen(rel, 1), level, tier, &r);
	e.add_parts = &d;
	e.nadd = 1;
	sift_manifest_commit(rel, &e);
}

static void
flush_and_renew(Relation rel, SiftTxnStage *st, SiftStagePart *p, DelRun *run)
{
	MemoryContext old;

	flush_part(rel, st, p, run);
	sift_rowbuf_free(p->rb);
	old = MemoryContextSwitchTo(TopTransactionContext);
	p->rb = sift_rowbuf_new(&st->spec);
	MemoryContextSwitchTo(old);
}

/* The buffer of a partition that holds the flush count is written at once; the largest buffer is when all hold the budget (checked every SIFT_BATCH_RESET_ROWS rows of a buffer, not for each row) */
static void
bulk_check(Relation rel, SiftTxnStage *st, SiftStagePart *p)
{
	SiftManifestRead now;
	SiftStagePart *big = p;

	if (p->rb->n < (int) st->bulk_rows && ((p->rb->n % SIFT_BATCH_RESET_ROWS) != 0 || stage_mem(st) < st->bulk_budget))
		return;
	for (int i = 0; i < st->nparts; i++)
		if (st->parts[i].rb->n > big->rb->n)
			big = &st->parts[i];
	now = sift_manifest_read_range(rel, NULL, NULL);
	if (!sift_manifest_same_topology(&now, &st->routing))
		flush_stage(rel, st, false);
	else
		flush_and_renew(rel, st, big, NULL);
}

static DelRun *
run_of(DelRun *runs, int nruns, const SiftManifestRead *mr, uint64 part_id)
{
	for (int i = 0; i < nruns; i++)
		if (mr->entries[runs[i].entry].part_id == part_id)
			return &runs[i];
	return NULL;
}

static void
top_xid_rows(SiftRowBuf *rb)
{
	TransactionId top = GetTopTransactionId();

	for (int r = 0; r < rb->n; r++)
		if (TransactionIdIsNormal(rb->xmin[r]))
			rb->xmin[r] = top;
}

/*
 * A row the transaction staged and deleted again (an INSERT its own DELETE or UPDATE ended) is in no snapshot a later statement can have: at the commit, when no subtransaction can roll back
 * either of the two, both are dropped, and nothing of the row is ever written. The rows and the deletes are matched by the address of the version.
 */
typedef struct Cancel
{
	uint64	   *dead;			/* the addresses of the staged deletes, ascending */
	int			ndead;
	bool	   *hit;
} Cancel;

static bool
cancels(SiftRowBuf *rb, int r, void *arg)
{
	Cancel	   *c = arg;
	const uint64 *at = bsearch(&rb->tid[r], c->dead, c->ndead, sizeof(uint64), addr_cmp);

	if (at == NULL)
		return false;
	c->hit[at - c->dead] = true;
	return true;
}

static void
cancel_staged(SiftTxnStage *st)
{
	Cancel		c = {.ndead = st->ndels};
	int			kept = 0;
	int			cancelled = 0;

	if (st->ndels == 0)
		return;
	c.dead = palloc(st->ndels * sizeof(uint64));
	for (int i = 0; i < st->ndels; i++)
		c.dead[i] = st->dels[i].tid;
	qsort(c.dead, st->ndels, sizeof(uint64), addr_cmp);
	c.hit = palloc0(st->ndels * sizeof(bool));
	for (int i = 0; i < st->nparts; i++)
		cancelled += sift_rowbuf_drop_if(st->parts[i].rb, cancels, &c);
	if (cancelled == 0)
		return;
	for (int i = 0; i < st->ndels; i++)
	{
		const uint64 *at = bsearch(&st->dels[i].tid, c.dead, c.ndead, sizeof(uint64), addr_cmp);
		const uint64 *succ = st->dels[i].is_update ? bsearch(&st->dels[i].succ_tid, c.dead, c.ndead, sizeof(uint64), addr_cmp) : NULL;

		if (c.hit[at - c.dead])
			continue;
		st->dels[i].is_update = st->dels[i].is_update && (succ == NULL || !c.hit[succ - c.dead]);
		st->dels[kept++] = st->dels[i];
	}
	st->ndels = kept;
}

/*
 * Writes the rows the transaction staged. at_end: the transaction is about to commit or prepare, so what is staged is its own and that of its committed subtransactions (an
 * aborted one dropped its rows): they are written under the top transaction id, which is one version of a batch where the subtransactions are one each, and which no reader asks
 * pg_subtrans about. The deletes go with the rows of their partition when few of both are written.
 */
static bool
staged_part(const SiftPartitionEntry *e, void *arg)
{
	const SiftTxnStage *st = arg;

	for (int i = 0; i < st->nparts; i++)
		if (st->parts[i].part_id == e->part_id)
			return true;
	return false;
}

static void
write_stage(Relation rel, SiftTxnStage *st, bool at_end)
{
	SiftManifestRead now = sift_manifest_read_range(rel, NULL, NULL);
	DelRun	   *runs = NULL;
	int			nruns = 0;
	MemoryContext old;

	if (at_end)
	{
		cancel_staged(st);
		for (int i = 0; i < st->nparts; i++)
			top_xid_rows(st->parts[i].rb);
	}
	if (!sift_manifest_same_topology(&now, &st->routing))
	{
		SiftRowBuf *all = sift_rowbuf_new(&st->spec);

		for (int i = 0; i < st->nparts; i++)
			for (int r = 0; r < st->parts[i].rb->n; r++)
				sift_rowbuf_copy_row(all, st->parts[i].rb, r, st->parts[i].rb->xmin[r], st->parts[i].rb->cmin[r], st->parts[i].rb->subxid[r]);
		for (int i = 0; i < st->nparts; i++)
			sift_rowbuf_free(st->parts[i].rb);
		st->nparts = 0;
		old = MemoryContextSwitchTo(TopTransactionContext);
		pfree(st->routing.entries);
		st->routing = now;
		st->routing.entries = memcpy(palloc(now.n * sizeof(SiftPartitionEntry)), now.entries, now.n * sizeof(SiftPartitionEntry));
		MemoryContextSwitchTo(old);
		for (int r = 0; r < all->n; r++)
			sift_rowbuf_copy_row(stage_part(st, &now.entries[route(&now, all->keys[r])])->rb, all, r, all->xmin[r], all->cmin[r], all->subxid[r]);
		sift_rowbuf_free(all);
	}
	if (at_end)
		runs = del_runs(st, &now, &nruns);
	for (int i = 0; i < st->nparts; i++)
		flush_and_renew(rel, st, &st->parts[i], run_of(runs, nruns, &now, st->parts[i].part_id));
	sift_f2_maint_note_parts(rel, now.entries, now.n, staged_part, st);
	if (at_end)
		drop_done_deletes(st, runs, nruns);
}

#ifdef SIFT_TESTING
uint64		sift_stage_flushes = 0;
#endif

/* The buffers that writing parts takes are the flush's own: a statement that flushes at each full index page does not keep them in its executor context */
static void
flush_stage(Relation rel, SiftTxnStage *st, bool at_end)
{
	MemoryContext scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift stage flush");
	MemoryContext caller = MemoryContextSwitchTo(scratch);

#ifdef SIFT_TESTING
	sift_stage_flushes += st->nparts > 0 || st->ndels > 0;
#endif
	write_stage(rel, st, at_end);
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(scratch);
	if (st->host_stage != NULL)
		st->host_event(rel, st, at_end ? SIFT_STAGE_FLUSH_AT_END : SIFT_STAGE_FLUSH, 0, 0);
}

void
sift_stage_flush(Relation rel)
{
	SiftTxnStage *st = stage_of(rel);

	if (st != NULL)
		flush_stage(rel, st, false);
}

void
sift_stage_flush_all(void)
{
	foreach_ptr(SiftTxnStage, st, stages)
	{
		Relation	rel = relation_open(st->relid, RowExclusiveLock);

		if (RelFileLocatorEquals(st->locator, rel->rd_locator))
			flush_stage(rel, st, false);
		relation_close(rel, NoLock);
	}
}

uint64
sift_stage_rows(Relation rel)
{
	const SiftTxnStage *st = stage_of(rel);
	uint64		n = 0;

	for (int i = 0; st != NULL && i < st->nparts; i++)
		n += st->parts[i].rb->n;
	return n;
}

bool
sift_stage_has_pending(Relation rel)
{
	const SiftTxnStage *st = stage_of(rel);

	return st != NULL && (st->ndels > 0 || st->host_pending > 0 || sift_stage_rows(rel) > 0);
}
