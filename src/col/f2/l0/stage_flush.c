/*
 * col/f2/l0/stage_flush.c - primitive 1: the rows and the deletes a transaction staged (col/f2/l0/stage.c) are written for the partitions they belong to.
 */
#include "core/host.h"

#include "access/relation.h"
#include "access/xact.h"
#include "miscadmin.h"

#include "col/f2/compact/compact.h"
#include "col/f2/del/delete.h"
#include "col/f2/f2.h"
#include "col/f2/batch.h"
#include "col/f2/store/store.h"
#include "col/f2/vacuum/run.h"
#include "col/f2/l0/shard_dir.h"
#include "col/f2/l0/stage_int.h"
#include "col/f2/uniq/shm.h"
#include "core/memtrack.h"
#include "core/part_lock.h"

/* The times a flush reads the partitions again after a split or a merge took one it was about to write */
#define SIFT_STAGE_ROUTE_TRIES	8

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
	TransactionId top = at_end ? GetTopTransactionId() : InvalidTransactionId;
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

/* The deletes of the transaction sorted by key, cut into the runs of the partitions they belong to: the entries ascend as the deletes do, so one cursor over the entries serves them all */
static DelRun *
del_runs(SiftTxnStage *st, const SiftManifestRead *mr, int *nruns)
{
	DelRun	   *runs = palloc(Max(st->ndels, 1) * sizeof(DelRun));
	int			m = 0;

	*nruns = 0;
	qsort(st->dels, st->ndels, sizeof(SiftStageDel), del_cmp);
	for (int i = 0, j; i < st->ndels; i = j)
	{
		while (m + 1 < mr->n && memcmp(mr->entries[m + 1].key_lo, st->dels[i].key, SIFT_BOUND_BYTES) <= 0)
			m++;
		for (j = i + 1; j < st->ndels && (m + 1 >= mr->n || memcmp(st->dels[j].key, mr->entries[m + 1].key_lo, SIFT_BOUND_BYTES) < 0); j++)
			;
		runs[(*nruns)++] = (DelRun) {.entry = m,.from = i,.to = j};
	}
	return runs;
}

static void
retries_exhausted(Relation rel)
{
	ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					errmsg("sift index \"%s\": its partitions were split or merged %d times while the transaction wrote its rows", RelationGetRelationName(rel), SIFT_STAGE_ROUTE_TRIES)));
}

/* The upper bound of the partition at of the manifest read: the lower bound of the next one, NULL for the last */
static const uint8 *
key_hi_of(const SiftManifestRead *mr, int at)
{
	return at + 1 < mr->n ? mr->entries[at + 1].key_lo : NULL;
}

/* The partition at of the manifest read, with its lock: a split or a merge holds it to change the partition, so nothing changes it until the writer gives it back. False, and no lock, when the manifest has changed that partition since the read */
static bool
lock_partition(Relation rel, const SiftManifestRead *mr, int at)
{
	SiftPartitionEntry e;

	return sift_l0_lock_range(rel, mr->entries[at].part_id, mr->entries[at].key_lo, key_hi_of(mr, at), &e);
}

/* The deletes of the runs from run on are the ones that are left: the earlier ones were written */
static void
keep_deletes_from(SiftTxnStage *st, const DelRun *run)
{
	st->ndels -= run->from;
	memmove(st->dels, st->dels + run->from, st->ndels * sizeof(SiftStageDel));
}

/* The deletes of the transaction for the partitions they belong to, after its rows; at the end the transaction's top xid ends them, the command stays. A partition that a split or a merge took since the manifest was read is read again with the deletes that are left */
static void
write_deletes(Relation rel, SiftTxnStage *st, bool at_end)
{
	for (int tries = 0; st->ndels > 0; tries++)
	{
		SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
		int			nruns;
		DelRun	   *runs = del_runs(st, &mr, &nruns);
		int			i = 0;

		for (; i < nruns && lock_partition(rel, &mr, runs[i].entry); i++)
		{
			const SiftPartitionEntry *e = &mr.entries[runs[i].entry];
			int			nrecs;
			SiftPendDelRec *recs = run_recs(st, &runs[i], &nrecs, at_end);

			sift_del_route(rel, e->part_id, e->key_lo, recs, nrecs, &st->spec);
			sift_part_page_unlock(rel, e->part_id, ExclusiveLock);
			pfree(recs);
		}
		if (i == nruns)
			break;
		if (tries + 1 == SIFT_STAGE_ROUTE_TRIES)
			retries_exhausted(rel);
		keep_deletes_from(st, &runs[i]);
	}
	st->ndels = 0;
}

