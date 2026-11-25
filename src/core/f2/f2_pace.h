/*
 * core/f2/f2_pace.h - what the maintenance of format major 2 asks of the system it shares (spec 49 7): the pages of its writes paced by sift.maintenance_cost_delay and
 * sift.maintenance_wal_rate, and the horizon it strips transaction ids at, kept sift.standby_conflicts-safe seconds behind the primary's so that a standby's query is not cancelled by it.
 *
 * The horizon: a primary that strips an id the moment no snapshot of its own needs it sends a standby's snapshot a recovery conflict, and the standby (without hot_standby_feedback)
 * cancels its query. A ring of 8 samples of the primary's horizon, one every 15 s, gives the horizon as it was SIFT_STANDBY_SAFE_LAG_SECS ago: parts older than that seal, fold and
 * recycle at full speed, and the recent ones keep their ids. A sample is two words, written with the ring's version odd, and read as a copy that a changed version discards.
 */
#ifndef SIFT_CORE_F2_PACE_H
#define SIFT_CORE_F2_PACE_H

#include "postgres.h"

#include "access/transam.h"
#include "datatype/timestamp.h"

#define SIFT_HORIZON_RING_SLOTS		8
#define SIFT_HORIZON_SAMPLE_SECS	15
#define SIFT_STANDBY_SAFE_LAG_SECS	60
#define SIFT_F2_PACE_UNIT_PAGES		16	/* the pages of one sleep of the pacer (128 kB) */
#define SIFT_F2_PACE_WINDOW_SECS	10	/* the WAL rate is kept over windows of this length: an idle worker has no credit to spend in a burst */

typedef struct SiftHorizonSample
{
	TimestampTz ts;				/* 0: never written */
	uint64		horizon;		/* a full transaction id: GetOldestNonRemovableTransactionId as of ts */
} SiftHorizonSample;

typedef struct SiftStandbyHorizon
{
	uint32		version;		/* odd while a writer changes the slots */
	uint32		head;			/* samples ever recorded */
	SiftHorizonSample slot[SIFT_HORIZON_RING_SLOTS];
} SiftStandbyHorizon;

StaticAssertDecl(sizeof(SiftStandbyHorizon) == 8 + SIFT_HORIZON_RING_SLOTS * 16, "the ring is its 8 samples and two words");

extern void sift_horizon_ring_init(SiftStandbyHorizon *r);

/* Records raw as the horizon at now when the last sample is SIFT_HORIZON_SAMPLE_SECS old or there is none; false when it was too soon or another process was writing */
extern bool sift_horizon_ring_record(SiftStandbyHorizon *r, TimestampTz now, FullTransactionId raw);

/*
 * The horizon to strip at: the sample of the newest age of at least SIFT_STANDBY_SAFE_LAG_SECS (the oldest one while the ring has not as much history, raw while it has none), never
 * past raw.
 */
extern FullTransactionId sift_horizon_ring_lagged(const SiftStandbyHorizon *r, TimestampTz now, FullTransactionId raw);

/* Whether the oldest unfrozen id of the table is more than half of autovacuum_freeze_max_age behind next: the ids are then stripped at once */
extern bool sift_horizon_wraparound_urgent(TransactionId next, TransactionId oldest, int freeze_max_age);

/* The units of SIFT_F2_PACE_UNIT_PAGES that npages more pages complete, acc being the pages charged since the last of them */
extern uint32 sift_f2_pace_units(uint64 *acc, uint64 npages);

/* The ms a writer that wrote wal_bytes in elapsed_ms must wait to keep to kbps kilobytes per second; 0 when it is within, or kbps is 0 (no rate) */
extern int64 sift_f2_wal_deficit_ms(uint64 wal_bytes, int kbps, int64 elapsed_ms);

#ifndef SIFT_S49_STANDALONE
#include "utils/rel.h"

/* The ring the processes of the server share, once the maintenance table is made; a process that has none samples into a ring of its own */
extern void sift_standby_horizon_attach(SiftStandbyHorizon *shared);

/*
 * The horizon of full transaction ids to strip, settle and recycle at for the index: its heap's oldest non-removable id, held back by the lag above when wal_level has a standby to
 * protect (XLogStandbyInfoActive) and sift.standby_conflicts is on, and not held back for urgent_freeze (an explicit sift.col_maintain, VACUUM FREEZE) or a table near wraparound.
 */
extern FullTransactionId sift_standby_safe_horizon(Relation index, bool urgent_freeze);

/*
 * Pages of a write of a paced maintenance worker: its pacer sleeps for each SIFT_F2_PACE_UNIT_PAGES and the WAL it wrote is kept to sift.maintenance_wal_rate; any other process is left alone.
 * The step of a worker of the queue holds locks of partitions that the writers of the clients wait for: what it owes the pacer is paid by sift_f2_pace_pay, between its steps and groups.
 */
extern void sift_f2_pace_pages(uint64 npages);
extern void sift_f2_pace_pay(void);
#endif

#endif							/* SIFT_CORE_F2_PACE_H */
