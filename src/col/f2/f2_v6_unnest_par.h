/*
 * col/f2/f2_v6_unnest_par.h - the aggregate scan of a LATERAL unnest (agg/v6/v6_unnest_run.h) on the compute threads of the backend (core/par/pool.h).
 *
 * The leader reads the batches of the index and says which rows of each take part (the serial run's source below), and feeds them to a ring of slots that the threads expand and fold into
 * agg/v6/par/group.h (agg/v6/par/unnest.h). A slot a thread cannot fold exactly is folded by the leader, the same way the serial run does. When the groups are more than the threads fold (the table of a
 * thread outgrows SIFT_PAR_GROUP_LOW, a stream or an arena fills), what the threads folded is dropped and the serial run reads the batches again that they had: the answer is the serial run's.
 */
#ifndef SIFT_COL_F2_V6_UNNEST_PAR_H
#define SIFT_COL_F2_V6_UNNEST_PAR_H

#include "postgres.h"

#include "agg/v6/par/unnest.h"
#include "col/f2/f2_sink_ext.h"

typedef struct SiftV6UnnestSrc
{
	/* The next batch of the index with at least one row to expand, and the rows of it that take part (the rows the index cannot hold are folded by next, from the heap); false after the last */
	bool		(*next) (void *arg, const SiftF2ExtBatch **b, uint64 *mask);
	/* The pairs of the rows of the batch, as the serial run folds them */
	void		(*fold) (void *arg, const SiftF2ExtBatch *b, const uint64 *mask);
	/* The scan of the index starts over; next gives the batches again, and not their rows of the heap */
	void		(*redo) (void *arg);
	SiftUnPairs *pairs;			/* the pairs into the pipe of the statement */
	const SiftV6Pipe *pipe;		/* the pipe of the statement: its fact slots are the columns of the batch, then the element */
	bool		left;
	void	   *arg;
} SiftV6UnnestSrc;

/* false: the statement is not one the threads take (nothing was read). true: every row is in the groups of g */
extern bool sift_v6_unnest_par(const SiftV6Spec *spec, SiftV6Group *g, Relation index, const SiftV6UnnestSrc *src);

#endif							/* SIFT_COL_F2_V6_UNNEST_PAR_H */
