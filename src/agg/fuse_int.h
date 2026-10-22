/*
 * agg/fuse_int.h - arms that share one pass (agg/fuse.c)
 */
#ifndef SIFT_AGG_FUSE_INT_H
#define SIFT_AGG_FUSE_INT_H

#include "agg/agg_fwd_int.h"

typedef struct FuseSt
{
	AggExec    *F;				/* the executor of the fused spec */
	int		  **keymap;			/* per set: the key of the fused spec that is each key of the node */
	int		  **aggmap;			/* per set: the aggregate of the fused spec that is each aggregate of the node */
	int			refs;
} FuseSt;

/* hidden: the build has -fno-plt, which turns a call to a function of another file into a call through the GOT */
#pragma GCC visibility push(hidden)

/* fuse.c */
void fuse_register(AggExec *ex);
void fuse_unregister(AggExec *ex);
bool fuse_start(AggExec *ex);
TupleTableSlot *fuse_next(AggExec *ex, TupleTableSlot *slot);

#pragma GCC visibility pop

#endif
