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
 * The ring: the worker of a COPY that stays for the command. A run of one group (LOAD_RING_UNITS units) is written by the backend straight into a slot of the ring in
 * shared memory; when the group is full the slot is handed over and the worker encodes it, while the backend goes on reading the input. The parse of the input and the
 * encoding of the rows take about as long, so one worker keeps the backend reading: a second would idle, and groups taken by different participants end in
 * segments of their own, which the merge that follows has to rewrite (as many instructions as writing them took). The worker takes the slots in the order they were
 * handed over, so its writer goes on over the superblocks of the groups one after the other and the segment ends only at the memory budget of the writer (a writer
 * of the table that took the superblocks between two runs ends it too).
 *
 * The blocks a segment takes are in no directory until it is published, so for as long as the ring runs the backend holds SIFT_LOAD_LOCK_BLKNO in ShareLock, which the
 * orphan sweep of a VACUUM only tries for (and leaves the sweep for the next VACUUM when it is not free). The segments are published in order every LOAD_PUBLISH segments
 * and at the end, each time in one change of the directory, under the lock of the active segment only for that change: the load blocks neither the writers of the table
 * nor its maintenance.
 */
#define RING_SLOTS		3		/* the backend fills one, the worker writes one, one waits: the parse and the encoding take turns at being the slower */
#ifdef SIFT_TESTING
/* the phases the leader of the ring waits in for the advisory lock of sift.test_phase_gate + this: before it publishes segments, and before it publishes the last ones */
#define SIFT_TBL_PHASE_PUBLISH	5
#define SIFT_TBL_PHASE_STOP		6
#define load_publish()			(sift_test_load_publish > 0 ? sift_test_load_publish : LOAD_PUBLISH)
#else
#define load_publish()			LOAD_PUBLISH
#endif
#define LOAD_PUBLISH	8		/* segments from which the backend publishes the ones written */
/* the segments a group can close: one for each unit at a memory budget lower than a unit, and one for a gap before it */
#define SLOT_SEGS		(LOAD_RING_UNITS + 1)

enum
{
	SLOT_FREE,
	SLOT_READY,					/* handed over: the run and its rows are the worker's */
	SLOT_DONE					/* written: the segments are the backend's */
};

typedef struct LoadSlot
{
	pg_atomic_uint32 state;
	bool		final;			/* the last group of the load: the worker ends its segment */
	LoadRun		run;
	int			nsegs;
	SiftSegEntry segs[SLOT_SEGS];
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
	uint64		head;			/* slots handed over; the next to fill is head % nslots */
	uint64		tail;			/* slots whose segments are taken */
	int			nsegs;
	SiftSegEntry *segs;			/* the segments taken and not yet published, in the order of their superblocks */
	int			maxsegs;
} LoadRingLeader;

static char *
slot_rows(LoadRing *rg, int i)
{
	return (char *) rg + rg->data_off + (Size) i * rg->cap;
}

PGDLLEXPORT void sift_tbl_load_ring_worker(dsm_segment *seg, shm_toc *toc);

void
sift_tbl_load_ring_worker(dsm_segment *seg, shm_toc *toc)
{
	SiftParWorker w;
	LoadRing   *rg;
	LoadEnc		enc;
	MemoryContext cx;
	bool		begun = false;

	sift_par_worker_begin(&w, toc);
	rg = w.shared;
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table load ring", ALLOCSET_DEFAULT_SIZES);
	for (uint64 n = 0;; n++)
	{
		LoadSlot   *sl = &rg->slots[n % rg->nslots];

		ConditionVariablePrepareToSleep(&rg->cv);
		while (pg_atomic_read_u32(&sl->state) != SLOT_READY && !pg_atomic_read_u32(&rg->closed))
			ConditionVariableSleep(&rg->cv, PG_WAIT_EXTENSION);
		ConditionVariableCancelSleep();
		if (pg_atomic_read_u32(&sl->state) != SLOT_READY)
			break;
		pg_read_barrier();
#ifdef SIFT_TESTING
		sift_test_phase_wait(SIFT_TBL_PHASE_LOAD);
#endif
		if (!begun)
		{
			load_enc_begin(&enc, w.heap, cx, sl->run.vers, load_budget());
			begun = true;
		}
		sl->nsegs = 0;
		load_enc_units(&enc, &sl->run, slot_rows(rg, (int) (n % rg->nslots)), 0, sl->run.units, sl->segs, &sl->nsegs, sl->final);
		pg_write_barrier();
		pg_atomic_write_u32(&sl->state, SLOT_DONE);
		ConditionVariableBroadcast(&rg->cv);
	}
	sift_par_worker_end(&w);
}

