/*
 * core/maint_sched.h - SiftMaintShmem: the one queue of the background maintenance of sift, and the activation of the workers that drain it.
 *
 * 512 slots of 128 B in shared memory, one lock held for a few hundred ns, tiers P0..P3 that are the tiers of maint_pacer.h. A request is
 * keyed by (database, relation, kind): a second request of the same key is coalesced into the first (the higher priority wins), and one that
 * arrives while a worker has the key in flight sets a rerun flag, so no key is ever worked on by two processes and no trigger is lost.
 *
 *   score   P0 10000..29999 (the mark of a quarantine 20000+)   P1 1000..9999   P2 200..999   P3 50..199
 *   aging   a waiting P1..P3 request gains SIFT_MAINT_AGE_STEP points for every SIFT_MAINT_AGE_QUANTUM_US, to at most 9999: it ends up ahead
 *           of every younger P1 request and never ahead of a P0 one. Every SIFT_MAINT_FAIR_EVERY-th claim takes the oldest aged request of
 *           any tier, so even a steady stream of P0 requests cannot hold a request back for ever.
 *   claims  a claim records the pid of its owner. The reaper gives back the claim of a pid that is gone, and the slot of a worker that
 *           never started (its lease ran out) or is gone, so a worker that dies leaves nothing behind.
 *   hosts   the clock, the liveness of a pid, the start of a worker and the pacer go through SiftMaintHost: the server maps it to latches,
 *           BackendPidGetProc, RegisterDynamicBackgroundWorker and maint_pacer.c; the standalone test maps it to a fake clock and threads.
 *
 * Without shared_preload_libraries = 'pg_sift' the table is made in the dynamic shared memory registry by the first backend that asks and
 * the backend that enqueues starts the worker itself; with it, one launcher sleeps on its latch and starts them. Where the table cannot be
 * had (the registry is full) or no worker slot is free, nothing fails: a request is refused with a result, a queued one waits in the table
 * for sift.maintain() or the next worker, and the pacer is told that a tier found no free worker.
 */
#ifndef SIFT_MAINT_SCHED_H
#define SIFT_MAINT_SCHED_H

#include "postgres.h"

#include "datatype/timestamp.h"
#include "port/atomics.h"

#include "core/f2/f2_pace.h"
#include "core/shm_port.h"

#ifdef SIFT_SHM_STANDALONE
typedef enum SiftPacerTier
{
	SIFT_TIER_P0,
	SIFT_TIER_P1,
	SIFT_TIER_P2,
	SIFT_TIER_P3,
	SIFT_TIERS
} SiftPacerTier;
#else
#include "core/maint_pacer.h"
#endif

#define SIFT_MAINT_SLOTS			512
#define SIFT_MAINT_HEAT				1024
#define SIFT_MAINT_MAX_WORKERS		8
#define SIFT_MAINT_AGE_QUANTUM_US	(60 * USECS_PER_SEC)
#define SIFT_MAINT_AGE_STEP			1000
#define SIFT_MAINT_AGED_MAX			9999
#define SIFT_MAINT_FAIR_EVERY		16
#define SIFT_MAINT_LEASE_US			(30 * USECS_PER_SEC)
#define SIFT_MAINT_BUSY_US			(500 * 1000)
#define SIFT_MAINT_BACKOFF_MAX_MS	3600000
#define SIFT_MAINT_IDLE_NAPS		2
#define SIFT_MAINT_DEFER_US			(5 * USECS_PER_SEC)
#define SIFT_MAINT_TIER_DEFAULT		(-1)

typedef enum SiftMaintKind
{
	SIFT_MK_QUARANTINE_MARK,
	SIFT_MK_SLAB_FLUSH,
	SIFT_MK_PEND_DEL_FOLD,
	SIFT_MK_COMPACT_PART,
	SIFT_MK_SCOPE_REFRESH,
	SIFT_MK_BM25_MERGE,
	SIFT_MK_SPLIT_PART,
	SIFT_MK_MERGE_PART,
	SIFT_MK_TAIL_SHRINK,
	SIFT_MK_N
} SiftMaintKind;

typedef enum SiftMaintPhase
{
	SIFT_PHASE_QUEUED,
	SIFT_PHASE_SCAN,
	SIFT_PHASE_WRITE,
	SIFT_PHASE_RELOCATE,
	SIFT_PHASE_TRUNCATE,
	SIFT_PHASE_WAITING_PACE
} SiftMaintPhase;

typedef enum SiftMaintState
{
	SIFT_SLOT_FREE,
	SIFT_SLOT_QUEUED,
	SIFT_SLOT_CLAIMED
} SiftMaintState;

