/*
 * agg/par/star.h - the batches of a star or snowflake join through its dimensions, in pure C, for the compute threads of the backend (agg/par/pinrun.h).
 *
 * The leader reads every dimension once into the flat tables of agg/star/star.h (a hash of the keys, the columns of the payload) and makes a pipe (agg/pipe.h) of its own for each thread over
 * the same, read-only tables: no thread builds a table, copies one, or writes to one. A thread takes the fact columns of a granule, probes the dimensions batch by batch of SIFT_VEC_LEN rows through its pipe
 * and gathers the payload columns the keys and arguments read; the batches that leave the last dimension are the words that agg/par/group.h folds: a key or a sum of 64-bit integers (a key is the
 * value with its sign bit flipped, all ones is the NULL key), or one text key (a payload of a dimension, which a NULL join leaves NULL).
 *
 * What the words cannot say exactly is the scan's: a NULL in a counted or summed argument (the payload of a left join that found nothing), a key whose word is the NULL one, and more rows than the granule
 * has (a dimension that holds a key more than once expands its batch): the scan folds that granule, into the same groups.
 */
#ifndef SIFT_AGG_V6_PAR_STAR_H
#define SIFT_AGG_V6_PAR_STAR_H

#include "postgres.h"

#include "agg/par/cols.h"
#include "agg/par/group.h"
#include "agg/par/wordsink.h"
#include "agg/engine.h"
#include "agg/pipe.h"

/* Whether the statement is one the threads fold: its keys, aggregates and the columns of its dimensions are integers or (one key) text; a spec the planner made for the scan of the fact columns */
extern bool sift_par_star_shape(const SiftV6Spec *spec, SiftParWordShape *sh);

typedef struct SiftParStar SiftParStar;

/* The pipes and buffers of nthreads threads over the dimensions of pipe (the pipe of the leader, with its dimensions open): everything a thread takes, made now, by the leader */
extern SiftParStar *sift_par_star_new(MemoryContext cx, const SiftParWordShape *sh, const SiftV6Pipe *pipe, int nthreads);

/* Thread `worker`: the words of the n rows of a granule (fact[i] the column of slot i of the pipe, active the rows of the granule that pass), valid until the next call of the thread */
extern bool sift_par_star_batch(SiftParStar *st, int worker, const SiftParCol *fact, const uint64 *active, int n, SiftErr *err, SiftParGroupBatch *out);

#endif							/* SIFT_AGG_V6_PAR_STAR_H */