/* Takes the segments of the oldest slot once it is written (waits for it when wait); false if it is not */
static bool
ring_reap(Load *ld, bool wait)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;
	LoadSlot   *sl = &rg->slots[rl->tail % rg->nslots];

	if (!wait && pg_atomic_read_u32(&sl->state) != SLOT_DONE)
		return false;
	ConditionVariablePrepareToSleep(&rg->cv);
	while (pg_atomic_read_u32(&sl->state) != SLOT_DONE)
		ConditionVariableSleep(&rg->cv, PG_WAIT_EXTENSION);
	ConditionVariableCancelSleep();
	pg_read_barrier();
	if (rl->nsegs + sl->nsegs > rl->maxsegs)
	{
		rl->maxsegs = Max(rl->maxsegs * 2, rl->nsegs + sl->nsegs);
		rl->segs = repalloc(rl->segs, rl->maxsegs * sizeof(SiftSegEntry));
	}
	memcpy(rl->segs + rl->nsegs, sl->segs, sl->nsegs * sizeof(SiftSegEntry));
	rl->nsegs += sl->nsegs;
	pg_atomic_write_u32(&sl->state, SLOT_FREE);
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
		(void) ring_reap(ld, true);
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

/* The group the backend filled is handed over to the worker; last: it is the end of the load */
void
load_ring_hand_over(Load *ld, Relation rel, SiftTblRel *st, bool last)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;
	LoadSlot   *sl = &rg->slots[rl->head % rg->nslots];

	sl->run = ld->run;
	sl->run.xid = sl->run.vers ? GetCurrentTransactionId() : InvalidTransactionId;
	sl->final = last;
	pg_write_barrier();
	pg_atomic_write_u32(&sl->state, SLOT_READY);
	rl->head++;
	ld->open = false;
	ConditionVariableBroadcast(&rg->cv);
	if (last)
		return;
	while (ring_reap(ld, false))
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
 * Starts the worker and hands it the first group of the load (the rows already read): false, with nothing changed, when it cannot be had.
 */
bool
load_ring_start(Load *ld, Relation rel, SiftTblRel *st)
{
	Size		cap = (Size) LOAD_RING_UNITS * load_unit_bytes() / 4 * 5;
	int			nslots = Max(2, Min(RING_SLOTS, (int) (((Size) maintenance_work_mem * 1024) / cap)));
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
	if (!sift_par_setup(ldr, "sift_tbl_load_ring_worker", 1, head + nslots * cap, sizeof(uint64), false))
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
		pg_atomic_init_u32(&rg->slots[i].state, SLOT_FREE);
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
 * The end of the ring, at the end of the load or when the rest of it goes without the worker: the rows of the open run are the last group (an empty one when there are none,
 * which ends the segment), the worker ends, and the segments not yet published are, with the room the directory needs made when it has none
 */
void
load_ring_stop(Load *ld, Relation rel, SiftTblRel *st)
{
	LoadRingLeader *rl = ld->rl;
	LoadRing   *rg = rl->rg;
	Published	pub;

	if (ld->open && ld->n > 0)
		load_end_unit(ld);
	if (!ld->open)
	{
		load_ring_take_slot(ld, rel);
		ld->run.units = 0;
	}
	load_ring_hand_over(ld, rel, st, true);
	pg_atomic_write_u32(&rg->closed, 1);
	ConditionVariableBroadcast(&rg->cv);
	while (rl->tail < rl->head)
		(void) ring_reap(ld, true);
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
