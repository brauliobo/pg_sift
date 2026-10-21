/*
 * agg/stub.c - the sift_colstub index access method and the definition (layout) of its indexes.
 *
 * A SHELL: it has no storage. The index only carries the definition (key columns, expressions, the `paths` reloption) that
 * a sift_col index will carry, and agg/stub_src.c reads the heap pages raw to present the units of agg/colsrc.h, so
 * the aggregate planner and executor can be developed and tested before sift_col exists. CREATE INDEX and INSERT validate
 * the definition and the values exactly where sift_col will (types, `paths`, 255 bytes of text, a jsonb value that cannot
 * be cast to its declared type), VACUUM and the planner cost do nothing: the index is never used by an ordinary plan.
 *
 * `paths` (a jsonb key column must be a plain column of the table, and has to be declared there):
 *     attname: item, item, ...; attname: item, ...
 *     item  = a.b.c type              a scalar leaf at the jsonb path [a, b, c]
 *           | a.b[]{f1 type, f2 type} an array of objects at [a, b]: a count leaf and one child leaf per field
 *           | []{f1 type, ...}        the same for a column that is an array at its root
 *           | *:type                  (the only item of its column) a root object with free keys: a count leaf and the
 *                                     children "key" (text) and "value" (type)
 *     type  = int4, int8, float8, bool, date, timestamp or text (names as PostgreSQL's, e.g. "double precision")
 * A name may be double-quoted. A path is read like `->`: object keys only, an array on the way gives NULL. A value of another
 * JSON type than declared is converted as `(x #>> '{}')::type` is, and raises the error that cast raises.
 *
 * Deviations from, and choices inside, the contract of agg/colsrc.h (see agg/stub_src.c for the source side):
 *  - A group leaf has typid int4 (its counts). An absent array (missing key, JSON null, not an array) has a NULL count.
 *  - A text value of a char(n) column is the stored (blank padded) text.
 *  - The stub has no INSERT-time copy of the values: a failed INSERT (a text over 255 bytes, a bad cast) leaves an aborted
 *    heap tuple, which the raw read presents like any row and the decode raises the same error for, until VACUUM removes it.
 *  - A concurrent VACUUM can remove the TOAST values of a dead row between the raw read and the detoast ("missing chunk").
 *  - The zone prune entries of sift_colsrc_begin are ANDed: a unit is skipped when any entry excludes it.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/amapi.h"
#include "access/reloptions.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "nodes/makefuncs.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "agg/stub.h"
#include "bm25/am.h"

PG_FUNCTION_INFO_V1(sift_aggstub_handler);

typedef struct StubOptions
{
	int32		vl_len_;
	int			paths;			/* offset of the string, 0: none */
} StubOptions;

static relopt_kind stub_relopt_kind;
static Oid	stub_am_oid = InvalidOid;

/* ---------------------------------------------------------------------------
 * The `paths` grammar
 * ------------------------------------------------------------------------- */

typedef struct StubItem
{
	SiftSrcLeafKind kind;
	List	   *path;			/* String nodes; NIL: the root of the column */
	Oid			typid;			/* scalar leaf, the values of an object */
	List	   *fnames;			/* array: String nodes, one per field */
	List	   *ftypes;			/* array: Oids */
} StubItem;

typedef struct StubSpec
{
	char	   *attname;
	List	   *items;
	bool		used;
} StubSpec;

typedef struct StubParse
{
	const char *s;
	const char *p;
} StubParse;

static const struct
{
	const char *name;
	Oid			typid;
} stub_path_types[] = {
	{"int4", INT4OID}, {"integer", INT4OID}, {"int", INT4OID},
	{"int8", INT8OID}, {"bigint", INT8OID},
	{"float8", FLOAT8OID}, {"double precision", FLOAT8OID},
	{"bool", BOOLOID}, {"boolean", BOOLOID},
	{"date", DATEOID}, {"timestamp", TIMESTAMPOID}, {"text", TEXTOID},
};

