/*
 * col/suggest.c - sift.suggest_paths(rel, column, sample): the paths option (docs/COL-SHRED.md) that a jsonb column asks for, read from its documents.
 * Advice only: it changes nothing and returns text. The documents are those of SAMPLE_CHUNKS stretches of heap blocks, evenly spaced over the table, so
 * the same table gives the same answer. A key becomes a typed path when it is a scalar of one type in most of the documents that have it (integers int4, or
 * int8 beyond 32 bits, with numbers that are not integers float8, text, bool, and astext for a mix), a map of more than WILD_KEYS keys of one scalar type a
 * wildcard, an array of objects a group; a key that is rare or has no single shape is left out.
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"

#include "col/pg.h"
#include "col/shred.h"

#define SAMPLE_CHUNKS	16		/* stretches of blocks read, evenly spaced */
#define CHUNK_BLOCKS	32		/* the most blocks of a stretch */
#define WILD_KEYS		32		/* a map with more keys of one scalar type is a wildcard */
#define SHAPE_PCT		90		/* the share of a key's values that decides its shape */
#define RARE_PERMILLE	10		/* a key in fewer documents than this share is left out */
#define MAX_COLS		256		/* limits of the spec (docs/COL-SHRED.md) */
#define MAX_GROUPS		64

typedef struct SNode
{
	char	   *name;
	uint32		seen,			/* values at this path */
				nnull,
				nbool,
				nint,
				nfloat,
				ntext,
				nobj,
				narr;
	bool		big;			/* an integer beyond 32 bits */
	struct SNode **kid;			/* object members */
	int			nkids,
				capkids;
	struct SNode *elem;			/* the elements of the arrays at this path, together */
} SNode;

typedef struct Ctx
{
	MemoryContext tree;
	int			ncols,
				ngroups;
	uint32		docs;
} Ctx;

static SNode *
node_new(Ctx *cx, const char *name)
{
	SNode	   *n = MemoryContextAllocZero(cx->tree, sizeof(SNode));

	n->name = name ? MemoryContextStrdup(cx->tree, name) : NULL;
	return n;
}

static SNode *
kid_find(Ctx *cx, SNode *n, const char *key, int len)
{
	for (int i = 0; i < n->nkids; i++)
		if (strlen(n->kid[i]->name) == (size_t) len && memcmp(n->kid[i]->name, key, len) == 0)
			return n->kid[i];
	if (n->nkids == n->capkids)
	{
		n->capkids = n->capkids ? n->capkids * 2 : 8;
		n->kid = n->kid ? repalloc(n->kid, n->capkids * sizeof(SNode *)) : MemoryContextAlloc(cx->tree, n->capkids * sizeof(SNode *));
	}
	{
		char	   *name = pnstrdup(key, len);
		SNode	   *k = node_new(cx, name);

		pfree(name);
		return n->kid[n->nkids++] = k;
	}
}

static void value(Ctx *cx, SNode *n, JsonbIterator **it, JsonbIteratorToken tok, JsonbValue *v);

/* the members or elements of the container the iterator has just opened, up to its end */
static void
container(Ctx *cx, SNode *n, JsonbIterator **it, bool isobj)
{
	JsonbValue	v;
	JsonbIteratorToken tok;

	while ((tok = JsonbIteratorNext(it, &v, false)) != WJB_DONE)
	{
		if (tok == WJB_END_OBJECT || tok == WJB_END_ARRAY)
			return;
		if (isobj)
		{
			JsonbValue	val;
			SNode	   *k;

			if (tok != WJB_KEY)
				continue;
			k = kid_find(cx, n, v.val.string.val, v.val.string.len);
			tok = JsonbIteratorNext(it, &val, false);
			value(cx, k, it, tok, &val);
		}
		else
		{
			if (n->elem == NULL)
				n->elem = node_new(cx, NULL);
			value(cx, n->elem, it, tok, &v);
		}
	}
}

static void
value(Ctx *cx, SNode *n, JsonbIterator **it, JsonbIteratorToken tok, JsonbValue *v)
{
	n->seen++;
	if (tok == WJB_BEGIN_OBJECT)
	{
		n->nobj++;
		container(cx, n, it, true);
		return;
	}
	if (tok == WJB_BEGIN_ARRAY)
	{
		n->narr++;
		container(cx, n, it, false);
		return;
	}
	switch (v->type)
	{
		case jbvNull:
			n->nnull++;
			break;
		case jbvBool:
			n->nbool++;
			break;
		case jbvString:
			n->ntext++;
			break;
		case jbvNumeric:
			{
				/* numeric_out keeps the scale of the text: 5.0 is not an integer, as for the shredder */
				char	   *s = DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(v->val.numeric)));
				bool		integral = strpbrk(s, ".eE") == NULL;

				if (integral)
				{
					char	   *end;
					int64		x;

					errno = 0;
					x = strtoll(s, &end, 10);
					if (errno == ERANGE || *end != '\0')
						n->nfloat++;
					else
					{
						n->nint++;
						n->big |= x > PG_INT32_MAX || x < PG_INT32_MIN;
					}
				}
				else
					n->nfloat++;
				pfree(s);
			}
			break;
		default:
			break;
	}
}

