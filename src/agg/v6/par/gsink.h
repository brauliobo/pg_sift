/*
 * agg/v6/par/gsink.h - the groups that the compute threads deliver (agg/v6/par/group.h SiftParGroupHooks.emit) as a stream of records of each thread (core/par/radix.h), which the leader reads
 * to its fold when the run is over. A stream that is full says so: the groups of the threads are then not the whole answer.
 */
#ifndef SIFT_AGG_V6_PAR_GSINK_H
#define SIFT_AGG_V6_PAR_GSINK_H

#include "postgres.h"

#include "agg/v6/par/group.h"

typedef struct SiftV6GSink SiftV6GSink;

typedef void (*SiftV6GSinkFold) (void *arg, const SiftParGroupRow *row);

/* The streams of nthreads threads of `bytes` each, for the groups of gs */
extern SiftV6GSink *sift_v6_gsink_new(MemoryContext cx, const SiftParGroupSpec *gs, int nthreads, size_t bytes);
/* The bytes of the record of a group of gs: of a text key of up to SIFT_V6_GSINK_STR bytes */
#define SIFT_V6_GSINK_STR	32
extern uint64 sift_v6_gsink_record_bytes(const SiftParGroupSpec *gs);
/* Thread `worker`: the group goes to its stream (the hook emit of the groups) */
extern void sift_v6_gsink_emit(SiftV6GSink *s, int worker, const SiftParGroupRow *row);
extern bool sift_v6_gsink_full(const SiftV6GSink *s);
/* The leader: every group of every stream to fold */
extern void sift_v6_gsink_drain(SiftV6GSink *s, SiftV6GSinkFold fold, void *arg);

#endif							/* SIFT_AGG_V6_PAR_GSINK_H */
