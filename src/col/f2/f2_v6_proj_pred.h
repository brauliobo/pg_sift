/*
 * col/f2/f2_v6_proj_pred.h - the predicates of a statement as the cells of a projection can be tested for them: a comparison of one column with a constant (=, <>, <, <=, >, >=) or with a list of
 * constants (IN), on a column compared as integers (integers, dates, timestamps, booleans, a numeric as its scaled integer) or as bytes (text, uuid). A NULL cell satisfies none of them, as in a
 * WHERE clause; a cell that carries an exception tag (a value the column could not hold) cannot be judged, and the statement is then read from the base.
 */
#ifndef SIFT_COL_F2_V6_PROJ_PRED_H
#define SIFT_COL_F2_V6_PROJ_PRED_H

#include "core/host.h"

#include "col/f2/f2_proj.h"

typedef struct SiftProjConst
{
	int64		i;
	const char *s;
	uint32		len;
} SiftProjConst;

typedef struct SiftProjPred
{
	int16		leaf;
	uint8		op;				/* SiftProjOp */
	bool		bytes;			/* compared as bytes (text, uuid), else as integers */
	int			nvals;			/* 1 but for IN */
	const SiftProjConst *vals;
} SiftProjPred;

extern bool sift_proj_pred_pass(const SiftProjPred *p, const SiftCell *row);
extern bool sift_proj_preds_pass(const SiftProjPred *p, int n, const SiftCell *row);

/* The predicate of a spec (its constants inline) as the cells are tested for it; buf holds its constants (SIFT_PROJ_PRED_VALS of them) */
extern void sift_proj_pred_of_spec(const SiftProjSpecPred *sp, SiftProjConst *buf, SiftProjPred *out);

/* The reverse: false when the constants do not fit a spec's inline room (more than SIFT_PROJ_PRED_VALS of them, a string longer than SIFT_PROJ_PRED_BYTES) */
extern bool sift_proj_spec_pred_of_pred(const SiftProjPred *p, SiftProjSpecPred *out);

/* The query's predicate is the spec's: it holds of every row the projection keeps */
extern bool sift_proj_pred_is_spec(const SiftProjPred *q, const SiftProjSpecPred *w);

/* The predicates of the query as a stored row is tested for them (map->pred_use): as they are, with the bound of a bucket, or left out; n is the count, out has room for n, buf for n constants */
extern int	sift_proj_stored_preds(const SiftProjRollup *map, const SiftProjPred *preds, int n, SiftProjPred *out, SiftProjConst *buf);

/* Every row that passes the query's predicate q passes the spec's w (a WHERE or FILTER of a projection): the projection holds every row the query needs */
extern bool sift_proj_pred_implies(const SiftProjPred *q, const SiftProjSpecPred *w);

/*
 * A predicate on a column the projection keeps as date_trunc(unit, column), as the bucket values are tested for it. The bound must fall on the edge of a bucket, or the bucket would hold rows on
 * both sides of it: x >= B and x < B are buckets >= B and < B when B is an edge; x <= B and x > B when B + 1 is (a date or a timestamp is an integer); x = B only for a day of a date.
 */
extern bool sift_proj_const_aligned(SiftProjTime time, SiftProjOp op, int64 val, SiftProjUnit unit, SiftProjOp *mapped_op, int64 *mapped_val);

/* False when a predicate is on a cell of the row that holds an exception tag: no verdict can be given for the row */
extern bool sift_proj_preds_decidable(const SiftProjPred *p, int n, const SiftCell *row);

/* The rows of a part sorted by its first key come to an end for a predicate when this says so: the row is past every row an ascending first key accepts (<, <=, = hold no more, the NULLs are last) */
extern bool sift_proj_pred_past_end(const SiftProjKey *first, const SiftProjPred *p, int n, const SiftCell *row);

/* What a sorted source does with a row: hand it out (it passes, or a predicate cannot be decided on it and the caller decides), leave it, or end (no later row can pass) */
typedef enum SiftProjRowVerdict
{
	SRV_TAKE = 0,
	SRV_SKIP,
	SRV_END
} SiftProjRowVerdict;

extern SiftProjRowVerdict sift_proj_row_verdict(const SiftProjKey *first, const SiftProjPred *p, int n, const SiftCell *row);

#endif							/* SIFT_COL_F2_V6_PROJ_PRED_H */
