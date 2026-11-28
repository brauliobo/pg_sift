/*
 * agg/qual/jsonb_array.c - jsonb_array_length of a list of a document the scan reads from the payload of a row the leaves cannot hold (a list the index cut, a value that is no array), with
 * PostgreSQL's own error for a value that is no array.
 */
#include "postgres.h"

#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"

#include "agg/engine.h"
#include "core/errctx.h"

bool
sift_v6_json_array_length(const char *payload, uint32 len, MemoryContext cx, int64 *n)
{
	const Jsonb *jb = memcpy(MemoryContextAlloc(cx, len), payload, len);

	*n = JB_ROOT_COUNT(jb);
	return JB_ROOT_IS_ARRAY(jb) && !JB_ROOT_IS_SCALAR(jb);
}

void
sift_v6_json_array_length_raise(const char *payload, uint32 len, MemoryContext cx)
{
	(void) DirectFunctionCall1(jsonb_array_length, PointerGetDatum(memcpy(MemoryContextAlloc(cx, len), payload, len)));
	SIFT_INVARIANT(false, "jsonb_array_length of a value that is no array raises");
}
