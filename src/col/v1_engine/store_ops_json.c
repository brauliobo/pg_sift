/*
 * col/v1_engine/store_ops_json.c - what the unified scan of Format 1 reads of a key of a document besides its leaf: the JSON type of each row, which the reader tests one type at a time, and the value of a
 * row whose leaf could not hold it, which is read from the document of its heap row as `->>` reads it (and its float8 cast).
 */
#include "postgres.h"

#include "utils/jsonb.h"

#include "agg/engine.h"
#include "col/v1/src_exec.h"
#include "col/v1/store_ops.h"

const SiftV1JsonType sift_v1_json_types[SIFT_V1_NJSON_TYPES] = {
	{SIFT_SRC_JT_STRING, "string"}, {SIFT_SRC_JT_NUMBER, "number"}, {SIFT_SRC_JT_BOOLEAN, "boolean"}, {SIFT_SRC_JT_OBJECT, "object"},
	{SIFT_SRC_JT_ARRAY, "array"}, {SIFT_SRC_JT_NONE, NULL}, {SIFT_SRC_JT_NULL, "null"}
};

Jsonb *
sift_v1_json_member(Datum doc, const char *key)
{
	Jsonb	   *jb = DatumGetJsonbP(doc);
	JsonbValue	jv;

	if (!JB_ROOT_IS_OBJECT(jb) || getKeyJsonValueFromContainer(&jb->root, key, strlen(key), &jv) == NULL)
		return NULL;
	return JsonbValueToJsonb(&jv);
}