static void
parse_fail(const StubParse *ps, const char *what)
{
	ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("invalid sift_colstub paths: %s at position %d", what, (int) (ps->p - ps->s) + 1)));
}

static bool
is_ws(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void
skip_ws(StubParse *ps)
{
	while (is_ws(*ps->p))
		ps->p++;
}

static bool
accept(StubParse *ps, char c)
{
	skip_ws(ps);
	if (*ps->p != c)
		return false;
	ps->p++;
	return true;
}

static void
expect(StubParse *ps, char c)
{
	if (!accept(ps, c))
		parse_fail(ps, psprintf("\"%c\" expected", c));
}

/* A name: bare (no blank and none of . , ; : { } [ ] * ") or double-quoted with "" for a quote */
static char *
ident(StubParse *ps)
{
	StringInfoData buf;

	skip_ws(ps);
	initStringInfo(&buf);
	if (*ps->p == '"')
	{
		for (ps->p++; *ps->p != '"' || ps->p[1] == '"'; ps->p++)
		{
			if (!*ps->p)
				parse_fail(ps, "unterminated quoted name");
			appendStringInfoChar(&buf, *ps->p);
			ps->p += (*ps->p == '"');
		}
		ps->p++;
	}
	else
		for (; *ps->p && !strchr(" \t\r\n.,;:{}[]*\"", *ps->p); ps->p++)
			appendStringInfoChar(&buf, *ps->p);
	if (buf.len == 0)
		parse_fail(ps, "name expected");
	if (buf.len >= NAMEDATALEN)
		parse_fail(ps, "name too long");
	return buf.data;
}

static Oid
type_name(StubParse *ps)
{
	const char *start;
	char	   *name;
	int			len;

	skip_ws(ps);
	start = ps->p;
	while (*ps->p && !strchr(",;}", *ps->p))
		ps->p++;
	for (len = ps->p - start; len > 0 && is_ws(start[len - 1]); len--)
		;
	if (len == 0)
		parse_fail(ps, "type expected");
	name = pnstrdup(start, len);
	for (char *c = name; *c; c++)
		*c = pg_tolower((unsigned char) *c);
	for (int i = 0; i < lengthof(stub_path_types); i++)
		if (strcmp(name, stub_path_types[i].name) == 0)
			return stub_path_types[i].typid;
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("sift_colstub paths: type \"%s\" is not supported", name),
					errhint("Use int4, int8, float8, bool, date, timestamp or text.")));
}

static StubItem *
parse_item(StubParse *ps)
{
	StubItem   *it = palloc0(sizeof(StubItem));

	if (accept(ps, '*'))
	{
		expect(ps, ':');
		it->kind = SCL_OBJECT;
		it->typid = type_name(ps);
		return it;
	}
	skip_ws(ps);
	while (*ps->p != '[')
	{
		it->path = lappend(it->path, makeString(ident(ps)));
		if (*ps->p != '.')
			break;
		ps->p++;
	}
	if (!accept(ps, '['))
	{
		if (it->path == NIL)
			parse_fail(ps, "path expected");
		it->kind = SCL_SCALAR;
		it->typid = type_name(ps);
		return it;
	}
	expect(ps, ']');
	expect(ps, '{');
	it->kind = SCL_ARRAY;
	do
	{
		it->fnames = lappend(it->fnames, makeString(ident(ps)));
		it->ftypes = lappend_oid(it->ftypes, type_name(ps));
	} while (accept(ps, ','));
	expect(ps, '}');
	return it;
}

