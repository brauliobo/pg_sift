/*
 * table/load/ring.c - the ring: the workers of a COPY that write the groups while the backend goes on reading the input
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xact.h"
#ifdef SIFT_TESTING
#include "bm25/build.h"
#endif
#include "col/seg.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/condition_variable.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/wait_event.h"

#include "table/load/load_int.h"

/*
 * The ring: the workers of a COPY that stay for the command. A run of one group (LOAD_SBS units) is written by the backend straight into a slot
 * of the ring in shared memory; when the group is full the slot is handed over and a worker writes its segment while the backend goes on
 * reading the input. A slot is claimed group by group with one atomic word (sequence, groups, next group): a late claim of an earlier use of the
 * slot fails on the sequence.
 *
 * The blocks a group takes are in no directory until its segment is published, so for as long as the ring runs the backend holds
 * SIFT_LOAD_LOCK_BLKNO in ShareLock, which the orphan sweep of a VACUUM only tries for (and leaves the sweep for the next VACUUM when it is
 * not free). The segments are published in order every LOAD_PUBLISH groups and at the end, each time in one change of the directory, under
 * the lock of the active segment only for that change: the load blocks neither the writers of the table nor its maintenance.
 */
#define RING_SLOTS_MAX	8
#ifdef SIFT_TESTING
/* the phases the leader of the ring waits in for the advisory lock of sift.test_phase_gate + this: before it publishes segments, and before it publishes the last ones */
#define SIFT_TBL_PHASE_PUBLISH	5
#define SIFT_TBL_PHASE_STOP		6
#define load_publish()			(sift_test_load_publish > 0 ? sift_test_load_publish : LOAD_PUBLISH)
#else
#define load_publish()			LOAD_PUBLISH
#endif
#define LOAD_PUBLISH	8		/* segments from which the backend publishes the ones written */

typedef struct LoadSlot
{
	pg_atomic_uint64 claim;		/* seq << 16 | ngroups << 8 | next group; next == ngroups: nothing to take */
	pg_atomic_uint32 done;		/* groups written */
	LoadRun		run;
	SiftSegEntry segs[LOAD_MAX_GROUPS];
} LoadSlot;

typedef struct LoadRing
{
	SiftParCommon common;
	ConditionVariable cv;		/* a group is ready, or a group is written */
	pg_atomic_uint32 closed;	/* the command has no more groups */
	int			nslots;
	Size		cap;			/* bytes of the rows of a slot */
	Size		data_off;
	LoadSlot	slots[FLEXIBLE_ARRAY_MEMBER];
} LoadRing;

typedef struct LoadRingLeader
{
	SiftParLeader *ldr;
	LoadRing   *rg;
	int			participants;
	uint64		head;			/* slots handed over; the next to fill is head % nslots */
	uint64		tail;			/* slots whose segments are taken */
	uint32		seq;
	int			nsegs;
	SiftSegEntry *segs;			/* the segments of the groups taken and not yet published, in the order of their superblocks */
	int			maxsegs;
} LoadRingLeader;

static char *
slot_rows(LoadRing *rg, int i)
{
	return (char *) rg + rg->data_off + (Size) i * rg->cap;
}

/* The next group nobody has taken, or NULL */
static LoadSlot *
ring_claim(LoadRing *rg, int *g)
{
	for (int i = 0; i < rg->nslots; i++)
	{
		LoadSlot   *sl = &rg->slots[i];
		uint64		c = pg_atomic_read_u64(&sl->claim);

		while ((c & 0xff) < ((c >> 8) & 0xff))
			if (pg_atomic_compare_exchange_u64(&sl->claim, &c, c + 1))
			{
				*g = (int) (c & 0xff);
				return sl;
			}
	}
	return NULL;
}

static void
ring_write(LoadRing *rg, LoadSlot *sl, int g, Relation rel)
{
	int			i = (int) (sl - rg->slots);

	load_write_group(&sl->run, slot_rows(rg, i), rel, g, &sl->segs[g]);
	if (pg_atomic_add_fetch_u32(&sl->done, 1) == ((pg_atomic_read_u64(&sl->claim) >> 8) & 0xff))
		ConditionVariableBroadcast(&rg->cv);
}

PGDLLEXPORT void sift_tbl_load_ring_worker(dsm_segment *seg, shm_toc *toc);

void
sift_tbl_load_ring_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;
	LoadRing   *rg;

	sift_par_worker_begin(&w, toc);
	rg = w.shared;
	for (;;)
	{
		LoadSlot   *sl = NULL;
		int			g = 0;

		ConditionVariablePrepareToSleep(&rg->cv);
		while ((sl = ring_claim(rg, &g)) == NULL && !pg_atomic_read_u32(&rg->closed))
			ConditionVariableSleep(&rg->cv, PG_WAIT_EXTENSION);
		ConditionVariableCancelSleep();
		if (sl == NULL)
			break;
		ring_write(rg, sl, g, w.heap);
	}
	sift_par_worker_end(&w);
}

