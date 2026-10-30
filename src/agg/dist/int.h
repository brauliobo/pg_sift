/*
 * agg/dist/int.h - what the files of agg/dist/ share
 * The walk type, the pair macro, the small helpers every part inlines, and the functions
 * the adding, the conversion, the finish and the stream call in one another.
 */
#ifndef SIFT_AGG_DIST_DIR_INT_H
#define SIFT_AGG_DIST_DIR_INT_H

#include "agg/agg_int.h"

#define DSET_PAIR(g, v)		(UINT64CONST(1) << 63 | (uint64) (g) << 32 | (uint32) (v))
typedef void (*DistFn) (void *arg, uint32 g, uint32 v);
extern uint64 dset_insert_raw(DSet *s, uint64 pair);
extern void dist_walk(const Dist *d, DistFn fn, void *arg);
extern void dist_convert(AggExec *ex, Dist *d, DistMode to);
extern uint64 * dist_chunk(AggExec *ex, Dist *d);
extern bool dist_compact(Dist *d);
extern bool dist_compact_hash(AggExec *ex, Dist *d);

static inline int64
dist_x(const Dist *d, uint32 v)
{
	return d->sgn ? (int64) (int32) v : (int64) v;
}

#endif							/* SIFT_AGG_DIST_DIR_INT_H */
