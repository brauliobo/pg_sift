/*
 * col/test/xstamp_test.c - test hooks for the xid stamp of a segment (col/xstamp.c): the encoding and the union of the transactions a stamp waits for, on xids of the test's choosing
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "col/xstamp.h"

PG_FUNCTION_INFO_V1(sift_test_xstamp_pack);
PG_FUNCTION_INFO_V1(sift_test_xstamp_union);

/* A stamp as text: next:base:open (open in hex), 'none' for a segment that says nothing */
static Datum
stamp_text(SiftXStamp s)
{
	if (!TransactionIdIsNormal(s.next))
		PG_RETURN_TEXT_P(cstring_to_text("none"));
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%u:%u:%llx", s.next, s.base, (unsigned long long) s.open)));
}

static SiftXStamp
stamp_parse(text *t)
{
	SiftXStamp	s = {InvalidTransactionId, InvalidTransactionId, 0};
	unsigned long long open;

	if (sscanf(text_to_cstring(t), "%u:%u:%llx", &s.next, &s.base, &open) != 3)
		return (SiftXStamp) {InvalidTransactionId, InvalidTransactionId, 0};
	s.open = open;
	return s;
}

/* sift._test_xstamp_pack(top int8, open int8[]): the stamp of rows whose latest inserter is top, of which those in open had not finished */
Datum
sift_test_xstamp_pack(PG_FUNCTION_ARGS)
{
	ArrayType  *a = PG_GETARG_ARRAYTYPE_P(1);
	int64	   *v = (int64 *) ARR_DATA_PTR(a);
	int			n = ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a));
	TransactionId xid[SIFT_XSTAMP_OPEN_MAX + 1];

	if (n > SIFT_XSTAMP_OPEN_MAX + 1)
		elog(ERROR, "too many transactions");
	for (int i = 0; i < n; i++)
		xid[i] = (TransactionId) v[i];
	return stamp_text(sift_xstamp_pack((TransactionId) PG_GETARG_INT64(0), xid, n));
}

/* sift._test_xstamp_union(a text, b text): the stamp of the rows of both */
Datum
sift_test_xstamp_union(PG_FUNCTION_ARGS)
{
	SiftXStamp	a = stamp_parse(PG_GETARG_TEXT_PP(0)),
				b = stamp_parse(PG_GETARG_TEXT_PP(1));

	if (!sift_xstamp_union(&a, &b))
		a.next = InvalidTransactionId;
	return stamp_text(a);
}
#endif
