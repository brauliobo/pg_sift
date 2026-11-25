/*
 * agg/v6/par/wordsink.h - the end of a pipe (agg/v6/pipe.h) of a compute thread: the key and argument batches that leave its last stage as the words that agg/v6/par/group.h folds, in pure C.
 *
 * A key is a 64-bit integer with its sign bit flipped (all ones is the NULL key), or a text (a NULL one has the length SIFT_STR_NULL). A NULL argument of COUNT(col), SUM or AVG is in the sums:
 * the validity of each argument is a sum of its own, so that the group says how many arguments were not NULL. What the words cannot say exactly is the scan's: a key whose word is the NULL one, and
 * more rows than the sink holds: the item is the scan's, and folds into the same groups.
 */
#ifndef SIFT_AGG_V6_PAR_WORDSINK_H
#define SIFT_AGG_V6_PAR_WORDSINK_H

#include "postgres.h"

#include "agg/v6/par/group.h"
#include "agg/v6/v6.h"
#include "agg/v6/batch.h"

#define SIFT_PAR_WORD_AGGS	16

/* A sum of gs: the validity (1 for a non-NULL argument) or the value (0 for a NULL one) of the argument of aggregate agg */
typedef struct SiftParWordSlot
{
	int			agg;
	bool		valid;
} SiftParWordSlot;

typedef struct SiftParWordShape
{
	SiftParGroupSpec gs;
	int			nk;				/* the keys of the statement: the keys of gs, or none: one group of key 0 */
	int			nsum;
	SiftParWordSlot slot[SIFT_PAR_GROUP_MAX_SUM];
	int8		n_slot[SIFT_PAR_WORD_AGGS];	/* aggregate j: the sum that counts its non-NULL arguments, or -1: the rows of the group */
	int8		v_slot[SIFT_PAR_WORD_AGGS];	/* aggregate j: the sum of its arguments, or -1 */
} SiftParWordShape;

/* Whether the statement is one whose rows leave its pipe as words: no sets, bins, filters or derived columns; keys and the arguments of COUNT and of SUM and AVG of int2 and int4 that are
 * the columns of the index as the pipe gathers them, or of the source `via` (SV6SRC_DIM, SV6SRC_ELEM); the text keys of that source (of the index too when attr_text) */
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
