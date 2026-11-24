/*
 * agg/v6/par/wordsink.h - the end of a pipe (agg/v6/v6_pipe.h) of a compute thread: the key and argument batches that leave its last stage as the words that agg/v6/par/group.h folds, in pure C.
 *
 * A key is a 64-bit integer with its sign bit flipped (all ones is the NULL key), or one text key (a NULL one has the length SIFT_STR_NULL). What the words cannot say exactly is the scan's: a NULL
 * in a counted or summed argument, a key whose word is the NULL one, and more rows than the sink holds: the item is the scan's, and folds into the same groups.
 */
#ifndef SIFT_AGG_V6_PAR_WORDSINK_H
#define SIFT_AGG_V6_PAR_WORDSINK_H

#include "postgres.h"

#include "agg/v6/par/group.h"
#include "agg/v6/v6.h"
#include "agg/v6/v6_batch.h"

typedef struct SiftParWordShape
{
	SiftParGroupSpec gs;
	int			nk;				/* the keys of the statement: the key words of gs, or one text key; none: one group of key 0 */
	int			nsum;
	int			sum_agg[SIFT_PAR_GROUP_MAX_SUM];	/* the aggregate (of the spec) whose argument each sum is of */
	int			ncount;
	int			count_agg[SIFT_PAR_GROUP_MAX_SUM];	/* the aggregates COUNT(col): a NULL argument is the scan's */
} SiftParWordShape;

/* Whether the statement is one whose rows leave its pipe as words: no sets, bins, filters or derived columns; keys and the arguments of COUNT and of SUM and AVG of int2 and int4 that are
 * the columns of the index as the pipe gathers them, or of the source `via` (SV6SRC_DIM, SV6SRC_ELEM); one text key of that source (of the index too when attr_text) */
extern bool sift_par_word_shape(const SiftV6Spec *spec, SiftV6Src via, bool attr_text, SiftParWordShape *sh);

typedef struct SiftParWords SiftParWords;

/* The sink of one thread, for at most cap rows an item: made by the leader */
extern SiftParWords *sift_par_words_new(MemoryContext cx, const SiftParWordShape *sh, uint32 cap);
/* An item starts */
extern void sift_par_words_reset(SiftParWords *w);
/* The SiftV6BatchFn of the pipe: its arg is the SiftParWords */
extern void sift_par_words_sink(void *w, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec);
/* The item ended or a batch of it was refused: false with err filled (SIFT_ERR_SERIAL, SIFT_ERR_ARENA_FULL), or the words of the rows of the item */
extern bool sift_par_words_halted(const SiftParWords *w);
extern bool sift_par_words_refused(const SiftParWords *w, SiftErr *err);
extern void sift_par_words_out(const SiftParWords *w, SiftParGroupBatch *out);

#endif							/* SIFT_AGG_V6_PAR_WORDSINK_H */