void
sift_stage_flush_deletes(Relation rel, SiftTxnStage *st, bool at_end)
{
	MemoryContext scratch = sift_mem_allocset_create(TopTransactionContext, "sift stage deletes");
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
static SiftL0Append
append_refs(Relation rel, SiftTxnStage *st, SiftStagePart *p, DelRun *run, const uint8 *key_hi)
{
	uint32		n = p->rb->n;
	SiftL0HeapRef *refs = palloc(n * sizeof(SiftL0HeapRef));
	SiftL0Append done = SIFT_L0_NO_ROOM;

	for (uint32 r = 0; r < n; r++)
	{
		refs[r] = (SiftL0HeapRef) {.xmin = p->rb->xmin[r]};
		sift_f2_tid_set(&refs[r].ctid, p->rb->tid[r]);
	}
	if (fused_run(p, run))
	{
		int			nrecs;
		SiftPendDelRec *recs = run_recs(st, run, &nrecs, true);

		done = sift_l0_href_append_deleting(rel, p->part_id, p->key_lo, key_hi, refs, n, recs, nrecs);
		run->done = done == SIFT_L0_APPENDED;
		pfree(recs);
	}
	if (done == SIFT_L0_NO_ROOM)
		done = sift_l0_href_append(rel, p->part_id, p->key_lo, key_hi, refs, n);
	pfree(refs);
	return done;
}

/* The rows of a partition as one batch of its micro-slab, with the deletes of the run when few of both are written */
static SiftL0Append
append_slab(Relation rel, SiftTxnStage *st, SiftStagePart *p, DelRun *run, const uint8 *key_hi)
{
	SiftL0Append done = SIFT_L0_NO_ROOM;

	sift_rowbuf_sort(p->rb, &st->spec);
	if (fused_run(p, run))
	{
		int			nrecs;
		SiftPendDelRec *recs = run_recs(st, run, &nrecs, true);

		done = sift_l0_slab_append_deleting(rel, p->part_id, p->key_lo, key_hi, p->rb, recs, nrecs, &st->spec);
		run->done = done == SIFT_L0_APPENDED;
		pfree(recs);
	}
	return done == SIFT_L0_NO_ROOM ? sift_l0_slab_append(rel, p->part_id, p->key_lo, key_hi, p->rb, &st->spec) : done;
}

/* The rows of a partition as a part: written with no lock, which is taken only to commit it while the partition is still the one they were routed to; a part the manifest no longer has room for goes back */
static SiftL0Append
write_part(Relation rel, SiftTxnStage *st, SiftStagePart *p, const uint8 *key_hi)
{
	uint16		level = p->rb->n >= SIFT_L0_TO_L1_DIRECT_ROWS ? 1 : 0;
	SiftCompressTier tier = level > 0 ? SIFT_TIER_WARM_L1 : SIFT_TIER_HOT_L0;
	SiftColIn  *cols = palloc(Max(st->spec.ncols, 1) * sizeof(SiftColIn));
	SiftPartitionEntry now;
	SiftRows	r;
	SiftPartDesc d;

	sift_rowbuf_sort(p->rb, &st->spec);
	sift_rowbuf_cluster(p->rb, &st->spec, tier);
	r = sift_rowbuf_rows(p->rb, cols, &st->spec);
	r.bulk_tail = st->bulk_rows > 0 && p->rb->n >= SIFT_BATCH_COLD_MIN_ROWS;
	d = sift_part_write_rows(rel, (uint32) p->part_id, sift_manifest_new_gen(rel, 1), level, tier, &r);
	if (!sift_l0_lock_range(rel, p->part_id, p->key_lo, key_hi, &now))
	{
		sift_compact_release_outputs(rel, p->part_id, &d, 1);
		return SIFT_L0_MOVED;
	}
	sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = p->part_id,.key_lo = p->key_lo,.add_parts = &d,.nadd = 1});
	sift_part_page_unlock(rel, p->part_id, ExclusiveLock);
	return SIFT_L0_APPENDED;
}

/* Writes the rows of one partition as references or a slab batch when they are few, else as a part; SIFT_L0_MOVED, with none of them written, when a split or a merge changed the partition */
static SiftL0Append
flush_part(Relation rel, SiftTxnStage *st, SiftStagePart *p, DelRun *run, const uint8 *key_hi)
{
	SiftL0Append done = SIFT_L0_NO_ROOM;

	if (p->rb->n == 0)
		return SIFT_L0_APPENDED;
	if (!st->by_value && sift_f2_store_open(rel)->host->host_kind == SIFT_F2_HOST_INDEX && p->rb->n <= SIFT_L0_HEAPREF_BATCH_MAX)
		done = append_refs(rel, st, p, run, key_hi);
	if (done != SIFT_L0_NO_ROOM)
		return done;
	spill_wide_blobs(rel, p->rb);
	if (p->rb->n < SIFT_L0_MICRO_MIN)
		done = append_slab(rel, st, p, run, key_hi);
	return done == SIFT_L0_NO_ROOM ? write_part(rel, st, p, key_hi) : done;
}