static List *
parse_paths(const char *s)
{
	StubParse	ps = {s, s};
	List	   *specs = NIL;

	for (;;)
	{
		StubSpec   *sp = palloc0(sizeof(StubSpec));

		sp->attname = ident(&ps);
		foreach_ptr(StubSpec, other, specs)
			if (strcmp(other->attname, sp->attname) == 0)
				parse_fail(&ps, psprintf("column \"%s\" declared twice", sp->attname));
		expect(&ps, ':');
		do
			sp->items = lappend(sp->items, parse_item(&ps));
		while (accept(&ps, ','));
		foreach_ptr(StubItem, it, sp->items)
			if (it->kind == SCL_OBJECT && list_length(sp->items) > 1)
				parse_fail(&ps, "*:type has to be the only item of its column");
		specs = lappend(specs, sp);
		if (!accept(&ps, ';') || (skip_ws(&ps), !*ps.p))
			break;
	}
	skip_ws(&ps);
	if (*ps.p)
		parse_fail(&ps, "unexpected text");
	return specs;
}

static void
validate_paths(const char *value)
{
	if (value)					/* the default is checked too */
		(void) parse_paths(value);
}

/* ---------------------------------------------------------------------------
 * The layout of an index
 * ------------------------------------------------------------------------- */

static SiftSrcVecType
vtype_of(Oid typid, bool *known)
{
	*known = true;
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
		case BOOLOID:
			return SCV_I32;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return SCV_I64;
		case FLOAT4OID:
		case FLOAT8OID:
			return SCV_F64;
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case NAMEOID:
			return SCV_CODE;
	}
	*known = false;
	return SCV_I32;
}

static int
add_leaf(StubLayout *lay, int attno, int parent, SiftSrcLeafKind kind, Oid typid, List *path, const char *name,
		 StubRole role, bool from_json)
{
	int			id = lay->l.nleaves;
	SiftSrcLeaf *lf = &lay->l.leaf[id];
	StubLeafX  *x = &lay->x[id];
	bool		known;
	Oid			infn;

	if (id >= SIFT_COLSRC_MAX_LEAVES)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("sift_colstub: an index has at most %d leaves", SIFT_COLSRC_MAX_LEAVES)));
	lf->id = id;
	lf->attno = attno;
	lf->parent = parent;
	lf->kind = kind;
	lf->typid = kind == SCL_SCALAR ? typid : INT4OID;
	lf->vtype = kind == SCL_SCALAR ? vtype_of(typid, &known) : SCV_I32;
	lf->path = path;
	if (name)
		strlcpy(lf->name, name, NAMEDATALEN);
	x->role = role;
	x->namelen = name ? strlen(name) : 0;
	if (from_json && kind == SCL_SCALAR && typid != TEXTOID)
	{
		getTypeInputInfo(typid, &infn, &x->ioparam);
		fmgr_info(infn, &x->infn);
	}
	lay->l.nleaves++;
	return id;
}

static void
add_item_leaves(StubLayout *lay, int attno, const StubItem *it)
{
	int			grp;

	if (it->kind == SCL_SCALAR)
	{
		add_leaf(lay, attno, -1, SCL_SCALAR, it->typid, it->path, NULL, STUB_ROLE_VALUE, true);
		return;
	}
	grp = add_leaf(lay, attno, -1, it->kind, InvalidOid, it->path, NULL, STUB_ROLE_VALUE, true);
	if (it->kind == SCL_OBJECT)
	{
		add_leaf(lay, attno, grp, SCL_SCALAR, TEXTOID, NIL, "key", STUB_ROLE_KEY, true);
		add_leaf(lay, attno, grp, SCL_SCALAR, it->typid, NIL, "value", STUB_ROLE_PAIR, true);
		return;
	}
	for (int i = 0; i < list_length(it->fnames); i++)
		add_leaf(lay, attno, grp, SCL_SCALAR, list_nth_oid(it->ftypes, i), NIL, strVal(list_nth(it->fnames, i)),
				 STUB_ROLE_FIELD, true);
}

static const char *
index_paths(Relation index)
{
	const StubOptions *opts = (const StubOptions *) index->rd_options;

	return opts && opts->paths ? (const char *) opts + opts->paths : NULL;
}

