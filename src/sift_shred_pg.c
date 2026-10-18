/*
 * sift_shred_pg.c - walks a jsonb for the shredder (docs/COL-SHRED.md). The iterator keeps one frame per level and never builds
 * the document; a container no declared path reaches is not entered (skipNested), an object is left as soon as its last
 * declared key was seen, and a wide object is searched key by key.
 */
#include "sift_host.h"

#include "utils/jsonb.h"
#include "utils/numeric.h"

#include "sift_shred_pg.h"

/* An object with more than this many members per declared key is searched by key, else walked */
#define LOOKUP_RATIO 3

static void walk_value(SiftShred *sh, JsonbValue *v);

/* A wide object with few declared keys: one binary search per key beats walking every member */
static void
lookup_members(SiftShred *sh, JsonbContainer *c, int nkeys)
{
	for (int i = 0; i < nkeys; i++)
	{
		int			len;
		const char *key = sift_shred_object_key(sh, i, &len);
		JsonbValue	v;

		if (getKeyJsonValueFromContainer(c, key, len, &v) == NULL)
			continue;
		sift_shred_key(sh, key, len);
		walk_value(sh, &v);
	}
}

static void
walk_object(SiftShred *sh, JsonbContainer *c)
{
	int			nkeys = sift_shred_object_keys(sh);
	JsonbIterator *it;
	JsonbValue	k,
				v;

	if (nkeys > 0 && JsonContainerSize(c) > LOOKUP_RATIO * nkeys)
	{
		lookup_members(sh, c, nkeys);
		return;
	}
	it = JsonbIteratorInit(c);
	JsonbIteratorNext(&it, &v, true);	/* WJB_BEGIN_OBJECT */
	while (JsonbIteratorNext(&it, &k, true) == WJB_KEY)
	{
		SiftShredKey act = sift_shred_key(sh, k.val.string.val, k.val.string.len);

		JsonbIteratorNext(&it, &v, true);	/* WJB_VALUE */
		if (act == SIFT_SHRED_SKIP)
			continue;
		walk_value(sh, &v);
		if (act == SIFT_SHRED_STOP)
		{
			pfree(it);
			return;
		}
	}
}

static void
walk_array(SiftShred *sh, JsonbContainer *c)
{
	JsonbIterator *it = JsonbIteratorInit(c);
	JsonbValue	v;

	JsonbIteratorNext(&it, &v, true);	/* WJB_BEGIN_ARRAY */
	while (JsonbIteratorNext(&it, &v, true) == WJB_ELEM)
	{
		walk_value(sh, &v);
		if (sift_shred_full(sh))
		{
			pfree(it);
			return;
		}
	}
}

static void
walk_number(SiftShred *sh, Numeric num)
{
	char	   *text;

	if (!sift_shred_wants_number(sh))
	{
		sift_shred_number(sh, NULL, 0);
		return;
	}
	text = numeric_normalize(num);
	sift_shred_number(sh, text, strlen(text));
	pfree(text);
}

static void
walk_value(SiftShred *sh, JsonbValue *v)
{
	switch (v->type)
	{
		case jbvNull:
			sift_shred_null(sh);
			break;
		case jbvBool:
			sift_shred_bool(sh, v->val.boolean);
			break;
		case jbvString:
			sift_shred_string(sh, v->val.string.val, v->val.string.len);
			break;
		case jbvNumeric:
			walk_number(sh, v->val.numeric);
			break;
		case jbvBinary:
			{
				JsonbContainer *c = v->val.binary.data;
				bool		is_array = JsonContainerIsArray(c);

				if (!sift_shred_enter(sh, is_array))
					break;
				check_stack_depth();
				if (is_array)
					walk_array(sh, c);
				else
					walk_object(sh, c);
				sift_shred_leave(sh);
				break;
			}
		default:
			elog(ERROR, "sift shred: unexpected jsonb value type %d", v->type);
	}
}

void
sift_shred_container(SiftShred *sh, JsonbContainer *c)
{
	JsonbValue	v;

	if (JsonContainerIsScalar(c))
	{
		JsonbIterator *it = JsonbIteratorInit(c);

		JsonbIteratorNext(&it, &v, true);	/* WJB_BEGIN_ARRAY, rawScalar */
		JsonbIteratorNext(&it, &v, true);	/* WJB_ELEM */
		walk_value(sh, &v);
		JsonbIteratorNext(&it, &v, true);	/* WJB_END_ARRAY frees the iterator */
		return;
	}
	v.type = jbvBinary;
	v.val.binary.data = c;
	v.val.binary.len = c->header & JB_CMASK;
	walk_value(sh, &v);
}

void
sift_shred_jsonb(SiftShred *sh, Jsonb *jb)
{
	sift_shred_begin(sh);
	sift_shred_container(sh, &jb->root);
	sift_shred_finish(sh);
}
