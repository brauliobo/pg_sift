/*
 * agg/par_int.h - the parallel scan (agg/par/)
 */
#ifndef SIFT_AGG_PAR_INT_H
#define SIFT_AGG_PAR_INT_H

#include "agg/agg_fwd_int.h"

typedef struct ScanShared
{
	pg_atomic_uint64 topn_bound;	/* a top-N: the first-key rank of the n-th row of any participant (agg/topn.h) */
	pg_atomic_uint32 next;		/* the next item to take */
	pg_atomic_flag kept_counted;	/* a participant has counted the units the kept map prunes (all of them build one, and each would count them again) */
	uint32		nitems;
	uint32		gpart_n;		/* the participants split the groups by a key (SiftAggSpec.gpart), this many shares; 0: they take items of rows */
	uint32		nsb;
	uint32		morsel;			/* superblocks of an item (the scans that keep their state in the leader: all of them) */
	bool		ordered;
	bool		topn_reverse;	/* a top-N: the items go from the last superblock */
	bool		topn_walk;		/* ... or, in the order after partoff, one superblock each, by the best row their zone can hold */
	uint32		topn_item;		/* ... superblocks of an item otherwise */
	uint32		partoff[FLEXIBLE_ARRAY_MEMBER];	/* a partitioned table: the first superblock of each partition and the total (sift_aggpart_offsets); the walk's order follows */
} ScanShared;

typedef struct AggPar
{
	ParallelContext *pcxt;
	int			nlaunched;
	int			ndone;
	shm_mq_handle **mqh;
	bool	   *done;
	SiftSrcCounters cc;			/* of the workers, added */
	int64		matches;		/* BM25 matches the workers found, added */
} AggPar;

extern PGDLLIMPORT int sift_test_split;
extern PGDLLIMPORT bool sift_test_par_leader_wait;
extern PGDLLIMPORT int sift_test_item_gate;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* par/ */
Size shared_size(const SiftAggSpec *s, uint32 norder);
uint32 *shared_order(ScanShared *sh, const SiftAggSpec *s);
void shared_init(ScanShared *sh, AggExec *ex);
void scan_units(AggExec *ex, uint32 lo, uint32 hi);
void empty_set_groups(AggExec *ex);
void final_checks(AggExec *g, int64 ntuples);
void lane_fold(AggExec *ex);
void lanes_sum(AggExec *ex);
void scan_items(AggExec *ex);
bool take_item(AggExec *ex, uint32 *item);
void par_launch(AggExec *ex);
void par_finish(AggExec *ex);
bool split_shape_ok(const SiftAggSpec *s);
bool split_pass_key_ok(const SiftAggSpec *s, int j, const SiftSrcLeaf *lf);
bool split_key_ok(const SiftAggSpec *s, int j, const SiftSrcLeaf *lf);
bool split_run_ok(const AggExec *ex);
void split_begin(AggExec *ex, int key, uint32 n, uint32 mine);
void split_rows(AggExec *ex, uint64 *rmask, int nrow);

#pragma GCC visibility pop

#endif
