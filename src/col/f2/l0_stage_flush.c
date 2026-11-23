/*
 * col/f2/l0_stage_flush.c - primitive 1: the rows and the deletes a transaction staged (col/f2/l0_stage.c) are written for the partitions they belong to.
 */
#include "core/host.h"

#include "access/relation.h"
#include "access/xact.h"
#include "miscadmin.h"

#include "col/f2/compact.h"
#include "col/f2/delete.h"
#include "col/f2/f2.h"
#include "col/f2/f2_batch.h"
#include "col/f2/f2_store.h"
#include "col/f2/f2_vacuum_run.h"
#include "col/f2/l0_href.h"
#include "col/f2/l0_stage_int.h"
#include "core/memtrack.h"

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

void
sift_stage_flush_deletes(Relation rel, SiftTxnStage *st, bool at_end)
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
	bool		bulk = st->bulk_rows > 0 && p->rb->n >= SIFT_BATCH_COLD_MIN_ROWS;
	uint16		level = bulk || p->rb->n >= SIFT_L0_TO_L1_DIRECT_ROWS ? 1 : 0;
	/* a bulk part keeps to the light codecs: the warm tier already trains dictionaries and builds skip indexes, which the cold rewrite of the part does once */
	SiftCompressTier tier = level > 0 && !bulk ? SIFT_TIER_WARM_L1 : SIFT_TIER_HOT_L0;

	cols = palloc(Max(st->spec.ncols, 1) * sizeof(SiftColIn));
	sift_rowbuf_sort(p->rb, &st->spec);
	sift_rowbuf_cluster(p->rb, &st->spec, tier);
	r = sift_rowbuf_rows(p->rb, cols, &st->spec);
	r.bulk_tail = bulk;
	r.part_flags = bulk ? SIFT_PART_FLAG_BUILT : 0;
	st->bulk_warm |= bulk;
	d = sift_part_write_rows(rel, (uint32) p->part_id, sift_manifest_new_gen(rel, 1), level, tier, &r);
	e.add_parts = &d;
	e.nadd = 1;
	sift_manifest_commit(rel, &e);
}

static void
flush_and_renew(Relation rel, SiftTxnStage *st, SiftStagePart *p, DelRun *run)
{
	flush_part(rel, st, p, run);
	sift_stage_renew_part(st, p);
}

/* The buffer of a partition that holds the flush count is written at once; the largest buffer is when all hold the budget (checked every SIFT_BATCH_RESET_ROWS rows of a buffer, not for each row) */
void
sift_stage_bulk_check(Relation rel, SiftTxnStage *st, SiftStagePart *p)
{
	SiftManifestRead now;
	SiftStagePart *big = p;

	if (p->rb->n < (int) st->bulk_rows && ((p->rb->n % SIFT_BATCH_RESET_ROWS) != 0 || sift_stage_mem(st) < st->bulk_budget))
		return;
	for (int i = 0; i < st->nparts; i++)
		if (st->parts[i].rb->n > big->rb->n)
			big = &st->parts[i];
	now = sift_manifest_read_range(rel, NULL, NULL);
	if (!sift_manifest_same_topology(&now, &st->routing))
		sift_stage_flush_rows(rel, st, false);
	else
		flush_and_renew(rel, st, big, NULL);
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

/* The rows of the buffers again by the partitions of the manifest as it is now (a split or a merge changed it since they were routed) */
static void
reroute(SiftTxnStage *st, const SiftManifestRead *now)
{
	SiftRowBuf *all = sift_rowbuf_new(&st->spec);
	MemoryContext old;

	for (int i = 0; i < st->nparts; i++)
		for (int r = 0; r < st->parts[i].rb->n; r++)
			sift_rowbuf_copy_row(all, st->parts[i].rb, r, st->parts[i].rb->xmin[r], st->parts[i].rb->cmin[r], st->parts[i].rb->subxid[r]);
	sift_stage_reset_parts(st);
	old = MemoryContextSwitchTo(TopTransactionContext);
	pfree(st->routing.entries);
	st->routing = *now;
	st->routing.entries = memcpy(palloc(now->n * sizeof(SiftPartitionEntry)), now->entries, now->n * sizeof(SiftPartitionEntry));
	MemoryContextSwitchTo(old);
	for (int r = 0; r < all->n; r++)
	{
		SiftStagePart *p = sift_stage_part_get(st, &now->entries[sift_stage_route(now, all->keys[r])]);

		sift_rowbuf_copy_row(p->rb, all, r, all->xmin[r], all->cmin[r], all->subxid[r]);
		sift_stage_row_added(st, p, all->tid[r]);
	}
	sift_rowbuf_free(all);
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

/* The partitions that got parts of a bulk insert are asked to be compacted by the maintenance once the transaction commits: the plan finds the parts that are to be written again as parts of the cold tier */
static void
ask_retier(Relation rel, SiftTxnStage *st, const SiftManifestRead *now)
{
	for (int i = 0; i < now->n; i++)
		if (staged_part(&now->entries[i], st))
			sift_stage_ask_retier(rel, now->entries[i].part_id);
	st->bulk_warm = false;
}

static void
write_stage(Relation rel, SiftTxnStage *st, bool at_end)
{
	SiftManifestRead now = sift_manifest_read_range(rel, NULL, NULL);
	DelRun	   *runs = NULL;
	int			nruns = 0;

	if (at_end)
	{
		cancel_staged(st);
		for (int i = 0; i < st->nparts; i++)
			top_xid_rows(st->parts[i].rb);
	}
	if (!sift_manifest_same_topology(&now, &st->routing))
		reroute(st, &now);
	if (at_end)
		runs = del_runs(st, &now, &nruns);
	for (int i = 0; i < st->nparts; i++)
		flush_and_renew(rel, st, &st->parts[i], run_of(runs, nruns, &now, &st->parts[i]));
	sift_f2_maint_note_parts(rel, now.entries, now.n, staged_part, st);
	if (st->bulk_warm)
		ask_retier(rel, st, &now);
	if (at_end)
		drop_done_deletes(st, runs, nruns);
}

#ifdef SIFT_TESTING
uint64		sift_stage_flushes = 0;
#endif

/* The buffers that writing parts takes are the flush's own: a statement that flushes at each full index page does not keep them in its executor context */
void
sift_stage_flush_rows(Relation rel, SiftTxnStage *st, bool at_end)
{
	MemoryContext scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift stage flush");
	MemoryContext caller = MemoryContextSwitchTo(scratch);

#ifdef SIFT_TESTING
	sift_stage_flushes += st->nparts > 0 || st->ndels > 0;
#endif
	write_stage(rel, st, at_end);
	sift_cdi_cache_staged_changed(st);
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(scratch);
	if (st->host_stage != NULL)
		st->host_event(rel, st, at_end ? SIFT_STAGE_FLUSH_AT_END : SIFT_STAGE_FLUSH, 0, 0);
}