/* The context goes with what it held, which is given back to the system */
static void
end_scratch(MemoryContext scratch)
{
	sift_mem_release_os(MemoryContextMemAllocated(scratch, true));
	MemoryContextDelete(scratch);
}

/* The partition whose buffer holds the most memory: the one a flush frees the most with */
static SiftStagePart *
largest_buffer(SiftTxnStage *st)
{
	SiftStagePart *big = &st->parts[0];

	for (int i = 1; i < st->nparts; i++)
		if (st->parts[i].rb->mem_bytes > big->rb->mem_bytes)
			big = &st->parts[i];
	return big;
}

/*
 * The buffer of a partition that holds the flush count is written at once; the largest buffer when all of them hold the budget (checked for each row: the memory of the buffers is counted as rows
 * are added). The part is written in a context of its own, which goes with it: what the write holds (the encoded granules, the dictionaries, the zones) is the size of the part, which a
 * statement that loads a whole table would keep to its end otherwise. The flush count was estimated from the first rows, which carry the fixed memory of the buffers: a buffer that reaches it counts
 * again from its own rows, and holds more of them when the budget does. When no row is left in the stage, every key it held is in the partitions and the entries of the index in the
 * registry of unique keys (col/f2/uniq/shm.h) go.
 */
void
sift_stage_bulk_check(Relation rel, SiftTxnStage *st, SiftStagePart *p)
{
	SiftStagePart *victim;
	MemoryContext scratch;
	MemoryContext caller;
	SiftManifestRead now;
	bool		same;
	int			at;

	if (p->rb->n == (int) st->bulk_rows)
		st->bulk_rows = Max(st->bulk_rows, sift_batch_flush_rows(st->bulk_budget, p->rb->mem_bytes / p->rb->n));
	victim = p->rb->n >= (int) st->bulk_rows ? p : largest_buffer(st);
	if (p->rb->n < (int) st->bulk_rows && st->mem_bytes < st->bulk_budget)
		return;
	scratch = sift_mem_allocset_create(TopTransactionContext, "sift stage bulk flush");
	caller = MemoryContextSwitchTo(scratch);
	now = sift_manifest_read_range(rel, NULL, NULL);
	at = sift_stage_route(&now, victim->key_lo);
	same = sift_manifest_same_topology(&now, &st->routing) && flush_part(rel, st, victim, NULL, key_hi_of(&now, at)) != SIFT_L0_MOVED;
	MemoryContextSwitchTo(caller);
	end_scratch(scratch);
	if (same)
		sift_stage_renew_part(st, victim);
	else
		sift_stage_flush_rows(rel, st, false);
	if (st->total_rows == 0)
		sift_uniq_shm_release(&rel->rd_locator);
}