typedef enum Shape
{
	SH_NONE,
	SH_SCALAR,
	SH_OBJECT,
	SH_ARRAY
} Shape;

static Shape
shape_of(const SNode *n)
{
	uint32		scalar = n->nbool + n->nint + n->nfloat + n->ntext,
				live = n->seen - n->nnull;

	if (live == 0)
		return SH_NONE;
	if (scalar * 100 >= (uint64) live * SHAPE_PCT)
		return SH_SCALAR;
	if (n->nobj * 100 >= (uint64) live * SHAPE_PCT)
		return SH_OBJECT;
	if (n->narr * 100 >= (uint64) live * SHAPE_PCT)
		return SH_ARRAY;
	return SH_NONE;
}

/* the type name of a scalar node; every kind of value the node saw decides it, so that no value overflows */
static const char *
scalar_type(const SNode *n)
{
	if (n->ntext > 0)
		return n->nbool + n->nint + n->nfloat > 0 ? "astext" : "text";
	if (n->nbool > 0)
		return n->nint + n->nfloat > 0 ? "astext" : "bool";
	if (n->nfloat > 0)
		return "float8";
	return n->big ? "int8" : "int4";
}

static bool
bare_key(const char *k)
{
	if (*k == '\0')
		return false;
	for (; *k; k++)
		if (!((*k >= 'a' && *k <= 'z') || (*k >= 'A' && *k <= 'Z') || (*k >= '0' && *k <= '9') || *k == '_' || *k == '$' || (unsigned char) *k >= 0x80))
			return false;
	return true;
}

static void
append_key(StringInfo s, const char *k)
{
	if (bare_key(k))
	{
		appendStringInfoString(s, k);
		return;
	}
	appendStringInfoChar(s, '"');
	for (; *k; k++)
	{
		if (*k == '"')
			appendStringInfoChar(s, '"');
		appendStringInfoChar(s, *k);
	}
	appendStringInfoChar(s, '"');
}

static int
cmp_kid(const void *a, const void *b)
{
	const SNode *x = *(SNode *const *) a,
			   *y = *(SNode *const *) b;

	if (x->seen != y->seen)
		return x->seen > y->seen ? -1 : 1;
	return strcmp(x->name, y->name);
}

static void
add_item(StringInfo out, const char *item)
{
	appendStringInfo(out, "%s%s", out->len ? ", " : "", item);
}

static void emit(Ctx *cx, SNode *n, const char *prefix, StringInfo out);

/* the keys of object n below prefix: scalars as typed paths, objects flat below their path, arrays as lists or groups */
static void
emit(Ctx *cx, SNode *n, const char *prefix, StringInfo out)
{
	bool		wild = n->nkids > WILD_KEYS && prefix[0] != '\0';
	const char *wtype = NULL;

	for (int i = 0; wild && i < n->nkids; i++)
	{
		const char *t = shape_of(n->kid[i]) == SH_SCALAR ? scalar_type(n->kid[i]) : NULL;

		if (t == NULL || (wtype != NULL && strcmp(t, wtype) != 0))
			wild = false;
		wtype = wtype ? wtype : t;
	}
	if (wild && wtype != NULL)
	{
		if (cx->ncols < MAX_COLS)
		{
			add_item(out, psprintf("%s.* %s", prefix, wtype));
			cx->ncols++;
		}
		return;
	}
	qsort(n->kid, n->nkids, sizeof(SNode *), cmp_kid);
	for (int i = 0; i < n->nkids; i++)
	{
		SNode	   *k = n->kid[i];
		StringInfoData path;
		Shape		sh = shape_of(k);

		if (sh == SH_NONE || (uint64) (k->seen - k->nnull) * 1000 < (uint64) n->seen * RARE_PERMILLE)
			continue;
		initStringInfo(&path);
		if (prefix[0] != '\0')
			appendStringInfo(&path, "%s.", prefix);
		append_key(&path, k->name);
		if (sh == SH_SCALAR && cx->ncols < MAX_COLS)
		{
			add_item(out, psprintf("%s %s", path.data, scalar_type(k)));
			cx->ncols++;
		}
		else if (sh == SH_OBJECT)
			emit(cx, k, path.data, out);
		else if (sh == SH_ARRAY && k->elem != NULL)
		{
			Shape		es = shape_of(k->elem);

			if (es == SH_SCALAR && cx->ncols < MAX_COLS)
			{
				add_item(out, psprintf("%s[] %s", path.data, scalar_type(k->elem)));
				cx->ncols++;
			}
			else if (es == SH_OBJECT && cx->ngroups < MAX_GROUPS)
			{
				StringInfoData inner;
				int			cols = cx->ncols;

				initStringInfo(&inner);
				cx->ngroups++;
				emit(cx, k->elem, "", &inner);
				if (inner.len > 0)
					add_item(out, psprintf("%s[]{%s}", path.data, inner.data));
				else
				{
					cx->ngroups--;
					cx->ncols = cols;
				}
			}
		}
	}
}