static bool
slot_done(LoadSlot *sl)
{
	uint64		c = pg_atomic_read_u64(&sl->claim);

	return pg_atomic_read_u32(&sl->done) == ((c >> 8) & 0xff);
}

/* Takes the segments of the oldest slot once it is written (waits for it, writing groups meanwhile, when wait); false if it is not */
static bool
ring_reap(Load *ld, Relation rel, bool wait)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;
	LoadSlot   *sl = &rg->slots[rl->tail % rg->nslots];
	int			g;
	int			ng;

	if (!wait && !slot_done(sl))
		return false;
	ConditionVariablePrepareToSleep(&rg->cv);
	while (!slot_done(sl))
	{
		LoadSlot   *other = ring_claim(rg, &g);

		if (other != NULL)
		{
			ConditionVariableCancelSleep();
			ring_write(rg, other, g, rel);
			ConditionVariablePrepareToSleep(&rg->cv);
			continue;
		}
		ConditionVariableSleep(&rg->cv, PG_WAIT_EXTENSION);
	}
	ConditionVariableCancelSleep();
	pg_read_barrier();
	ng = (int) ((pg_atomic_read_u64(&sl->claim) >> 8) & 0xff);
	if (rl->nsegs + ng > rl->maxsegs)
	{
		rl->maxsegs = Max(rl->maxsegs * 2, rl->nsegs + ng);
		rl->segs = repalloc(rl->segs, rl->maxsegs * sizeof(SiftSegEntry));
	}
	memcpy(rl->segs + rl->nsegs, sl->segs, ng * sizeof(SiftSegEntry));
	rl->nsegs += ng;
	rl->tail++;
	return true;
}

/* The slot the backend fills next: waits for a free one */
void
load_ring_take_slot(Load *ld, Relation rel)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;

	while (rl->head - rl->tail >= (uint64) rg->nslots)
		(void) ring_reap(ld, rel, true);
	ld->rows.data = slot_rows(rg, (int) (rl->head % rg->nslots));
	ld->rows.maxlen = (int) Min(rg->cap, (Size) INT_MAX);
	ld->rows.len = 0;
}

/* The segments taken become part of the directory in one change; false, with nothing published, when it has no room for them (the ring is then stopped) */
static bool
ring_publish(Load *ld, Relation rel, SiftTblRel *st)
{
	LoadRingLeader *rl = ld->rl;
	Published	pub = {rl->segs, rl->nsegs};

	if (rl->nsegs == 0)
		return true;
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_TBL_PHASE_PUBLISH);
#endif
	LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(rel, &st->dir);
	if (st->dir.nsegs + pub.n > sift_tbl_max_segments - 2 || dir_backpressure(st))
	{
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		return false;
	}
	sift_dir_update(rel, &(SiftDirChange) {.fn = load_publish_groups,.arg = &pub,.extra = InvalidBuffer});
	UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	rl->nsegs = 0;
	return true;
}
/* The group the backend filled is handed over to the workers; last: the groups of the rest of the load are shared out between the participants */
void
load_ring_hand_over(Load *ld, Relation rel, SiftTblRel *st, bool last)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;
	LoadSlot   *sl = &rg->slots[rl->head % rg->nslots];
	int			ngroups = 1;

	sl->run = ld->run;
	sl->run.xid = sl->run.vers ? GetCurrentTransactionId() : InvalidTransactionId;
	sl->run.per = LOAD_RING_UNITS;
	if (last)
		ngroups = load_share_units(&sl->run, ld->rows.len, rl->participants);
	pg_atomic_write_u32(&sl->done, 0);
	pg_write_barrier();
	pg_atomic_write_u64(&sl->claim, ((uint64) ++rl->seq << 16) | ((uint64) ngroups << 8));
	rl->head++;
	ld->open = false;
	ConditionVariableBroadcast(&rg->cv);
	if (last)
		return;
	while (ring_reap(ld, rel, false))
		;
	sift_dir_read(rel, &st->dir);
	if (!ring_room(st, rl->nsegs + (int) (rl->head - rl->tail)) || (rl->nsegs >= load_publish() && !ring_publish(ld, rel, st)))
	{
		/* the directory has little room: the rest of the load goes the way of a load without workers, which makes room between its runs */
		ld->noring = true;
		load_ring_stop(ld, rel, st);
	}
}

/*
 * Starts the workers and hands them the first group of the load (the rows of the run already read): false, with nothing changed, when they cannot
 * be had.
 */