/* The run of the deletes of the partition: the runs ascend by the lower bound of their entry, as the parts do */
static DelRun *
run_of(DelRun *runs, int nruns, const SiftManifestRead *mr, const SiftStagePart *p)
{
	int			lo = 0,
				hi = nruns;

	while (lo < hi)
	{
		int			mid = lo + ((hi - lo) >> 1);

		if (memcmp(mr->entries[runs[mid].entry].key_lo, p->key_lo, SIFT_BOUND_BYTES) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < nruns && mr->entries[runs[lo].entry].part_id == p->part_id ? &runs[lo] : NULL;
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
	const uint64 *at = bsearch(&rb->tid[r], c->dead, c->ndead, sizeof(uint64), sift_stage_addr_cmp);

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
	qsort(c.dead, st->ndels, sizeof(uint64), sift_stage_addr_cmp);
	c.hit = palloc0(st->ndels * sizeof(bool));
	for (int i = 0; i < st->nparts; i++)
	{
		cancelled += sift_rowbuf_drop_if(st->parts[i].rb, cancels, &c);
		sift_stage_part_changed(st, &st->parts[i]);
	}
	if (cancelled == 0)
	{
		pfree(c.dead);
		pfree(c.hit);
		return;
	}
	sift_stage_tid_invalidate(st);
	for (int i = 0; i < st->ndels; i++)
	{
		const uint64 *at = bsearch(&st->dels[i].tid, c.dead, c.ndead, sizeof(uint64), sift_stage_addr_cmp);
		const uint64 *succ = st->dels[i].is_update ? bsearch(&st->dels[i].succ_tid, c.dead, c.ndead, sizeof(uint64), sift_stage_addr_cmp) : NULL;

		if (c.hit[at - c.dead])
			continue;
		st->dels[i].is_update = st->dels[i].is_update && (succ == NULL || !c.hit[succ - c.dead]);
		st->dels[kept++] = st->dels[i];
	}
	st->ndels = kept;
	pfree(c.dead);
	pfree(c.hit);
}

/* The rows of the buffers again by the partitions of the manifest as it is now (a split or a merge changed it since they were routed), one old buffer at a time: it is freed once its rows are in the new ones, so the rows are never held three times */
static void
reroute(SiftTxnStage *st, const SiftManifestRead *now)
{
	SiftStagePart *old = st->parts;
	int			nold = st->nparts;
	MemoryContext prev;

	st->parts = NULL;
	st->nparts = 0;
	st->cap_parts = 0;
	sift_stage_reset_parts(st);
	prev = MemoryContextSwitchTo(TopTransactionContext);
	pfree(st->routing.entries);
	st->routing = *now;
	st->routing.entries = memcpy(palloc(now->n * sizeof(SiftPartitionEntry)), now->entries, now->n * sizeof(SiftPartitionEntry));
	MemoryContextSwitchTo(prev);
	for (int i = 0; i < nold; i++)
	{
		const SiftRowBuf *from = old[i].rb;

		for (int r = 0; r < from->n; r++)
		{
			SiftStagePart *p = sift_stage_part_get(st, &now->entries[sift_stage_route(now, from->keys[r])]);

			sift_rowbuf_copy_row(p->rb, from, r, from->xmin[r], from->cmin[r], from->subxid[r]);
			sift_stage_row_added(st, p, from->tid[r]);
		}
		sift_rowbuf_free(old[i].rb);
	}
	if (old != NULL)
		pfree(old);
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

/*
 * Each buffer for its partition, which the writes lock themselves for as long as they change it: the writers of a hot partition go to its directory without the lock. False when a split or a merge
 * took a partition since the manifest was read, with the buffers of the partitions before it written and renewed
 */
static bool
flush_parts(Relation rel, SiftTxnStage *st, const SiftManifestRead *now, DelRun *runs, int nruns)
{
	for (int i = 0; i < st->nparts; i++)
	{
		SiftStagePart *p = &st->parts[i];

		if (flush_part(rel, st, p, run_of(runs, nruns, now, p), key_hi_of(now, sift_stage_route(now, p->key_lo))) == SIFT_L0_MOVED)
			return false;
		sift_stage_renew_part(st, p);
	}
	return true;
}

static void
write_stage(Relation rel, SiftTxnStage *st, bool at_end)
{
	SiftManifestRead now;
	DelRun	   *runs;
	int			nruns;

	if (at_end)
	{
		cancel_staged(st);
		for (int i = 0; i < st->nparts; i++)
			top_xid_rows(st->parts[i].rb);
	}
	for (int tries = 0;; tries++)
	{
		now = sift_manifest_read_range(rel, NULL, NULL);
		if (!sift_manifest_same_topology(&now, &st->routing))
			reroute(st, &now);
		nruns = 0;
		runs = at_end ? del_runs(st, &now, &nruns) : NULL;
		if (flush_parts(rel, st, &now, runs, nruns))
			break;
		if (tries + 1 == SIFT_STAGE_ROUTE_TRIES)
			retries_exhausted(rel);
		if (at_end)
			drop_done_deletes(st, runs, nruns);
	}
	sift_f2_maint_note_parts(rel, now.entries, now.n, staged_part, st);
	if (at_end)
		drop_done_deletes(st, runs, nruns);
}

#ifdef SIFT_TESTING
uint64		sift_stage_flushes = 0;
#endif

/*
 * The buffers that writing parts takes are the flush's own: a statement that flushes at each full index page does not keep them in its executor context. The context hangs from the
 * transaction, not from the caller's: an error leaves it behind, and a long-lived caller context (the message of an apply worker) would keep it until the process ends.
 */
void
sift_stage_flush_rows(Relation rel, SiftTxnStage *st, bool at_end)
{
	MemoryContext scratch = sift_mem_allocset_create(TopTransactionContext, "sift stage flush");
	MemoryContext caller = MemoryContextSwitchTo(scratch);

#ifdef SIFT_TESTING
	sift_stage_flushes += st->nparts > 0 || st->ndels > 0;
#endif
	write_stage(rel, st, at_end);
	sift_cdi_cache_staged_changed(st);
	MemoryContextSwitchTo(caller);
	end_scratch(scratch);
	if (st->host_stage != NULL)
		st->host_event(rel, st, at_end ? SIFT_STAGE_FLUSH_AT_END : SIFT_STAGE_FLUSH, 0, 0);
}
