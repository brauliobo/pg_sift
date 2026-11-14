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
	SIFT_WAIT_COUNT
} SiftWait;

/* The wait event of w, registered with the server (WaitEventExtensionNew) by the first backend that waits in it: the same id for the whole cluster */
extern uint32 sift_wait_event(SiftWait w);

#endif							/* SIFT_WAITS_H */
