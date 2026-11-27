/*
 * col/f2/pred/pred.h - the test of a predicate on a stored value (col/f2/pred/pred.c).
 */
#ifndef SIFT_F2_PRED_H
#define SIFT_F2_PRED_H

#include "col/am.h"
#include "core/scan_types.h"
#include "col/like/simd.h"

typedef enum Verdict
{
	V_NO,
	V_YES,
	V_MAYBE
} Verdict;


/* The order of the floats (a NaN is the greatest and equal to itself) and of the bytes (then the length) */
extern int	sift_f2_f8_cmp(double a, double b);
extern int	sift_f2_str_cmp(const char *a, uint32 alen, const char *b, uint32 blen);

/* The constants of an IN or NOT IN predicate with many of them, hashed on the first row that is tested (one probe instead of a walk of the list); lists of fewer than SIFT_PRED_SET_MIN_VALS are walked */
#define SIFT_PRED_SET_MIN_VALS	9

typedef struct SiftPredSet
{
	MemoryContext cx;
	bool		built;
	bool		has_nan;
	uint32		mask;
	uint32	   *slots;			/* the place in vals plus one, 0: empty */
} SiftPredSet;

/* What the predicate p on the leaf d says of row r of the stored values cv: yes, no, or undecided for the heap (ps: its set of constants, or NULL); the one verdict of every scan of the format */
extern Verdict sift_f2_pred_row(const SiftColDef *d, const SiftColPred *p, SiftPredSet *ps, const ColView *cv, int r);

extern bool sift_f2_in_i64(SiftPredSet *ps, const SiftColPred *p, int64 v);
extern bool sift_f2_in_f8(SiftPredSet *ps, const SiftColPred *p, double v);
extern bool sift_f2_in_str(SiftPredSet *ps, const SiftColPred *p, const char *s, uint32 len);

/* The tests take no set (NULL) where the caller keeps none: the list is walked */
/* One set for each of the n predicates, kept in cx */
extern SiftPredSet *sift_f2_pred_sets_new(MemoryContext cx, int n);

/* Whether a comparison that gave c satisfies op */
extern bool sift_f2_cmp_op(SiftColOp op, int c);

/* Whether the string satisfies a predicate on a string leaf (the vals of the predicate are the constants) */
extern bool sift_f2_str_pred(const SiftColPred *p, const char *s, uint32 len);
extern bool sift_f2_str_pred_in(SiftPredSet *ps, const SiftColPred *p, const char *s, uint32 len);

/* Whether the collation of a string predicate keeps the bytes from deciding it (a nondeterministic collation; a range under a collation that does not order by bytes): the heap decides */
extern bool sift_f2_collation_undecided(const SiftColPred *p);

/* The JSON type (SIFT_COLTAG_*) of the values a leaf of a key of a jsonb document holds as stored, and of the value of one row from its null flag and its tag: 0 where jsonb_typeof of the key is NULL
 * (a missing key, a document with no value there), SIFT_COLTAG_UNSURE where the writer did not keep the type of an overflow value */
extern uint8 sift_f2_leaf_type(const SiftColDef *d);
extern uint8 sift_f2_json_type(const SiftColDef *d, bool isnull, uint8 tag);

/* The leaf of cardinality(x) of an array that keeps its lengths: a plain integer leaf, exact for the predicates on it */
static inline bool
sift_f2_card_lane(const SiftColDef *d)
{
	return sift_col_def_arrlen_lane(d) && d->lane == 1;
}

/* What a predicate of any operator says of a string leaf's stored bytes: the verdicts of sift_f2_str_pred, and MAYBE for what the bytes of an array image or of a case folded string cannot decide (col/f2/pred/ext.h) */
extern SiftLikeVerdict sift_f2_str_verdict(const SiftColPred *p, const char *s, uint32 len);

/* A value the leaf holds in a form the test cannot judge (a wide number, the image of an array or a range, a pattern that folds case): the heap decides */
extern bool sift_f2_undecided(const SiftColDef *d, const SiftColPred *p);

#endif							/* SIFT_F2_PRED_H */
