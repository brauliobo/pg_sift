/*
 * agg/vnum_test.c - test-only SQL function sift._test_vnum_program_ok, compiled only with SIFT_TESTING and registered by the pg_sift_test extension:
 * whether the proof of a vector program of an aggregate argument takes a postfix program, given as the codes of its steps (0 leaf, 1 constant,
 * 2 add, 3 sub, 4 mul, 5 neg, other: an operation it does not run).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "fmgr.h"
#include "utils/array.h"

#include "agg/spec.h"
#include "agg/vnum.h"

PG_FUNCTION_INFO_V1(sift_test_vnum_program_ok);

Datum
sift_test_vnum_program_ok(PG_FUNCTION_ARGS)
{
	static const SiftAggNxOp ops[] = {SNX_LEAF, SNX_CONST, SNX_ADD, SNX_SUB, SNX_MUL, SNX_NEG, SNX_ABS};
	ArrayType  *codes = PG_GETARG_ARRAYTYPE_P(0);
	int			n = ArrayGetNItems(ARR_NDIM(codes), ARR_DIMS(codes));
	const int32 *code = (const int32 *) ARR_DATA_PTR(codes);
	SiftAggNx  *step = palloc0(Max(n, 1) * sizeof(SiftAggNx));
	SiftAggNum	p = {.n = n, .step = step};

	for (int k = 0; k < n; k++)
		step[k].op = ops[code[k]];
	PG_RETURN_BOOL(vnum_program_ok(&p));
}

#endif
