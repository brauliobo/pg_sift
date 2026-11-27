/*
 * col/shred/pg.c - walks a jsonb for the shredder (docs/COL-SHRED.md). The iterator keeps one frame per level and never builds
 * the document; a container no declared path reaches is not entered (skipNested), an object is left as soon as its last
 * declared key was seen, and a wide object is searched key by key.
 */
#include "core/host.h"

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"

#include "col/shred/pg.h"

/* An object with more than this many members per declared key is searched by key, else walked */
#define LOOKUP_RATIO 3

static void walk_value(SiftShred *sh, JsonbValue *v, SiftShredRecheck *rc);

/* A wide object with few declared keys: one binary search per key beats walking every member */
static void
lookup_members(SiftShred *sh, JsonbContainer *c, int nkeys, SiftShredRecheck *rc)
{
	for (int i = 0; i < nkeys; i++)
	{
		int			len;
		const char *key = sift_shred_object_key(sh, i, &len);
		JsonbValue	v;

		CHECK_FOR_INTERRUPTS();
		if (getKeyJsonValueFromContainer(c, key, len, &v) == NULL)
			continue;
		sift_shred_key(sh, key, len);
		if (rc != NULL)
		{
			rc->key = key;
			rc->keylen = len;
		}
		walk_value(sh, &v, rc);
	}
}

static void
walk_object(SiftShred *sh, JsonbContainer *c, SiftShredRecheck *rc)
{
	int			nkeys = sift_shred_object_keys(sh);
	JsonbIterator *it;
	JsonbValue	k,
				v;

	if (nkeys > 0 && JsonContainerSize(c) > LOOKUP_RATIO * nkeys)
	{
		lookup_members(sh, c, nkeys, rc);
		return;
	}
	it = JsonbIteratorInit(c);
	JsonbIteratorNext(&it, &v, true);	/* WJB_BEGIN_OBJECT */
	while (JsonbIteratorNext(&it, &k, true) == WJB_KEY)
	{
		SiftShredKey act = sift_shred_key(sh, k.val.string.val, k.val.string.len);

		CHECK_FOR_INTERRUPTS();
		JsonbIteratorNext(&it, &v, true);	/* WJB_VALUE */
		if (act == SIFT_SHRED_SKIP)
			continue;
		if (rc != NULL)
		{
			rc->key = k.val.string.val;
			rc->keylen = k.val.string.len;
		}
		walk_value(sh, &v, rc);
		if (act == SIFT_SHRED_STOP)
		{
			pfree(it);
			return;
		}
	}
}

static void
walk_array(SiftShred *sh, JsonbContainer *c, SiftShredRecheck *rc)
{
	JsonbIterator *it = JsonbIteratorInit(c);
	JsonbValue	v;

	JsonbIteratorNext(&it, &v, true);	/* WJB_BEGIN_ARRAY */
	while (JsonbIteratorNext(&it, &v, true) == WJB_ELEM)
	{
		CHECK_FOR_INTERRUPTS();
		walk_value(sh, &v, rc);
		if (sift_shred_full(sh))
		{
			pfree(it);
			return;
		}
	}
}

/* the target is an integer leaf: `(doc ->> 'k')::int` of a number with digits after its point (3.0) raises, so the leaf cannot hold it */
static bool
integral_leaf(const SiftShred *sh)
{
	int			col;

	return sift_shred_target(sh, &col) == SIFT_SHRED_T_LEAF && sh->spec->cols[col].type >= SIFT_SHRED_INT2 && sh->spec->cols[col].type <= SIFT_SHRED_INT8;
}

static void
walk_number(SiftShred *sh, Numeric num)
{
	char	   *text;

	if (sift_shred_wants_number_text(sh))
		text = DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(num)));
	else if (!sift_shred_wants_number(sh) || (integral_leaf(sh) && DatumGetInt32(DirectFunctionCall1(numeric_scale, NumericGetDatum(num))) > 0))
	{
		sift_shred_number(sh, NULL, 0);
		return;
	}
	else
		text = numeric_normalize(num);
	sift_shred_number(sh, text, strlen(text));
	pfree(text);
}

