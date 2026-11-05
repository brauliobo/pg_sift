/*
 * agg/par/int.h - the parallel scan, inside agg/par/ (the entry points are in agg/par_int.h)
 */
#ifndef SIFT_AGG_PAR_DIR_INT_H
#define SIFT_AGG_PAR_DIR_INT_H

#include "agg/agg_int.h"

/* The superblocks the zones and summaries of the source leave a unit in, for the scan: a range of none costs a few words, not a begin (colsrc.h kept) */
typedef struct SkipMap
{
	bool		ok;
	uint32		nsb;
	uint64	   *kept;
	uint64		rows;			/* the rows of the units kept */
} SkipMap;

/*
 * A scan whose plan asks for workers runs them under a parallel context of its own (not under a Gather: the partial tables are
 * not tuples). The leader and the workers take items from the counter in the shared memory; a worker sends its tables to the
 * leader through a shm_mq, as one blob each, and the leader merges them.
 */
#define KEY_SHARED	UINT64CONST(0x53494654A001)
#define KEY_SPEC	UINT64CONST(0x53494654A002)
#define KEY_PARAMS	UINT64CONST(0x53494654A003)
#define KEY_SNAP	UINT64CONST(0x53494654A004)
#define KEY_MQ		UINT64CONST(0x53494654A005)
#define KEY_MAP		UINT64CONST(0x53494654B000)	/* + the combo: the table of its join (agg/lookshare.c) */
#define MQ_SIZE		(1 << 20)
/* the lookup tables the workers share, together: a larger set is read by the leader alone */
#define SHARE_MAX	((Size) 1 << 30)
#define DONE_MAGIC	0x53414732
#define HITS_MAGIC	0x53414733
#define SPINE_MAGIC	0x53414734
#define KEY_SPINE	UINT64CONST(0x53494654A006)
#define KEY_DICTSHM	UINT64CONST(0x53494654A007)	/* present when the leader has the table of shared dictionaries (col/dictshm.c): the workers join it */
#define KEY_CHILD	UINT64CONST(0x53494654C000)	/* + the join: the statement whose plan is its correlated child */
#define KEY_SUB	UINT64CONST(0x53494654E000)	/* the statement whose sub-plans the expressions of the spec start */
#define KEY_INITVALS	UINT64CONST(0x53494654A008)	/* the values of the InitPlan outputs the scan reads as constants (init_values) */
#define KEY_FMAP	UINT64CONST(0x53494654F000)	/* + the combo: the table of a hybrid first-row lookup (agg/first.c) */
#define KEY_MEMO	UINT64CONST(0x53494654D000)	/* + the join: the rows of its child by parameter values (agg/memo.c) */

/* the rows of a preserved side that a worker's tuples matched: nrow flags of the table of combo ci */
typedef struct WorkerHits
{
	uint32		magic;
	uint32		ci;
	uint32		nrow;
} WorkerHits;

typedef struct WorkerDone
{
	uint32		magic;
	uint32		pad;
	int64		n_units;
	int64		n_pre_units;
	int64		n_super;
	int64		n_rollup_miss;
	int64		n_exc_units;
	int64		n_exc_scan;
	int64		n_dead_units;
	int64		n_dead_scan;
	int64		n_rollup_queued;
	int64		n_rollup_cost;
	int64		n_rows;
	int64		n_tuples;
	int64		n_dense;
	int64		n_combo_trim;
	int64		n_matches;
	int64		n_skipped;
	int64		n_runs;
	int64		n_lazy;
	SiftSrcCounters cc;
} WorkerDone;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* par/ */
SkipMap *skip_map(AggExec *ex);
void lane_keep(AggExec *ex, uint32 lane);
void par_poll(AggExec *ex);
void par_forget_memo(AggExec *ex);

#pragma GCC visibility pop

#endif