static StubSpec *
find_spec(List *specs, const char *attname)
{
	foreach_ptr(StubSpec, sp, specs)
		if (strcmp(sp->attname, attname) == 0)
			return sp;
	return NULL;
}

/* The expression of key attribute i (0-based) over the heap row, varno 1 */
static Node *
att_expr(Relation index, int i, List *exprs, int *nexpr)
{
	AttrNumber	heapatt = index->rd_index->indkey.values[i];
	Oid			typid;
	int32		typmod;
	Oid			coll;

	if (heapatt == 0)
	{
		return list_nth(exprs, (*nexpr)++);
	}
	get_atttypetypmodcoll(index->rd_index->indrelid, heapatt, &typid, &typmod, &coll);
	return (Node *) makeVar(1, heapatt, typid, typmod, coll, 0);
}

StubLayout *
sift_aggstub_layout(Relation index)
{
	StubLayout *lay = palloc0(sizeof(StubLayout));
	const char *paths = index_paths(index);
	List	   *specs = paths ? parse_paths(paths) : NIL;
	List	   *exprs = RelationGetIndexExpressions(index);
	int			nexpr = 0;
	int			natts = index->rd_index->indnatts;

	if (!sift_aggstub_owns_index(index->rd_rel->relam))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a " SIFT_COLSTUB_AM " index", RelationGetRelationName(index))));
	lay->l.heapoid = index->rd_index->indrelid;
	lay->l.is_table = false;
	lay->l.natts = natts;
	lay->l.leaf = palloc0(sizeof(SiftSrcLeaf) * SIFT_COLSRC_MAX_LEAVES);
	for (int i = 0; i < natts; i++)
	{
		Oid			typid = TupleDescAttr(index->rd_att, i)->atttypid;
		bool		known;
		StubSpec   *sp = NULL;

		lay->l.attexprs = lappend(lay->l.attexprs, att_expr(index, i, exprs, &nexpr));
		if (typid == JSONBOID)
		{
			AttrNumber	heapatt = index->rd_index->indkey.values[i];

			if (heapatt != 0)
				sp = find_spec(specs, get_attname(lay->l.heapoid, heapatt, false));
			if (!sp)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg(SIFT_COLSTUB_AM " index column %d is jsonb: declare its paths in the \"paths\" option of the index", i + 1),
								errhint("Only a plain column can have paths.")));
			sp->used = true;
			foreach_ptr(StubItem, it, sp->items)
				add_item_leaves(lay, i + 1, it);
			continue;
		}
		(void) vtype_of(typid, &known);
		if (!known)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg(SIFT_COLSTUB_AM " does not support the type %s of index column %d", format_type_be(typid), i + 1)));
		add_leaf(lay, i + 1, -1, SCL_SCALAR, typid, NIL, NULL, STUB_ROLE_VALUE, false);
	}
	foreach_ptr(StubSpec, sp, specs)
		if (!sp->used)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
							errmsg("sift_colstub paths: \"%s\" is not a jsonb column of the index", sp->attname)));
	return lay;
}

/* ---------------------------------------------------------------------------
 * The access method
 * ------------------------------------------------------------------------- */

typedef struct StubBuild
{
	const StubLayout *lay;
	double		ntuples;
} StubBuild;

static void
stub_build_callback(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool tupleIsAlive, void *state)
{
	StubBuild  *b = state;

	sift_aggstub_check_row(b->lay, values, isnull);
	b->ntuples++;
}

static IndexBuildResult *
stub_ambuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	StubBuild	b = {NULL, 0};
	IndexBuildResult *result = palloc0(sizeof(IndexBuildResult));

	if (indexInfo->ii_Predicate != NIL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg(SIFT_COLSTUB_AM " does not support partial indexes")));
	b.lay = sift_aggstub_layout(index);
	table_index_build_scan(heap, index, indexInfo, true, true, stub_build_callback, &b, NULL);
	result->heap_tuples = result->index_tuples = b.ntuples;
	return result;
}

static void
stub_ambuildempty(Relation index)
{
}

