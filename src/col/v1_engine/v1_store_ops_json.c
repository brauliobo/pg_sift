/*
 * col/v1_engine/v1_store_ops_json.c - what the unified scan of Format 1 reads of a key of a document besides its leaf: the JSON type of each row, which the reader tests one type at a time, and the value of a
 * row whose leaf could not hold it, which is read from the document of its heap row as `->>` reads it (and its float8 cast).
 */
#include "postgres.h"

#include "utils/jsonb.h"

#include "agg/v6/v6.h"
#include "col/v1/src_exec.h"
#include "col/v1/v1_store_ops.h"

const SiftV1JsonType sift_v1_json_types[SIFT_V1_NJSON_TYPES] = {
	{SIFT_SRC_JT_STRING, "string"}, {SIFT_SRC_JT_NUMBER, "number"}, {SIFT_SRC_JT_BOOLEAN, "boolean"}, {SIFT_SRC_JT_OBJECT, "object"},
	{SIFT_SRC_JT_ARRAY, "array"}, {SIFT_SRC_JT_NONE, NULL}, {SIFT_SRC_JT_NULL, "null"}
};

bool
sift_v1_json_member(Datum doc, const char *key, SiftV6Class cls, MemoryContext cx, const char **text, uint32 *tlen, double *f)
{
	Jsonb	   *jb = DatumGetJsonbP(doc);
	JsonbValue	jv;
	Jsonb	   *member;

	if (!JB_ROOT_IS_OBJECT(jb) || getKeyJsonValueFromContainer(&jb->root, key, strlen(key), &jv) == NULL)
		return false;
	member = JsonbValueToJsonb(&jv);
	return sift_v6_json_exception(cls, (const char *) member, VARSIZE(member), cx, text, tlen, f);
}
