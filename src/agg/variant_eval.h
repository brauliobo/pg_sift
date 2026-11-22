/*
 * agg/variant_eval.h - the evaluation of the exception stream of a shredded JSONB path (agg/vec.h SiftVariantExcStream) without a
 * heap fetch: the rows whose value is not of the path's type are in the granule, next to the typed column, and a query reads them
 * from there.
 *
 * Payload of an entry, a varlena with a 4 byte header, by kind: SVAR_NUM_SPECIAL a numeric; SVAR_JSONB_SCALAR, SVAR_JSONB_DOC a
 * jsonb; SVAR_CAST_ERROR the text of the value, without a header. The value of an exception is that of the SQL cast of its text to
 * the type of the path (SVEC_F64: float8, SVEC_I64: int8): a numeric goes through the numeric cast (a NaN or an infinity is a
 * float8 and an error as an int8; a fraction is rounded as an int8), a string through the type's input function (so '12' is 12),
 * a boolean or a document through its text ('true', '{"a": 1}'), a JSON null is NULL, and a value the cast refuses raises the error
 * the cast raises, only if its row survives the predicates and the deletes.
 *
 * A granule is aggregated either by the typed column over the rows of (active & ~exc_mask) (sift_variant_primary_mask) with the
 * exceptions of the active rows folded in (sift_variant_eval), or, when its pre-aggregates are usable, by the pre-aggregates of
 * the typed rows with the exceptions folded in (sift_variant_fold_granule): the cost is the number of exceptions, not of rows.
 */
#ifndef SIFT_AGG_VARIANT_EVAL_H
#define SIFT_AGG_VARIANT_EVAL_H

#include "agg/vec.h"

/* The values of the exceptions of one 2048-row slice of a granule, cast to the type of the path (allocate with sift_variant_vals_new: the vector is SIFT_VEC_ALIGN aligned) */
typedef struct __attribute__((aligned(SIFT_VEC_ALIGN))) SiftVariantVals
{
	uint16		rows[SIFT_VEC_LEN];	/* the row (in the slice) of value k */
	union
	{
		int64		i64[SIFT_VEC_LEN + 8];
		double		f64[SIFT_VEC_LEN + 8];
	}			v __attribute__((aligned(SIFT_VEC_ALIGN)));
	SiftVec		vec;			/* dense, all valid: the non-NULL values, SIFT_VEC_PAD readable bytes after the last */
	uint16		n_active;		/* the exception rows that survived (count(*)), NULL values included */
} SiftVariantVals;

/* count(*) (rows, the caller's), count(x), sum, min and max of a float8 or int8 path; min and max are valid when count > 0 */
typedef struct SiftVarAgg
{
	SiftVecType type;
	int64		rows;
	int64		count;
	SiftI128	isum;
	double		fsum;
	union
	{
		int64		i;
		double		f;
	}			min, max;
} SiftVarAgg;

extern SiftVariantVals *sift_variant_vals_new(void);
extern void sift_var_agg_init(SiftVarAgg *a, SiftVecType type);
/* Adds the values of a dense vector of the type of a: count grows by its length */
extern void sift_var_agg_add_vec(SiftVarAgg *a, const SiftVec *v);
extern void sift_var_agg_merge(SiftVarAgg *dst, const SiftVarAgg *src);

/* mask (the 32 words of slice number slice, n rows: the predicates and the deletes) &= ~the exception rows; returns the rows left */
extern uint16 sift_variant_primary_mask(uint64 *mask, const SiftVariantExcStream *exc, uint32 slice, uint16 n);

/* The exceptions of slice number slice (n rows) whose bit is set in active (SIFT_VEC_WORDS words, NULL: all), cast to type */
extern void sift_variant_eval(SiftVariantVals *out, const SiftVariantExcStream *exc, uint32 slice, const uint64 *active, uint16 n,
							  SiftVecType type);

/* The exceptions of the n_rows rows of a granule whose bit is set in active (128 words, NULL: all) folded into a */
extern void sift_variant_fold_granule(SiftVarAgg *a, const SiftVariantExcStream *exc, const uint64 *active, uint32 n_rows);

#endif							/* SIFT_AGG_VARIANT_EVAL_H */