static bool
stub_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
			  IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	if (!indexInfo->ii_AmCache)
	{
		MemoryContext old = MemoryContextSwitchTo(indexInfo->ii_Context);

		indexInfo->ii_AmCache = sift_aggstub_layout(index);
		MemoryContextSwitchTo(old);
	}
	sift_aggstub_check_row(indexInfo->ii_AmCache, values, isnull);
	return false;
}

static IndexBulkDeleteResult *
stub_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *state)
{
	return stats ? stats : palloc0(sizeof(IndexBulkDeleteResult));
}

static IndexBulkDeleteResult *
stub_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (!stats)
		stats = palloc0(sizeof(IndexBulkDeleteResult));
	stats->num_index_tuples = info->num_heap_tuples;
	stats->estimated_count = true;
	return stats;
}

/* No ordinary plan may choose the index (it has no operators, so none is offered one; this is the net) */
static void
stub_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost,
					Cost *indexTotalCost, Selectivity *indexSelectivity, double *indexCorrelation, double *indexPages)
{
	*indexStartupCost = *indexTotalCost = 1.0e30;
	*indexSelectivity = 1.0;
	*indexCorrelation = 0.0;
	*indexPages = 0.0;
}

static bytea *
stub_amoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {{"paths", RELOPT_TYPE_STRING, offsetof(StubOptions, paths)}};

	return (bytea *) build_reloptions(reloptions, validate, stub_relopt_kind, sizeof(StubOptions), tab, lengthof(tab));
}

static bool
stub_amvalidate(Oid opclassoid)
{
	return true;
}

static IndexScanDesc
stub_ambeginscan(Relation index, int nkeys, int norderbys)
{
	return RelationGetIndexScan(index, nkeys, norderbys);
}

static void
stub_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
}

static int64
stub_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg(SIFT_COLSTUB_AM " indexes cannot be scanned")));
}

static void
stub_amendscan(IndexScanDesc scan)
{
}

Datum
sift_aggstub_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = sift_am_routine(0, 0, true);

	am->amcanbuildparallel = am->amusemaintenanceworkmem = false;
	am->amparallelvacuumoptions = 0;
	am->ambuild = stub_ambuild;
	am->ambuildempty = stub_ambuildempty;
	am->aminsert = stub_aminsert;
	am->ambulkdelete = stub_ambulkdelete;
	am->amvacuumcleanup = stub_amvacuumcleanup;
	am->amcostestimate = stub_amcostestimate;
	am->amoptions = stub_amoptions;
	am->amvalidate = stub_amvalidate;
	am->ambeginscan = stub_ambeginscan;
	am->amrescan = stub_amrescan;
	am->amgetbitmap = stub_amgetbitmap;
	am->amendscan = stub_amendscan;
	PG_RETURN_POINTER(am);
}

/* ---------------------------------------------------------------------------
 * The source's entry points that need only the definition
 * ------------------------------------------------------------------------- */

static void
am_oid_invalidate(Datum arg, int cacheid, uint32 hashvalue)
{
	stub_am_oid = InvalidOid;
}

static Oid
am_oid(void)
{
	if (!OidIsValid(stub_am_oid))
		stub_am_oid = get_index_am_oid(SIFT_COLSTUB_AM, true);
	return stub_am_oid;
}

void
sift_aggstub_init(void)
{
	stub_relopt_kind = add_reloption_kind();
	add_string_reloption(stub_relopt_kind, "paths", "jsonb paths of the key columns: attname: a.b type, a.b[]{f type}, *:type; ...",
						 NULL, validate_paths, AccessExclusiveLock);
	CacheRegisterSyscacheCallback(AMOID, am_oid_invalidate, (Datum) 0);
	sift_aggstub_register();
}

bool
sift_aggstub_owns_index(Oid indexrelam)
{
	return OidIsValid(indexrelam) && indexrelam == am_oid();
}

#endif							/* SIFT_TESTING */
