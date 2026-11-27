/*
 * agg/par/scan.c - what the two strategies of v6_par.h share: the participant's place at the barrier, the loop that claims morsels, the hand-over of a table to the partitions, and the
 * merge of a partition into an engine state. The morsels are those of core/par/morsel.h over the parts of the scan (the shared block holds the parts, one SiftParPartRef each); the engine decides
 * what a morsel is (group.c: the granules of a part of a sift_col index or of a sift_table, col/f2/sink/sink.h).
 */
#include "core/host.h"

#include "access/parallel.h"
#include "executor/nodeAgg.h"

#include "agg/par/par.h"
#include "agg/topn/topk_run.h"
#include "agg/test_gucs.h"

uint32
sift_v6_par_barrier_event(void)
{
	static uint32 ev;
	uint32		e = __atomic_load_n(&ev, __ATOMIC_RELAXED);

	if (e == 0)
	{
		e = WaitEventExtensionNew("SiftV6ParBarrier");
		__atomic_store_n(&ev, e, __ATOMIC_RELAXED);
	}
	return e;
}

uint64
sift_v6_par_share(const SiftV6ParDsmHdr *hdr)
{
	return Max(Min((uint64) get_hash_memory_limit() / ((uint64) hdr->nworkers_planned + 1), SIFT_PAR_LOCAL_BYTES), SIFT_PAR_MIN_SHARE_BYTES);
}

SiftV6ParRun *
sift_v6_par_run_new(SiftV6ParDsmHdr *hdr, const SiftV6ParEngine *eng, SiftV6TopKRun *topk)
{
	SiftV6ParRun *r = palloc0(sizeof(SiftV6ParRun));

	SIFT_INVARIANT(hdr->magic == SIFT_V6_PAR_DSM_MAGIC, "the shared block of the parallel aggregate is initialized");
	r->hdr = hdr;
	r->coord = sift_v6_par_coord(hdr);
	r->eng = eng;
	r->topk = topk;
	r->stage = SV6S_NEW;
	r->export_cap = sift_v6_par_share(hdr);
	return r;
}

bool
sift_v6_par_local_next(SiftV6ParRun *r, void *local, Datum *values, bool *isnull)
{
	if (r->topk == NULL)
		return r->eng->local_next(local, values, isnull);
	r->eng->local_offer(local, r->topk);
	return false;
}

/* The place at the barrier: the phase it was in when this participant came, 0 when there is a scan to do */
int
sift_v6_par_attach(SiftV6ParRun *r)
{
	int			phase;

	r->pid = (int) pg_atomic_fetch_add_u32(&r->hdr->next_pid, 1);
	phase = BarrierAttach(&r->hdr->phase_barrier);
	r->attached = true;
	return phase;
}

void
sift_v6_par_leave(SiftV6ParRun *r)
{
	if (!r->attached)
		return;
	r->attached = false;
	BarrierDetach(&r->hdr->phase_barrier);
}

/* A guarded run on the stack of the process, and the run it is inside of */
typedef struct Guarded
{
	SiftV6ParRun *run;
	struct Guarded *outer;
} Guarded;

static Guarded *guarded;

/* The exit of the process in guarded runs: the barrier of the others is not left waiting for it, in any run it is inside of */
static void
guarded_exit(int code, Datum arg)
{
	for (Guarded *g = guarded; g != NULL; g = g->outer)
		sift_v6_par_leave(g->run);
}

/* The hook is made once and never cancelled: a callback another part of the process registers inside the run (the first pin of a shared dictionary) cannot get between it and its cancel */
void
sift_v6_par_guarded(SiftV6ParRun *r, void (*body) (SiftV6ParRun *r))
{
	static bool hooked;
	Guarded		g = {r, guarded};

	if (!hooked)
	{
		before_shmem_exit(guarded_exit, 0);
		hooked = true;
	}
	guarded = &g;
	PG_TRY();
	{
		body(r);
	}
	PG_CATCH();
	{
		guarded = g.outer;
		sift_v6_par_leave(r);
		PG_RE_THROW();
	}
	PG_END_TRY();
	guarded = g.outer;
}

void
sift_v6_par_scan(SiftV6ParRun *r, void (*relief) (void *r))
{
	SiftMorselQueue *mq = sift_v6_par_morsels(r->hdr);
	SiftMorsel	m;

	r->eng->scan_hook(r->eng->env, relief, r);
	while (sift_morsel_claim(mq, (uint32) r->pid, &m))
	{
		CHECK_FOR_INTERRUPTS();
		sift_v6_test_item_gate();
		r->eng->scan_morsel(r->local, &m, &r->stats);
		r->stats.morsels_stolen++;
		relief(r);
	}
}

