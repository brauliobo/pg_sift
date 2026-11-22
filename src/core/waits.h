/*
 * core/waits.h - the named wait events of pg_sift: what pg_stat_activity.wait_event says of a backend that waits in this library (Extension, SiftMaintenanceNap, ...).
 */
#ifndef SIFT_WAITS_H
#define SIFT_WAITS_H

#include "postgres.h"

typedef enum SiftWait
{
	SIFT_WAIT_NAP,				/* SiftMaintenanceNap: a maintenance worker between two looks at its index */
	SIFT_WAIT_ARENA,			/* SiftArenaWait: a writer waits for the rebuild of a slot of the write buffer references */
	SIFT_WAIT_PAR_LEADER,		/* SiftParallelLeader: the leader of a parallel aggregate waits for the rows of its workers */
	SIFT_WAIT_LOAD_RING,		/* SiftLoadRing: a bulk load of a sift_table waits for a slot of its ring */
	SIFT_WAIT_BUILD_GATE,		/* SiftBuildGate: a worker of a parallel index build waits for the leader */
	SIFT_WAIT_MAINT_PACE,		/* SiftMaintenancePace: a maintenance step sleeps in the pacer (foreground load, sift.maintenance_cost_delay, sift.maintenance_wal_rate) */
	SIFT_WAIT_TAIL_LOCK,		/* SiftTailShrinkLock: the cut of the free end of a file waits between two tries for the lock of its relation */
	SIFT_WAIT_COUNT
} SiftWait;

/* The wait event of w, registered with the server (WaitEventExtensionNew) by the first backend that waits in it: the same id for the whole cluster */
extern uint32 sift_wait_event(SiftWait w);

/* Sleeps ms milliseconds on the process latch under the wait event w, and looks for an interrupt: a cancel, SIGTERM or the end of the postmaster wakes the sleep */
extern void sift_wait(SiftWait w, long ms);

#endif							/* SIFT_WAITS_H */
