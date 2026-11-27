/*
 * core/maint/pacer.h - SiftMaintPacer: how the background maintenance of sift gives way to the foreground, with no setting of its own.
 *
 * Every worker step has a tier: P0 (latency: the promotion of a write buffer, the quarantine mark), P1 (hot analytics: the merges, the rollup refresh), P2 (structural and cold: the
 * dead-row and space pass of a table) and P3 (space: the cut of the free end of a file, the sweep of orphan blocks). The load of the foreground decides how long a step sleeps for the pages it
 * writes and whether a P2 or P3 step starts at all:
 *
 *   L = running client backends / max(2, max_parallel_workers)       S = fast average of the duration of a scan / its slow average (at least 1 ms)
 *
 *   FULL       L < 0.25, S < 1.5, no lock yielded    P0 0 ms      P1, P2, P3 sift.maintenance_cost_delay
 *   MODERATE   L < 0.75, S < 2.5                     P0 0 ms      P1 2..15 ms, P2 and P3 10..30 ms (never less than sift.maintenance_cost_delay)
 *   THROTTLED  otherwise                             P0 0..2 ms   P1 20..50 ms, P2 and P3 do not start: they ask again after 5..30 s (mode DEFERRED while one waits)
 *
 * (a delay is per 16 pages, 128 kB, as sift.maintenance_cost_delay always was; the point in the range is the share of the way to the next regime). The telemetry is a few atomics in the
 * shared memory of the directory (col/dsmreg.h): the backends that finish a scan feed the averages (one scan of 64, one operation of 16 for the engines of sift_pacer_fg_sample_begin), and only a maintenance worker looks at the backends, once per 250 ms. A foreground that
 * stays silent for a second is over: the averages fall back to the usual, the backoff of a heavy regime ends with the load, and a read of the pacer after the workers have left looks again.
 * A request for a worker that finds none free marks its tier; the workers of a lower tier leave after the step they are in, so a P0 request is served before a P1 one, and so on.
 * The reads and writes of a worker go through a BAS_VACUUM ring, so a merge never takes the buffers of the queries. SIGTERM, a cancel and the end of the postmaster wake a sleep at once.
 */
#ifndef SIFT_MAINT_PACER_H
#define SIFT_MAINT_PACER_H

#include "postgres.h"

#include "datatype/timestamp.h"
#include "storage/bufmgr.h"
#include "utils/timestamp.h"

#include "core/store/store.h"

typedef enum SiftPacerTier
{
	SIFT_TIER_P0,
	SIFT_TIER_P1,
	SIFT_TIER_P2,
	SIFT_TIER_P3,
	SIFT_TIERS
} SiftPacerTier;

typedef enum SiftPacerMode
{
	SIFT_PACE_FULL,
	SIFT_PACE_MODERATE,
	SIFT_PACE_THROTTLED,
	SIFT_PACE_DEFERRED
} SiftPacerMode;

/* What the law is told */
typedef struct SiftPaceLoad
{
	uint32		active_oltp;	/* client backends that run a statement now */
	uint32		parallel_slots;	/* max(max_parallel_workers, 2): the share of them the backends take is the moderate regime */
	uint32		cpu_slots;		/* max(max_parallel_workers, the online CPUs, 2): the share of them is the heavy regime, which holds P2 and P3 back */
	uint64		lat_ewma_us;	/* fast average of the duration of a scan */
	uint64		lat_base_us;	/* slow average */
	uint32		lock_yields;	/* locks a step gave way for since the last look */
} SiftPaceLoad;

typedef struct SiftPaceDecision
{
	SiftPacerMode mode;
	int			share_pm;		/* how far into its regime the load is, 0..1000 */
} SiftPaceDecision;

extern SiftPaceDecision sift_pacer_decide(const SiftPaceLoad *load);

/* The sleep, in ms per 16 pages, of a step of tier under d; at least base_delay_ms (sift.maintenance_cost_delay) outside P0; -1: a step of this tier does not start under d */
extern int	sift_pacer_delay_ms(const SiftPaceDecision *d, SiftPacerTier tier, int base_delay_ms);

/* A scan of the foreground took usecs: one in 64 feeds the averages */
extern void sift_pacer_note_foreground(int64 usecs);

/*
 * The foreground of the engines feeds the averages with one operation in 16 of each backend, the first one always: a pooled session of a few statements reports too. The clock is read only for
 * the operations that count: if (sift_pacer_fg_sample_begin(&t0)) { ...the operation...; sift_pacer_record_foreground(GetCurrentTimestamp() - t0); }
 */
extern uint32 sift_pacer_fg_ops;

static inline bool
sift_pacer_fg_sample_begin(TimestampTz *t0)
{
	if ((++sift_pacer_fg_ops & 15U) != 0)
		return false;
	*t0 = GetCurrentTimestamp();
	return true;
}

/* An operation of the foreground took usecs: feeds both averages and marks the foreground as seen now */
extern void sift_pacer_record_foreground(int64 usecs);

/* A step of a worker could not take a lock that the foreground holds and gave way */
extern void sift_pacer_note_lock_yield(void);

/* A request for a worker of tier found none free at now */
extern void sift_pacer_note_starved(SiftPacerTier tier, TimestampTz now);

/* Does a worker of tier leave after its step at now, for a request of a higher tier that found none free (within the last sift.maintenance_naptime)? */
extern bool sift_pacer_should_yield(SiftPacerTier tier, TimestampTz now);

/* Must a step of a worker of this tier wait for a quieter moment? True for P2 and P3 under THROTTLED, and then they ask again after the backoff */
extern bool sift_pacer_defer(SiftPacerTier tier);

/* A worker paid for 16 pages of I/O: sleeps what the load asks, under the wait event SiftMaintenancePace */
extern void sift_pacer_pause(SiftPacerTier tier);

/* The writes of the steps of this worker are paced as its tier says and keep to sift.maintenance_wal_rate; a process that is no paced worker is left alone */
extern void sift_wt_pace_worker(SiftWriteTarget *wt);

/* The client backends that run a statement, in the database db or in all of them (InvalidOid) */
extern int	sift_pacer_running_backends(Oid db);

/* What a maintenance worker last saw (sift_internal.maint_pacer): the running backends it counted, the mode it decided and the delay of a P1 step under it; FULL, 0 and 0 before the first look */
typedef struct SiftPacerSnapshot
{
	uint32		active_oltp;
	SiftPacerMode mode;
	int			delay_ms;
} SiftPacerSnapshot;

extern void sift_pacer_snapshot(SiftPacerSnapshot *out);

/* The ring a paced worker reads and writes through, NULL for any other process */
extern PGDLLIMPORT BufferAccessStrategy sift_maint_ring;
extern void sift_maint_ring_open(void);

#ifdef SIFT_TESTING
/* The telemetry as the tests read and reset it */
extern void sift_pacer_test_state(uint64 *ewma_us, uint64 *base_us);
extern void sift_pacer_test_note_foreground(int64 usecs);
extern void sift_pacer_test_reset(void);
#endif

#endif							/* SIFT_MAINT_PACER_H */