static void
put_record(void *arg, const uint8 *rec, uint32 len)
{
	SiftV6ParRun *r = arg;
	const SiftV6RecHdr *h = (const SiftV6RecHdr *) rec;

	Assert(len == h->len && len % 8 == 0);
	if (r->hdr->strategy == SV6P_RADIX_UNGROUPED_DIST && (h->flags & V6REC_PLAIN))
	{
		r->eng->local_merge(r->plocal, rec);
		return;
	}
	sift_radix_put(r->rl, h->hash64, rec, len);
	r->ring_recs++;
	r->ring_bytes += len;
}

static uint8 *
room_record(void *arg, uint64 hash, uint32 len)
{
	SiftV6ParRun *r = arg;
	uint8	   *rec = sift_radix_stage_slot(r->rl, hash, len);

	if (rec != NULL)
	{
		r->ring_recs++;
		r->ring_bytes += len;
	}
	return rec;
}

/* Hands every group of the participant's table to the partitions (the table is empty afterwards) */
void
sift_v6_par_export(SiftV6ParRun *r)
{
	const SiftV6RecSink sink = {put_record, room_record, r};

	r->eng->local_export(r->local, &sink);
	if (r->hdr->strategy == SV6P_TWO_STAGE && sift_par_coord_spilled(r->coord))
		pg_atomic_write_u32(&r->hdr->escalated, 1);
}

static void
put_slot(void *arg, const uint8 *rec, uint32 len)
{
	SiftV6ParRun *r = arg;
	char	   *slot = (char *) r->hdr + r->hdr->plain_off + (Size) r->pid * r->hdr->plain_slot_bytes;

	SIFT_INVARIANT(len <= r->hdr->plain_slot_bytes && ((const SiftV6RecHdr *) rec)->len == len, "the state of an ungrouped aggregate fits its slot");
	memcpy(slot, rec, len);
}

/* The distinct strategy: the participant's state of the aggregates (what its plain records merged into) goes to the slot of its number, which the final row reads */
void
sift_v6_par_export_plain(SiftV6ParRun *r)
{
	const SiftV6RecSink sink = {put_slot, NULL, r};

	r->eng->local_export(r->plocal, &sink);
}

typedef struct MergeArg
{
	const SiftV6ParEngine *eng;
	void	   *into;
} MergeArg;

static void
merge_block(void *arg, const uint8 *block, uint32 len)
{
	const MergeArg *m = arg;

	m->eng->local_merge_block(m->into, block, len);
	CHECK_FOR_INTERRUPTS();
}

void
sift_v6_par_merge_part(SiftV6ParRun *r, void *into, uint32 part)
{
	MergeArg	m = {r->eng, into};

	sift_radix_scan_part(r->coord, part, merge_block, &m);
}

bool
sift_v6_par_claim_part(SiftV6ParRun *r, uint32 *part)
{
	CHECK_FOR_INTERRUPTS();
	if (!sift_par_next_part(r->coord, part))
		return false;
	r->stats.radix_parts_claimed++;
	return true;
}

/* The records of the participant are all in the partitions (or its files), and counted: what the others read after the barrier */
void
sift_v6_par_finish_stages(SiftV6ParRun *r)
{
	SiftRadixLocal *rl = r->rl;

	r->rl = NULL;
	sift_radix_local_finish(rl);
	pg_atomic_fetch_add_u64(&r->hdr->ring.total_recs, (int64) r->ring_recs);
	pg_atomic_fetch_add_u64(&r->hdr->ring.total_bytes, (int64) r->ring_bytes);
	r->ring_recs = r->ring_bytes = 0;
}

void
sift_v6_par_run_end(SiftV6ParRun *r)
{
	sift_v6_par_leave(r);
	if (r->local != NULL)
		r->eng->local_free(r->local);
	if (r->plocal != NULL)
		r->eng->local_free(r->plocal);
	if (r->rl != NULL)
		sift_radix_local_finish(r->rl);
	if (r->stage != SV6S_NEW)
	{
		SiftV6ParWorkerStats *st = sift_v6_par_stats(r->hdr, r->pid);

		*st = r->stats;
		st->topk_dropped = r->topk != NULL ? sift_v6_topk_run_dropped(r->topk) : 0;
		st->workers = IsParallelWorker();
	}
	pfree(r);
}

bool
sift_v6_par_next(SiftV6ParRun *r, Datum *values, bool *isnull)
{
	switch ((SiftV6ParStrategy) r->hdr->strategy)
	{
		case SV6P_TWO_STAGE:
			return sift_v6_par_two_stage_next(r, values, isnull);
		case SV6P_RADIX_GROUPED:
			return sift_v6_par_radix_next(r, values, isnull);
		case SV6P_RADIX_UNGROUPED_DIST:
			return sift_v6_par_dist_next(r, values, isnull);
		case SV6P_SERIAL:
			break;
	}
	SIFT_INVARIANT(false, "a parallel run has a parallel strategy");
	return false;
}