typedef enum SiftMaintEnq
{
	SIFT_ENQ_QUEUED,
	SIFT_ENQ_COALESCED,
	SIFT_ENQ_EVICTED,
	SIFT_ENQ_FULL,
	SIFT_ENQ_UNAVAILABLE
} SiftMaintEnq;

typedef enum SiftMaintStep
{
	SIFT_MSTEP_DONE,
	SIFT_MSTEP_MORE,
	SIFT_MSTEP_BUSY,
	SIFT_MSTEP_DEFERRED,
	SIFT_MSTEP_FAILED,
	SIFT_MSTEP_GONE
} SiftMaintStep;

typedef enum SiftMaintMode
{
	SIFT_MAINT_MODE_OFF,
	SIFT_MAINT_MODE_LOCAL,
	SIFT_MAINT_MODE_DYNAMIC,
	SIFT_MAINT_MODE_LAUNCHER
} SiftMaintMode;

typedef enum SiftMaintKick
{
	SIFT_KICK_NONE,
	SIFT_KICK_WOKE,
	SIFT_KICK_SPAWNED,
	SIFT_KICK_NOSLOT,
	SIFT_KICK_DEFERRED
} SiftMaintKick;

typedef struct SiftMaintReq
{
	Oid			db;
	Oid			rel;
	SiftMaintKind kind;
	int			tier;			/* SIFT_MAINT_TIER_DEFAULT: the tier of the kind */
	uint32		urgency;		/* points above the floor of the tier, the heat of the relation scales them in P1 */
	uint64		part_id;		/* 0: the whole relation; two requests for different parts coalesce into a whole-relation one */
} SiftMaintReq;

typedef struct SiftMaintClaim
{
	int			slot;
	uint32		gen;
	Oid			db;
	Oid			rel;
	SiftMaintKind kind;
	SiftPacerTier tier;
	uint64		part_id;
	uint32		score;
	uint32		failures;
	TimestampTz enqueued_at;
} SiftMaintClaim;

typedef struct SiftMaintSlot
{
	Oid			db;
	Oid			rel;
	uint8		kind;
	uint8		tier;
	uint8		state;
	uint8		rerun;
	uint32		score;
	uint64		part_id;
	TimestampTz enqueued_at;
	TimestampTz not_before;
	TimestampTz claimed_at;
	int32		pid;
	uint32		gen;
	uint32		failures;
	uint32		crashes;
	uint8		phase;
	uint8		pad0[7];
	uint64		done;
	uint64		total;
	uint64		bytes_written;
	uint8		pad1[32];
} SiftMaintSlot;

typedef struct SiftMaintWorkerSlot
{
	uint8		state;			/* 0 free, 1 starting (the lease holds the slot), 2 running */
	int32		pid;
	int32		procno;
	Oid			db;
	TimestampTz lease_until;
} SiftMaintWorkerSlot;

typedef struct SiftMaintStats
{
	uint64		enqueued;
	uint64		coalesced;
	uint64		upgraded;
	uint64		evicted;
	uint64		overflowed;
	uint64		claimed;
	uint64		completed;
	uint64		reclaimed;
	uint64		fair_claims;
	uint64		spawned;
	uint64		spawn_failed;
	uint64		lease_lapsed;
} SiftMaintStats;

typedef struct SiftMaintShmem
{
	union
	{
		struct
		{
			SiftShmLock lock;
			pg_atomic_uint32 seq;			/* odd while a writer changes the table: the snapshot retries */
			pg_atomic_uint32 active_workers;
			pg_atomic_uint32 max_workers;	/* sift.max_maint_workers as the host configured it, 0..8 */
			pg_atomic_uint32 launcher;		/* procno + 1 of the launcher, 0 when there is none */
			uint32		naptime_ms;
			uint32		nqueued;
			uint32		nclaimed;
			uint32		claim_seq;
			uint32		next_gen;
			uint32		magic;
			uint32		tranche;
		};
		char		pad0[PG_CACHE_LINE_SIZE];
	};
	union
	{
		SiftMaintStats stats;
		char		pad1[PG_CACHE_LINE_SIZE];
	};
	union
	{
		struct
		{
			pg_atomic_uint64 fg_write_ops;	/* sampled 1 in 64 by the foreground, a line of their own */
			pg_atomic_uint64 fg_read_ops;
		};
		char		pad2[PG_CACHE_LINE_SIZE];
	};
	SiftMaintWorkerSlot worker[SIFT_MAINT_MAX_WORKERS];
	uint8		pad3[64];
	SiftStandbyHorizon horizon;		/* the primary's horizon as it was a minute ago: what a maintenance pass strips at (core/f2/f2_pace.h) */
	uint8		pad4[3 * PG_CACHE_LINE_SIZE - sizeof(SiftStandbyHorizon)];
	pg_atomic_uint32 heat[SIFT_MAINT_HEAT];	/* reads of a relation, sampled 1 in 128 */
	uint32		khash[SIFT_MAINT_SLOTS];	/* 0 for a free slot: the dedup scan reads 2 kB, not 64 kB */
	SiftMaintSlot slot[SIFT_MAINT_SLOTS];
} SiftMaintShmem;

