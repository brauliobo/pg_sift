/*
 * col/f2/compact/par.c - the compaction of a group by the participants of VACUUM (PARALLEL n) (par.h).
 *
 * The leader of the compaction makes a parallel context of n - 1 workers over the inputs of the group, and the granules of all of them are one list that is cut into n shares. A participant takes the
 * next share nobody has, reads the rows of its granules (a share is a run of the granules of one part or of several, so one large part is shared as well as many small ones), drops the dead
 * and the aborted, freezes and writes what is left as parts of its own, and leaves them in the slot of the share. Nothing of the output is reachable until the manifest takes it in: the
 * leader, when every share is written, commits all the parts in the one change of the manifest of a serial compaction (compact.c), with the deletes that reached the inputs meanwhile.
 */
#include "postgres.h"

#include "access/parallel.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "utils/rel.h"

#include "col/f2/compact/compact.h"
#include "col/f2/compact/par.h"
#include "col/f2/f2.h"
#include "col/f2/store/store.h"
#include "core/parbuild.h"
#include "core/xid_cache.h"

/* A participant is worth this many granules of the inputs: a group of fewer than two granules for each is compacted by one */
#define SIFT_COMPACT_PAR_MIN_GRANULES	2

/* The shares of the granules are this many for each participant, so that one that is slow does not keep the others waiting for a fourth of the list */
#define SIFT_COMPACT_PAR_SHARES			2

int			sift_vacuum_parallel_workers = 0;

typedef struct CompactParShared
{
	SiftParCommon common;
	uint32		part_id;
	bool		bounds_hold;
	int64		hot_cut;
	uint64		horizon;
	int			nin;
	uint32		nparticipants;
	uint32		nshares;
	uint32		slot_cap;
	pg_atomic_uint32 next;		/* the first share nobody has taken */
	pg_atomic_uint32 unsettled;	/* a share found a deletion that is not settled: the shares that follow are left */
	/* the nin descriptors of the inputs and the nin counts of their granules follow */
} CompactParShared;

/* What the participant that took a share leaves for the leader */
typedef struct CompactSlot
{
	SiftCompactResult res;
	uint32		nout;
	SiftPartDesc out[FLEXIBLE_ARRAY_MEMBER];
} CompactSlot;

static SiftPartDesc *
inputs_of(CompactParShared *sh)
{
	return (SiftPartDesc *) (sh + 1);
}

static uint32 *
granules_of(CompactParShared *sh)
{
	return (uint32 *) (inputs_of(sh) + sh->nin);
}

static Size
shared_size(int nin)
{
	return add_size(sizeof(CompactParShared), mul_size(nin, sizeof(SiftPartDesc) + sizeof(uint32)));
}

static Size
slot_size(uint32 cap)
{
	return MAXALIGN(offsetof(CompactSlot, out) + (Size) cap * sizeof(SiftPartDesc));
}

static CompactSlot *
slot_at(void *slots, uint32 cap, uint32 share)
{
	return (CompactSlot *) ((char *) slots + (Size) share * slot_size(cap));
}

static const SiftPartSpec *
spec_of(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_INDEX ? &sift_f2_rel(rel)->spec : &sift_f2_store_open(rel)->spec;
}

/* The run of the one list of the granules of the inputs that share s of n is: [from, to) cut to the parts it falls in; how many parts */
static int
share_of(const uint32 *granules, int nin, uint32 s, uint32 n, int *idx, uint32 *lo, uint32 *hi)
{
	uint64		total = 0,
				off = 0,
				from,
				to;
	int			k = 0;

	for (int i = 0; i < nin; i++)
		total += granules[i];
	from = total * s / n;
	to = total * (s + 1) / n;
	for (int i = 0; i < nin; i++)
	{
		uint64		first = Max(off, from),
					end = Min(off + granules[i], to);

		if (first < end)
		{
			idx[k] = i;
			lo[k] = (uint32) (first - off);
			hi[k++] = (uint32) (end - off);
		}
		off += granules[i];
	}
	return k;
}

static void
run_share(Relation rel, CompactParShared *sh, void *slots, uint32 s)
{
	CompactSlot *slot = slot_at(slots, sh->slot_cap, s);
	int		   *idx = palloc(sh->nin * sizeof(int));
	uint32	   *lo = palloc(sh->nin * sizeof(uint32)),
			   *hi = palloc(sh->nin * sizeof(uint32));
	SiftPartDesc *sub = palloc(sh->nin * sizeof(SiftPartDesc));
	int			k = share_of(granules_of(sh), sh->nin, s, sh->nshares, idx, lo, hi);
	int			nout = 0;

	for (int j = 0; j < k; j++)
		sub[j] = inputs_of(sh)[idx[j]];
	if (k > 0 && !sift_compact_write_slice(rel, sh->part_id, spec_of(rel), sub, k, lo, hi, sh->hot_cut, sh->bounds_hold, FullTransactionIdFromU64(sh->horizon), (int) sh->nparticipants, &slot->res, slot->out, (int) sh->slot_cap, &nout))
		pg_atomic_write_u32(&sh->unsettled, 1);
	slot->nout = nout;
	pfree(idx);
	pfree(lo);
	pfree(hi);
	pfree(sub);
}

/* Takes the shares one after another while there are any and no share has found a deletion that is not settled */
static void
run_shares(Relation rel, CompactParShared *sh, void *slots)
{
	uint32		s;

	while (pg_atomic_read_u32(&sh->unsettled) == 0 && (s = pg_atomic_fetch_add_u32(&sh->next, 1)) < sh->nshares)
		run_share(rel, sh, slots, s);
}

