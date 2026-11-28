/*
 * core/maint/vacuum.h - what sift.vacuum_rewrite_limit means to a VACUUM, in one place (0 is unlimited in every format), and when the cleanup of an index runs.
 */
#ifndef SIFT_MAINT_VACUUM_H
#define SIFT_MAINT_VACUUM_H

#include "postgres.h"

#include "access/genam.h"

/* The limit in bytes whoever asks, UINT64_MAX for 0: the size of a step that holds a lock (the rounds of the space step, the partitions of a rewrite), which an explicit limit bounds in any process */
extern uint64 sift_vacuum_rewrite_cap(void);

/* The bytes of rewriting a VACUUM pass may start: only an autovacuum worker that is not freezing is held to the limit; VACUUM FREEZE, anti-wraparound and a VACUUM someone runs are never cut short */
extern uint64 sift_vacuum_rewrite_budget(bool aggressive);

/* Says (DEBUG1; INFO for VACUUM VERBOSE) that the limit stopped a pass with work left, so that the work that waits for the next pass leaves a trace */
extern void sift_vacuum_say_deferred(int message_level, uint64 written);

/*
 * The cleanup of a sift index (amvacuumcleanup): run now, or, when VACUUM calls it in parallel mode (core/maint/vacuum_owed.c), at the pre-commit of the transaction of its table, where the
 * steps that lock the index exclusively can run; the stats VACUUM gets then are estimated, and the cleanup updates the index's pg_class row itself
 */
typedef IndexBulkDeleteResult *(*SiftVacuumCleanup) (IndexVacuumInfo *info, IndexBulkDeleteResult *stats);
extern IndexBulkDeleteResult *sift_vacuum_cleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, SiftVacuumCleanup cleanup);

#endif