typedef struct SiftMaintRow
{
	int32		pid;
	Oid			db;
	Oid			rel;
	uint64		part_id;
	SiftMaintKind kind;
	SiftPacerTier tier;
	uint32		priority;
	SiftMaintPhase phase;
	uint64		done;
	uint64		total;
	uint64		bytes_written;
	TimestampTz started_at;
	bool		queued;
} SiftMaintRow;

/* What sift.maint_pacer() reads: the workers are the queue's, the mode and the delay are the pacer's (the host fills them) */
typedef struct SiftMaintStatus
{
	uint32		workers_active;
	uint32		workers_max;
	uint32		queued[SIFT_TIERS];
	uint32		claimed;
	bool		cold_deferred;
	SiftMaintStats stats;
} SiftMaintStatus;

/* Everything the queue asks of its host; the server's is sift_maint_server_host */
typedef struct SiftMaintHost
{
	TimestampTz (*now) (void);
	int			(*self_pid) (void);
	bool		(*pid_alive) (int pid);
	bool		(*spawn) (Oid db, int worker);		/* false: the server has no slot */
	void		(*wake) (int procno);
	bool		(*defer_cold) (void);				/* the pacer: P2 and P3 wait for a quieter moment */
	void		(*note_starved) (SiftPacerTier tier, TimestampTz now);
	bool		(*should_yield) (SiftPacerTier tier, TimestampTz now);	/* the pacer: a higher tier found no worker free, this one leaves after its step */
	int			(*bgworkers_in_use) (void);
	int			(*other_workers) (void);			/* the workers of the other pool of the server (core/worker.h): sift.max_maint_workers counts them with these */
} SiftMaintHost;

typedef SiftMaintStep (*SiftMaintStepFn) (const SiftMaintClaim *claim, void *arg);

extern Size sift_maint_shmem_size(void);
extern void sift_maint_shmem_init(SiftMaintShmem *m, int tranche_id, uint32 max_workers, uint32 naptime_ms);
extern void sift_maint_set_limits(SiftMaintShmem *m, uint32 max_workers, uint32 naptime_ms);

extern SiftMaintKind sift_maint_kind_of_name(const char *job);
extern const char *sift_maint_job_name(SiftMaintKind kind, SiftPacerTier tier);
extern const char *sift_maint_phase_name(SiftMaintPhase phase);
extern SiftPacerTier sift_maint_kind_tier(SiftMaintKind kind);
extern uint32 sift_maint_score(SiftPacerTier tier, uint32 urgency);
extern uint32 sift_maint_aged_score(uint32 score, SiftPacerTier tier, TimestampTz enqueued_at, TimestampTz now);

/* A request. m == NULL (the registry had no table): SIFT_ENQ_UNAVAILABLE, nothing else happens */
extern SiftMaintEnq sift_maint_enqueue(SiftMaintShmem *m, const SiftMaintHost *host, const SiftMaintReq *req);
/* The best runnable request of db (InvalidOid: any), as a claim of the caller; false when there is none. allow_cold: P2 and P3 may start */
extern bool sift_maint_claim(SiftMaintShmem *m, const SiftMaintHost *host, Oid db, bool allow_cold, SiftMaintClaim *out);
extern bool sift_maint_complete(SiftMaintShmem *m, const SiftMaintHost *host, const SiftMaintClaim *claim, SiftMaintStep result);
extern void sift_maint_progress(SiftMaintShmem *m, const SiftMaintClaim *claim, SiftMaintPhase phase, uint64 done, uint64 total, uint64 bytes);
/* Claim, run and complete until there is no runnable request, max_steps steps ran or the pacer asks the worker to leave (*yielded): the loop of a worker and of sift.maintain() */
extern int	sift_maint_run(SiftMaintShmem *m, const SiftMaintHost *host, Oid db, int max_steps, SiftMaintStepFn step, void *arg, bool *yielded);
extern bool sift_maint_has_work(SiftMaintShmem *m, const SiftMaintHost *host, Oid db, bool allow_cold);