PGDLLEXPORT void sift_compact_par_worker(dsm_segment *seg, shm_toc *toc);

void
sift_compact_par_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;

	sift_par_worker_begin(&w, toc);
	run_shares(w.index != NULL ? w.index : w.heap, w.shared, w.parts);
	sift_par_worker_end(&w);
}

static void
res_add(SiftCompactResult *to, const SiftCompactResult *r)
{
	to->new_gen = to->new_gen == 0 ? r->new_gen : to->new_gen;
	to->rows_in += r->rows_in;
	to->rows_out += r->rows_out;
	to->dead_dropped += r->dead_dropped;
	to->aborted_dropped += r->aborted_dropped;
	to->frozen += r->frozen;
	to->stripped = sift_fxid_max(to->stripped, r->stripped);
	to->granules_copied += r->granules_copied;
	to->granules_written += r->granules_written;
	to->bytes_out += r->bytes_out;
	to->writer_bytes = Max(to->writer_bytes, r->writer_bytes);
	to->dict_cols_remapped += r->dict_cols_remapped;
	to->dict_codes_remapped += r->dict_codes_remapped;
}

/* A participant failed: the parts the shares that were done wrote are given back, no manifest will take them */
static void
release_slots(Relation rel, uint64 part_id, void *slots, uint32 cap, uint32 nshares)
{
	for (uint32 s = 0; s < nshares; s++)
	{
		const CompactSlot *slot = slot_at(slots, cap, s);

		sift_compact_release_outputs(rel, part_id, slot->out, (int) slot->nout);
	}
}

/* The participants the group is worth (none in parallel mode: the leader of a parallel VACUUM of the heap is in it, and a worker is): what VACUUM (PARALLEL n) asks for, no more than the leader and the workers max_parallel_maintenance_workers allows, and two granules for each */
static uint32
participants_of(Relation rel, uint64 granules)
{
	if (sift_vacuum_parallel_workers < 2 || IsInParallelMode() || RelationUsesLocalBuffers(rel) || max_parallel_workers < 1)
		return 0;
	return (uint32) Min(Min((uint64) sift_vacuum_parallel_workers, (uint64) max_parallel_maintenance_workers + 1), granules / SIFT_COMPACT_PAR_MIN_GRANULES);
}

SiftCompactParResult
sift_compact_par_write(Relation rel, uint64 part_id, const SiftPartDesc *in, const uint32 *granules, int nin, int64 hot_cut, bool bounds_hold, FullTransactionId horizon, SiftCompactResult *res, SiftPartDesc **out, int *nout)
{
	uint64		total = 0;
	uint32		n,
				nshares;
	CompactParShared *sh;
	SiftParLeader *ldr;
	bool		index = rel->rd_rel->relkind == RELKIND_INDEX;
	bool		unsettled;
	uint32		cap;

	for (int i = 0; i < nin; i++)
		total += granules[i];
	n = participants_of(rel, total);
	if (n < 2)
		return SIFT_COMPACT_PAR_OFF;
	nshares = (uint32) Min((uint64) SIFT_COMPACT_PAR_SHARES * n, total / SIFT_COMPACT_PAR_MIN_GRANULES);
	cap = (uint32) ((total + nshares - 1) / nshares) + 3;
	ldr = sift_par_enter(false);
	if (!sift_par_setup(ldr, "sift_compact_par_worker", (int) n - 1, shared_size(nin), mul_size(slot_size(cap), nshares), false))
		return SIFT_COMPACT_PAR_OFF;
	sh = ldr->shared;
	*sh = (CompactParShared) {.common = {index ? InvalidOid : RelationGetRelid(rel), index ? RelationGetRelid(rel) : InvalidOid, false},.part_id = (uint32) part_id,.bounds_hold = bounds_hold,
		.hot_cut = hot_cut,.horizon = U64FromFullTransactionId(horizon),.nin = nin,.nparticipants = n,.nshares = nshares,.slot_cap = cap};
	pg_atomic_init_u32(&sh->next, 0);
	pg_atomic_init_u32(&sh->unsettled, 0);
	memcpy(inputs_of(sh), in, nin * sizeof(SiftPartDesc));
	memcpy(granules_of(sh), granules, nin * sizeof(uint32));
	memset(ldr->parts, 0, mul_size(slot_size(cap), nshares));
	LaunchParallelWorkers(ldr->pcxt);
	if (ldr->pcxt->nworkers_launched == 0)
	{
		sift_par_end(ldr);
		return SIFT_COMPACT_PAR_OFF;
	}
	PG_TRY();
	{
		WaitForParallelWorkersToAttach(ldr->pcxt);
		run_shares(rel, sh, ldr->parts);
		sift_par_wait(ldr);
	}
	PG_CATCH();
	{
		release_slots(rel, part_id, ldr->parts, cap, nshares);
		PG_RE_THROW();
	}
	PG_END_TRY();
	unsettled = pg_atomic_read_u32(&sh->unsettled) != 0;
	*out = palloc(Max(nshares * cap, 1) * sizeof(SiftPartDesc));
	*nout = 0;
	for (uint32 s = 0; s < nshares; s++)
	{
		const CompactSlot *slot = slot_at(ldr->parts, cap, s);

		res_add(res, &slot->res);
		memcpy(*out + *nout, slot->out, slot->nout * sizeof(SiftPartDesc));
		*nout += (int) slot->nout;
	}
	sift_par_end(ldr);
	return unsettled ? SIFT_COMPACT_PAR_UNSETTLED : SIFT_COMPACT_PAR_DONE;
}
