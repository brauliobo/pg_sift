/*
 * agg/par/unnest.h - the pairs of (row, element) of a LATERAL unnest of an array column of the index, made and folded by the compute threads of the backend (core/par/pool.h), in pure C.
 *
 * The leader reads the batches of the index (col/f2/sink/ext.h), decides which rows take part (the deletion vector, the WHERE clause of the table, the visibility of the heap row) and copies a batch of
 * up to SIFT_VEC_LEN rows into the slot of a ring: the columns of the rows, the lengths and elements of their arrays (one column for unnest, the declared columns of jsonb_to_recordset or the elements of
 * jsonb_array_elements over the shredded leaves of a jsonb document), the elements that pass the quals of the element, the rows whose array the index cannot hold left out (the leader reads those from the
 * heap, as the serial run does). A thread expands the slot into pairs (agg/unnest/unnest.h), gathers the columns beside the element and runs them through a pipe of its own (agg/unnest/pairs.h)
 * whose end is the words of agg/par/wordsink.h; it knows nothing of the server. A slot a thread cannot fold exactly (a NULL in a counted or summed column, more pairs than its sink holds) is the leader's: it
 * expands the same slot into the pipe of the statement.
 */
#ifndef SIFT_AGG_V6_PAR_UNNEST_H
#define SIFT_AGG_V6_PAR_UNNEST_H

#include "postgres.h"

#include "agg/par/wordsink.h"
#include "agg/unnest/pairs.h"
#include "agg/vexpr/vexpr.h"

#define SIFT_UN_COLS_MAX	SIFT_VEXPR_MAX_REGS
#define SIFT_UN_PAIRS_MAX	(64 * SIFT_VEC_LEN)

/* One batch of rows with their arrays, immutable from the leader's feed until the release of its slot */
typedef struct SiftUnSlot
{
	uint32		n;				/* the rows */
	uint32		nelems;			/* the elements of their arrays */
	int			ncols;
	int			nel;			/* the columns of the element */
	SiftVec		col[SIFT_UN_COLS_MAX];
	SiftVec		elem[SIFT_UN_ELEMS_MAX];	/* those the statement reads; the others are empty */
	const uint64 *emask;		/* the elements that pass the quals of the element (nelems bits); NULL: all of them */
	uint64		mask[SIFT_VEC_WORDS];	/* the rows that take part, those the index cannot hold left out */
	uint64		doc_valid[SIFT_VEC_WORDS];
	uint32		lengths[SIFT_VEC_LEN];
	uint32		offsets[SIFT_VEC_LEN + 1];
	uint32		block_off[SIFT_VEC_LEN / 64 + 1];
	/* what the vectors point to: the leader makes it, and grows it for a batch that needs more */
	void	   *data[SIFT_UN_COLS_MAX];
	uint64	   *valid[SIFT_UN_COLS_MAX];
	void	   *edata[SIFT_UN_ELEMS_MAX];
	uint64	   *evalid[SIFT_UN_ELEMS_MAX];
	uint64	   *ebits;
	uint32		ecap;
	char	   *str;
	size_t		strcap;
} SiftUnSlot;

/* The pairs of the slot into p, until halt (when given) refuses a batch: the rows of the slot in the order of the serial run */
extern void sift_un_slot_pairs(SiftUnPairs *p, const SiftUnSlot *sl, bool left, const SiftParWords *halt);

/* Whether the statement is one the threads fold: an unnest of an array or of the leaves of a jsonb array (jsonb_array_elements, jsonb_to_recordset) with keys and arguments that are plain columns of the table or the element, as agg/par/wordsink.h says */
extern bool sift_par_unnest_shape(const SiftV6Spec *spec, SiftParWordShape *sh);

typedef struct SiftParUnnest SiftParUnnest;

/* The sink, pairs and pipe of each of nthreads threads, over the pipe of the leader (the types of whose fact slots are the ncols columns of the slot, then the nel of the element, those of eused the pairs gather) */
extern SiftParUnnest *sift_par_unnest_new(MemoryContext cx, const SiftParWordShape *sh, const SiftV6Pipe *pipe, bool left, int nthreads, int ncols, int nel, uint32 eused);
/* Thread `worker`: the words of the pairs of the slot, valid until its next call; SIFT_ERR_SERIAL, SIFT_ERR_ARENA_FULL leave the slot to the leader */
extern bool sift_par_unnest_batch(SiftParUnnest *un, int worker, const SiftUnSlot *sl, SiftErr *err, SiftParGroupBatch *out);

#endif							/* SIFT_AGG_V6_PAR_UNNEST_H */