/* The claims of a pid that is gone, the workers that never started or died; returns the claims given back */
extern int	sift_maint_reap(SiftMaintShmem *m, const SiftMaintHost *host);
/* The claims of a pid that exits: requeued, as a failed step when failed (it backs off), as one that never ran when not */
extern int	sift_maint_release_pid(SiftMaintShmem *m, const SiftMaintHost *host, int pid, bool failed);

/* The cap on workers of sift: Min(configured, max_worker_processes - Max(2, max_worker_processes / 4) - the bgworkers that are not ours); the reserve is left to parallel queries */
extern int	sift_maint_effective_max(int configured, int max_worker_processes, int bgworkers_other);
extern SiftMaintMode sift_maint_mode(bool async_maintenance, bool preloaded, bool table_ok, int effective_max);
extern int	sift_maint_worker_reserve(SiftMaintShmem *m, const SiftMaintHost *host, Oid db, int effective_max);
/* False when the slot was given up (its lease lapsed) and taken by another: the worker must exit */
extern bool sift_maint_worker_started(SiftMaintShmem *m, int worker, int pid, int procno);
extern void sift_maint_worker_exit(SiftMaintShmem *m, int worker, int pid);
extern SiftMaintKick sift_maint_kick(SiftMaintShmem *m, const SiftMaintHost *host, int effective_max);
/* Enqueue, then start or wake what drains it as the mode says */
extern SiftMaintEnq sift_maint_request(SiftMaintShmem *m, const SiftMaintHost *host, SiftMaintMode mode, int effective_max, const SiftMaintReq *req);

extern void sift_maint_heat_note(SiftMaintShmem *m, Oid rel);
extern uint32 sift_maint_heat(SiftMaintShmem *m, Oid rel);
extern void sift_maint_heat_decay(SiftMaintShmem *m);
extern void sift_maint_fg_note(SiftMaintShmem *m, bool write);

/* A coherent copy of the table without the lock of the writers (a seqlock, the lock after SIFT_MAINT_SNAP_TRIES misses): the rows sorted by priority, the running ones first */
#define SIFT_MAINT_SNAP_TRIES	8
extern int	sift_maint_snapshot(SiftMaintShmem *m, const SiftMaintHost *host, SiftMaintRow rows[SIFT_MAINT_SLOTS], SiftMaintStatus *status);

#ifndef SIFT_SHM_STANDALONE
#include "storage/lockdefs.h"
#include "utils/relcache.h"

extern const SiftMaintHost sift_maint_server_host;
/*
 * How a step of a worker opens its relation: the table locked first, as a statement does (DROP TABLE locks the table and then its indexes), then the relation itself;
 * NULL when it is gone (the step answers SIFT_MSTEP_GONE). The points sift-maint-f2-before-table-lock and sift-maint-f2-after-table-lock stop a worker here in test/t/maint_lock_order.sh
 */
extern Relation sift_maint_open(Oid relid, LOCKMODE lock);
/* The table of this backend: the dynamic shared memory registry makes it for the first one; NULL where the registry has no room */
extern SiftMaintShmem *sift_maint_table(void);
/* sift.async_maintenance, sift.max_maint_workers and sift.maintenance_naptime as the GUC hooks of worker.c give them */
extern void sift_maint_configure(bool async_maintenance, int max_workers, int naptime_ms);
extern SiftMaintEnq sift_maint_enqueue_here(const SiftMaintReq *req);

/* The workers of the queue that run or are about to (the workers of core/worker.h count them against sift.max_maint_workers too) */
extern int sift_maint_active_workers(void);
extern PGDLLEXPORT void sift_maint_sched_worker_main(Datum main_arg);
extern PGDLLEXPORT void sift_maint_sched_launcher_main(Datum main_arg);
/* Registers the launcher: _PG_init under shared_preload_libraries only */
extern void sift_maint_launcher_register(void);
/* The one function that runs a step of a kind inside a transaction of the worker, set by the area that owns the steps */
extern void sift_maint_set_stepper(SiftMaintStepFn step);

/* The assign hooks of sift.async_maintenance, sift.max_maint_workers and sift.maintenance_naptime */
extern void sift_maint_assign_async(bool newval, void *extra);
extern void sift_maint_assign_max_workers(int newval, void *extra);
extern void sift_maint_assign_naptime(int newval, void *extra);

/* The step of every claim the workers of the queue run (core/maint_step.c) */
extern SiftMaintStep sift_maint_step(const SiftMaintClaim *claim, void *arg);
#endif

#endif							/* SIFT_MAINT_SCHED_H */