Datum
sift_col_suggest_paths(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	text	   *colname = PG_GETARG_TEXT_PP(1);
	int			sample = PG_ARGISNULL(2) ? 2000 : PG_GETARG_INT32(2);
	char	   *col = text_to_cstring(colname);
	Relation	rel;
	AttrNumber	attno;
	Ctx			cx = {.tree = AllocSetContextCreate(CurrentMemoryContext, "sift suggest paths", ALLOCSET_DEFAULT_SIZES)};
	MemoryContext doc_cx = AllocSetContextCreate(CurrentMemoryContext, "sift suggest document", ALLOCSET_SMALL_SIZES);
	SNode	   *root;
	BlockNumber nblocks;
	int			nchunks;
	Snapshot	snap;
	TupleTableSlot *slot;
	StringInfoData out;
	char	   *result;

	if (sample < 1 || sample > 100000)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("sample must be between 1 and 100000")));
	rel = table_open(relid, AccessShareLock);
	if (rel->rd_rel->relkind != RELKIND_RELATION && rel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a table", RelationGetRelationName(rel))));
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(rel->rd_rel->relkind), RelationGetRelationName(rel));
	if (check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("\"%s\" has row-level security", RelationGetRelationName(rel)),
						errhint("Declare the paths by hand: the documents are not read past a policy.")));
	attno = get_attnum(relid, col);
	if (attno == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("column \"%s\" does not exist", col)));
	if (TupleDescAttr(RelationGetDescr(rel), attno - 1)->atttypid != JSONBOID)
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("column \"%s\" is not jsonb", col)));
	root = node_new(&cx, NULL);
	nblocks = RelationGetNumberOfBlocks(rel);
	nchunks = (int) Min(SAMPLE_CHUNKS, nblocks);
	snap = GetActiveSnapshot();
	slot = table_slot_create(rel, NULL);
	for (int c = 0; c < nchunks && cx.docs < (uint32) sample; c++)
	{
		BlockNumber start = (BlockNumber) ((uint64) c * nblocks / nchunks),
					len = Min(CHUNK_BLOCKS, Max(1, nblocks / nchunks));
		uint32		quota = (uint32) ((sample + nchunks - 1) / nchunks),
					got = 0;
		TableScanDesc scan = table_beginscan_strat(rel, snap, 0, NULL, true, false);

		heap_setscanlimits(scan, start, len);
		while (got < quota && table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			bool		isnull;
			Datum		d = slot_getattr(slot, attno, &isnull);
			MemoryContext old;
			Jsonb	   *jb;
			JsonbIterator *it;
			JsonbValue	v;

			CHECK_FOR_INTERRUPTS();
			if (isnull)
				continue;
			old = MemoryContextSwitchTo(doc_cx);
			jb = DatumGetJsonbP(d);
			it = JsonbIteratorInit(&jb->root);
			if (JsonbIteratorNext(&it, &v, false) == WJB_BEGIN_OBJECT)
			{
				root->seen++;
				root->nobj++;
				container(&cx, root, &it, true);
				cx.docs++;
				got++;
			}
			MemoryContextSwitchTo(old);
			MemoryContextReset(doc_cx);
		}
		table_endscan(scan);
	}
	ExecDropSingleTupleTableSlot(slot);
	initStringInfo(&out);
	{
		StringInfoData inner;

		initStringInfo(&inner);
		emit(&cx, root, col, &inner);
		result = inner.data;
	}
	if (result[0] != '\0')
	{
		/* the answer is a spec the option accepts: a spec the parser refuses is a bug here, not in the documents */
		(void) sift_shred_spec_parse(result);
	}
	table_close(rel, AccessShareLock);
	MemoryContextDelete(doc_cx);
	MemoryContextDelete(cx.tree);
	if (result[0] == '\0')
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(result));
}