static void
walk_plain(SiftShred *sh, JsonbValue *v, SiftShredRecheck *rc)
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
					walk_array(sh, c, rc);
				else
					walk_object(sh, c, rc);
				sift_shred_leave(sh);
				break;
			}
		default:
			SIFT_INVARIANT(false, "sift shred: unexpected jsonb value type %d", v->type);
	}
}

const char *
sift_shred_pay_jsonb(const void *ev, uint32 *len)
{
	Jsonb	   *jb = JsonbValueToJsonb((JsonbValue *) ev);

	*len = VARSIZE(jb);
	return (const char *) jb;
}

static void
walk_value(SiftShred *sh, JsonbValue *v, SiftShredRecheck *rc)
{
	sh->ev = v;
	if (rc == NULL)
		walk_plain(sh, v, NULL);
	else
	{
		if (!sift_shred_recheck_value(sh, v, rc))
			walk_plain(sh, v, rc);
		sift_shred_recheck_settle(sh, rc);
	}
}

void
sift_shred_container_rc(SiftShred *sh, JsonbContainer *c, SiftShredRecheck *rc)
{
	JsonbValue	v;

	if (JsonContainerIsScalar(c))
	{
		JsonbIterator *it = JsonbIteratorInit(c);

		JsonbIteratorNext(&it, &v, true);	/* WJB_BEGIN_ARRAY, rawScalar */
		JsonbIteratorNext(&it, &v, true);	/* WJB_ELEM */
		walk_value(sh, &v, rc);
		JsonbIteratorNext(&it, &v, true);	/* WJB_END_ARRAY frees the iterator */
		return;
	}
	v.type = jbvBinary;
	v.val.binary.data = c;
	v.val.binary.len = c->header & JB_CMASK;
	walk_value(sh, &v, rc);
}

void
sift_shred_container(SiftShred *sh, JsonbContainer *c)
{
	sift_shred_container_rc(sh, c, NULL);
}

void
sift_shred_jsonb(SiftShred *sh, Jsonb *jb)
{
	sift_shred_begin(sh);
	sift_shred_container(sh, &jb->root);
	sift_shred_finish(sh);
}

/* The storage pg_type gives an element type sift_shred_array_elem_ok, without a syscache lookup for every array */
static void
elem_storage(Oid elem, int16 *len, bool *byval, char *align)
{
	*len = elem == INT2OID ? sizeof(int16) : elem == INT4OID ? sizeof(int32) : elem == INT8OID ? sizeof(int64) : -1;
	*byval = elem == INT2OID || elem == INT4OID || (elem == INT8OID && FLOAT8PASSBYVAL);
	*align = elem == INT2OID ? TYPALIGN_SHORT : elem == INT8OID ? TYPALIGN_DOUBLE : TYPALIGN_INT;
}

/* A native array as the value of a list: its elements flattened in storage order (a multidimensional array is one list, as unnest reads it) */
void
sift_shred_array(SiftShred *sh, ArrayType *arr)
{
	Oid			elem = ARR_ELEMTYPE(arr);
	int16		len;
	bool		byval;
	char		align;
	Datum	   *vals;
	bool	   *nulls;
	int			n;

	if (!sift_shred_enter(sh, true))
		return;
	elem_storage(elem, &len, &byval, &align);
	deconstruct_array(arr, elem, len, byval, align, &vals, &nulls, &n);
	for (int i = 0; i < n; i++)
	{
		CHECK_FOR_INTERRUPTS();
		if (nulls[i])
			sift_shred_null(sh);
		else if (elem == INT2OID || elem == INT4OID || elem == INT8OID)
		{
			char		buf[24];
			int64		v = elem == INT2OID ? DatumGetInt16(vals[i]) : elem == INT4OID ? DatumGetInt32(vals[i]) : DatumGetInt64(vals[i]);
			int			l = pg_lltoa(v, buf);

			sift_shred_number(sh, buf, l);
		}
		else
		{
			text	   *t = DatumGetTextPP(vals[i]);

			sift_shred_string(sh, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
		}
		if (sift_shred_full(sh))
			break;
	}
	pfree(vals);
	pfree(nulls);
	sift_shred_leave(sh);
}

/* the element types of a native array a path may name (the planner tests them with literals of the same type) */
bool
sift_shred_array_elem_ok(Oid elem)
{
	return elem == TEXTOID || elem == VARCHAROID || elem == INT2OID || elem == INT4OID || elem == INT8OID;
}