bool
load_ring_start(Load *ld, Relation rel, SiftTblRel *st)
{
	int			nworkers = Min(max_parallel_maintenance_workers, LOAD_MAX_GROUPS - 1);
	Size		cap = (Size) LOAD_RING_UNITS * sift_tbl_write_buffer_limit * 1024 / 4 * 5;
	int			nslots = Max(2, Min(Min(nworkers + 1, RING_SLOTS_MAX), (int) (((Size) maintenance_work_mem * 1024) / cap)));
	Size		head = MAXALIGN(offsetof(LoadRing, slots) + nslots * sizeof(LoadSlot));
	SiftParLeader *ldr;
	LoadRingLeader *rl;
	LoadRing   *rg;
	MemoryContext old;

	sift_dir_read(rel, &st->dir);
	if (!ring_room(st, 0) || !ConditionalLockPage(rel, SIFT_LOAD_LOCK_BLKNO, ShareLock))
		return false;
	(void) GetCurrentTransactionId();
	old = MemoryContextSwitchTo(ld->cxt);
	ldr = sift_par_enter(false);
	if (!sift_par_setup(ldr, "sift_tbl_load_ring_worker", nworkers, head + nslots * cap, sizeof(uint64), false))
	{
		MemoryContextSwitchTo(old);
		UnlockPage(rel, SIFT_LOAD_LOCK_BLKNO, ShareLock);
		return false;
	}
	rg = ldr->shared;
	rg->common = (SiftParCommon) {RelationGetRelid(rel), InvalidOid, false};
	ConditionVariableInit(&rg->cv);
	pg_atomic_init_u32(&rg->closed, 0);
	rg->nslots = nslots;
	rg->cap = cap;
	rg->data_off = head;
	for (int i = 0; i < nslots; i++)
	{
		pg_atomic_init_u64(&rg->slots[i].claim, 0);
		pg_atomic_init_u32(&rg->slots[i].done, 0);
	}
	LaunchParallelWorkers(ldr->pcxt);
#ifdef SIFT_TESTING
	sift_test_par_workers = ldr->pcxt->nworkers_launched;
#endif
	if (ldr->pcxt->nworkers_launched == 0)
	{
		sift_par_end(ldr);
		MemoryContextSwitchTo(old);
		UnlockPage(rel, SIFT_LOAD_LOCK_BLKNO, ShareLock);
		return false;
	}
	rl = palloc0(sizeof(LoadRingLeader));
	rl->ldr = ldr;
	rl->rg = rg;
	rl->participants = 1 + ldr->pcxt->nworkers_launched;
	rl->maxsegs = 16;
	rl->segs = palloc(rl->maxsegs * sizeof(SiftSegEntry));
	MemoryContextSwitchTo(old);
	/* the rows already read are the first group */
	memcpy(slot_rows(rg, 0), ld->rows.data, ld->rows.len);
	pfree(ld->rows.data);
	ld->rows.data = slot_rows(rg, 0);
	ld->rows.maxlen = (int) Min(cap, (Size) INT_MAX);
	ld->rl = rl;
	load_ring_hand_over(ld, rel, st, false);
	return true;
}

/*
 * The end of the ring, at the end of the load or when the rest of it goes without the workers: the rows of the open run are shared out, the backend
 * writes with the workers, the workers end, and the segments not yet published are, with the room the directory needs made when it has none
 */
void
load_ring_stop(Load *ld, Relation rel, SiftTblRel *st)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;
	int			g;
	LoadSlot   *sl;
	Published	pub;

	if (ld->open && (ld->n > 0 || ld->run.units > 0))
	{
		if (ld->n > 0)
			load_end_unit(ld);
		if (ld->run.units > 0)
			load_ring_hand_over(ld, rel, st, true);
	}
	ld->open = false;
	pg_atomic_write_u32(&rg->closed, 1);
	ConditionVariableBroadcast(&rg->cv);
	while ((sl = ring_claim(rg, &g)) != NULL)
		ring_write(rg, sl, g, rel);
	while (rl->tail < rl->head)
		(void) ring_reap(ld, rel, true);
	sift_par_wait(rl->ldr);
	sift_par_end(rl->ldr);
	ld->rl = NULL;
	{
		MemoryContext old = MemoryContextSwitchTo(ld->cxt);

		initStringInfo(&ld->rows);
		MemoryContextSwitchTo(old);
	}
	pub = (Published) {rl->segs, rl->nsegs};
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_TBL_PHASE_STOP);
#endif
	if (pub.n > 0)
		load_publish_all(rel, st, &pub);
	UnlockPage(rel, SIFT_LOAD_LOCK_BLKNO, ShareLock);
}
