/*
 * agg/v6/par/unnest.h - the pairs of (row, element) of a LATERAL unnest of an array column of the index, made and folded by the compute threads of the backend (core/par/pool.h), in pure C.
 *
 * The leader reads the batches of the index (col/f2/f2_sink_ext.h), decides which rows take part (the deletion vector, the WHERE clause of the table, the visibility of the heap row) and copies a batch of
 * up to SIFT_VEC_LEN rows into the slot of a ring: the columns of the rows, the lengths and elements of their arrays, the rows whose array the index cannot hold left out (the leader reads those from the
 * heap, as the serial run does). A thread expands the slot into pairs (agg/v6/v6_unnest.h), gathers the columns beside the element and runs them through a pipe of its own (agg/v6/v6_unnest_pairs.h)
 * whose end is the words of agg/v6/par/wordsink.h; it knows nothing of the server. A slot a thread cannot fold exactly (a NULL in a counted or summed column, more pairs than its sink holds) is the leader's: it
 * expands the same slot into the pipe of the statement.
 */
#ifndef SIFT_AGG_V6_PAR_UNNEST_H
#define SIFT_AGG_V6_PAR_UNNEST_H

#include "postgres.h"

#include "agg/v6/par/wordsink.h"
#include "agg/v6/v6_unnest_pairs.h"
#include "agg/vexpr.h"

#define SIFT_UN_COLS_MAX	SIFT_VEXPR_MAX_REGS
#define SIFT_UN_PAIRS_MAX	(64 * SIFT_VEC_LEN)

/* One batch of rows with their arrays, immutable from the leader's feed until the release of its slot */
typedef struct SiftUnSlot
{
	uint32		n;				/* the rows */
	uint32		nelems;			/* the elements of their arrays */
	int			ncols;
	SiftVec		col[SIFT_UN_COLS_MAX];
	SiftVec		elem;
	uint64		mask[SIFT_VEC_WORDS];	/* the rows that take part, those the index cannot hold left out */
	uint64		doc_valid[SIFT_VEC_WORDS];
	uint32		lengths[SIFT_VEC_LEN];
	uint32		offsets[SIFT_VEC_LEN + 1];
	uint32		block_off[SIFT_VEC_LEN / 64 + 1];
	/* what the vectors point to: the leader makes it, and grows it for a batch that needs more */
	void	   *data[SIFT_UN_COLS_MAX];
	uint64	   *valid[SIFT_UN_COLS_MAX];
	void	   *edata;
	uint64	   *evalid;
	uint32		ecap;
	char	   *str;
	size_t		strcap;
} SiftUnSlot;

/* The pairs of the slot into p, until halt (when given) refuses a batch: the rows of the slot in the order of the serial run */
extern void sift_un_slot_pairs(SiftUnPairs *p, const SiftUnSlot *sl, bool left, const SiftParWords *halt);

/* Whether the statement is one the threads fold: keys and arguments that are plain columns of the table or the element, as agg/v6/par/wordsink.h says */
extern bool sift_par_unnest_shape(const SiftV6Spec *spec, SiftParWordShape *sh);

typedef struct SiftParUnnest SiftParUnnest;

/* The sink, pairs and pipe of each of nthreads threads, over the pipe of the leader (the types of whose fact slots are the columns of the slot, then the element) */
extern SiftParUnnest *sift_par_unnest_new(MemoryContext cx, const SiftParWordShape *sh, const SiftV6Pipe *pipe, bool left, int nthreads);
/* Thread `worker`: the words of the pairs of the slot, valid until its next call; SIFT_ERR_SERIAL, SIFT_ERR_ARENA_FULL leave the slot to the leader */
extern bool sift_par_unnest_batch(SiftParUnnest *un, int worker, const SiftUnSlot *sl, SiftErr *err, SiftParGroupBatch *out);

#endif							/* SIFT_AGG_V6_PAR_UNNEST_H */
