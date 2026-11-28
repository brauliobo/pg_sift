/*
 * col/v1_engine/store_ops_range_count.c - SiftAggV6 answering count(*) of a range of one leaf of a Format 1 index from the global orders of its segments (option order_array_columns).
 *
 * The order of a leaf keeps the key and the heap address of every row of a segment sorted by the key (the ranks of the strings, for a text leaf), so the rows of a segment in a range
 * are the entries between two bisections. A segment answers once, at the first unit of it the source offers, for its units that are exact (sift_col_unit_seg_range); its other units, and
 * every unit of a segment that cannot answer, go to the next sink or are read, so an answer is the scan's.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "common/int.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "agg/group/int.h"
#include "col/source_fold.h"
#include "col/v1/reader.h"
#include "col/v1/store_ops.h"

typedef struct SegCount
{
	const void *seg;
	bool		counted;
	Bitmapset  *read;			/* the superblocks of its units that are read */
} SegCount;

typedef struct RangeCount
{
	SiftV1UnitSink sink;
	const SiftV1UnitSink *next;
	SiftV6Group *group;
	SiftV6ScanStats *stats;
	MemoryContext cx;
	int			leaf;
	SiftColRange range;
	List	   *segs;
} RangeCount;

static bool
pass_on(RangeCount *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	return c->next != NULL && c->next->offer(c->next->arg, rd, src, unit);
}

static SegCount *
seg_count(RangeCount *c, const void *seg)
{
	foreach_ptr(SegCount, s, c->segs)
		if (s->seg == seg)
			return s;
	return NULL;
}

/* The segment of the unit counted (its exact units) the first time one of its units comes */
static SegCount *
seg_counted(RangeCount *c, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit, const void *seg)
{
	MemoryContext old = MemoryContextSwitchTo(c->cx);
	SegCount   *s = palloc0(sizeof(SegCount));
	uint64		rows;
	V6Acc		acc = {0};

	s->seg = seg;
	s->counted = rd->seg_range_count(src, unit, c->leaf, &c->range, c->cx, &rows, &s->read);
	c->segs = lappend(c->segs, s);
	MemoryContextSwitchTo(old);
	if (s->counted)
	{
		acc.n = (int64) rows;
		sift_v6_group_merge_cells(c->group, NULL, &acc, 1);
	}
	return s;
}

static bool
offer(void *arg, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	RangeCount *c = arg;
	const void *seg = unit->nsu == 0 ? rd->unit_seg(src, unit) : NULL;
	SegCount   *s = seg != NULL ? seg_count(c, seg) : NULL;

	if (seg == NULL || (s == NULL && !sift_v6_group_takes_cells(c->group)))
		return pass_on(c, rd, src, unit);
	if (s == NULL)
		s = seg_counted(c, rd, src, unit, seg);
	if (!s->counted || bms_is_member((int) unit->sb, s->read))
		return pass_on(c, rd, src, unit);
	c->stats->grans_preagg++;
	return true;
}

static bool
int_type(Oid t)
{
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

/* The key the leaf keeps for the constant: the integer, the days of a date, the microseconds of a timestamp; false for a type of another class than the column's */
static bool
key_of(const Const *k, Oid coltype, int64 *key)
{
	if (int_type(coltype) && int_type(k->consttype))
		*key = k->consttype == INT2OID ? DatumGetInt16(k->constvalue) : k->consttype == INT4OID ? DatumGetInt32(k->constvalue) : DatumGetInt64(k->constvalue);
	else if (k->consttype != coltype)
		return false;
	else if (coltype == DATEOID)
		*key = DatumGetDateADT(k->constvalue);
	else if (coltype == TIMESTAMPOID || coltype == TIMESTAMPTZOID)
		*key = DatumGetTimestamp(k->constvalue);
	else
		return false;
	return true;
}

static void
empty_range(SiftColRange *r)
{
	r->lo = PG_INT64_MAX;
	r->hi = PG_INT64_MIN;
}

/* [lo, hi] narrowed by the comparison of strategy s with key x; a bound past the ends of int64 empties it for good (lo > hi) */
static void
int_bound(SiftColRange *r, int s, int64 x)
{
	int64		y;

	switch (s)
	{
		case BTLessStrategyNumber:
			if (pg_sub_s64_overflow(x, 1, &y))
				empty_range(r);
			else
				r->hi = Min(r->hi, y);
			break;
		case BTGreaterStrategyNumber:
			if (pg_add_s64_overflow(x, 1, &y))
				empty_range(r);
			else
				r->lo = Max(r->lo, y);
			break;
		case BTLessEqualStrategyNumber:
			r->hi = Min(r->hi, x);
			break;
		case BTGreaterEqualStrategyNumber:
			r->lo = Max(r->lo, x);
			break;
		default:
			r->lo = Max(r->lo, x);
			r->hi = Min(r->hi, x);
			break;
	}
}

/* A bound of a text range: one on each side, or the same twice (the index qual and the clause of the batch of one WHERE condition) */
static bool
str_side(const char **p, uint32 *len, bool *excl, const text *t, bool x)
{
	const char *s = VARDATA_ANY(t);
	const uint32 n = VARSIZE_ANY_EXHDR(t);

	if (*p != NULL)
		return *len == n && *excl == x && memcmp(*p, s, n) == 0;
	*p = pnstrdup(s, n);
	*len = n;
	*excl = x;
	return true;
}

static bool
str_bound(SiftColRange *r, int s, const text *t, Oid collid)
{
	if (r->collid != InvalidOid && r->collid != collid)
		return false;
	r->collid = collid;
	if ((s == BTLessStrategyNumber || s == BTLessEqualStrategyNumber || s == BTEqualStrategyNumber) && !str_side(&r->shi, &r->shilen, &r->shi_excl, t, s == BTLessStrategyNumber))
		return false;
	return !(s == BTGreaterStrategyNumber || s == BTGreaterEqualStrategyNumber || s == BTEqualStrategyNumber) || str_side(&r->slo, &r->slolen, &r->slo_excl, t, s == BTGreaterStrategyNumber);
}

static const Var *
column_of(const Node *n)
{
	if (n != NULL && IsA(n, RelabelType))
		n = (const Node *) ((const RelabelType *) n)->arg;
	return n != NULL && IsA(n, Var) && ((const Var *) n)->varno == 1 && ((const Var *) n)->varlevelsup == 0 && ((const Var *) n)->varattno > 0 ? (const Var *) n : NULL;
}

/* The leaf of the column holds its values as they are: a scalar of the type of the column, of the integer class or text */
static bool
leaf_ok(const SiftSrcLeaf *l, Oid type, bool str)
{
	return l != NULL && l->kind == SCL_SCALAR && l->parent < 0 && l->path == NIL && !l->wide && !l->lossy && !l->astext && !l->scope && l->typid == type &&
		(str ? l->vtype == SCV_CODE : l->vtype == SCV_I32 || l->vtype == SCV_I64);
}

/* `column op constant` with a btree comparison of the column's type, on the leaf of the clauses before it */
static bool
bind_test(RangeCount *c, Relation host, const SiftSrcLayout *lay, const Node *clause)
{
	const OpExpr *op = (const OpExpr *) clause;
	const Var  *var;
	const Const *k;
	const SiftSrcLeaf *l;
	int			s;
	int64		x;

	if (!IsA(clause, OpExpr) || list_length(op->args) != 2 || (var = column_of(linitial(op->args))) == NULL || !IsA(lsecond(op->args), Const))
		return false;
	k = lsecond(op->args);
	c->range.str = var->vartype == TEXTOID;
	l = sift_v1_heap_att_leaf(host, lay, var->varattno);
	if (k->constisnull || !leaf_ok(l, var->vartype, c->range.str) || (c->leaf >= 0 && c->leaf != l->id))
		return false;
	c->leaf = l->id;
	s = get_op_opfamily_strategy(op->opno, get_opclass_family(GetDefaultOpClass(var->vartype, BTREE_AM_OID)));
	if (s == 0)
		return false;
	if (c->range.str)
		return k->consttype == TEXTOID && OidIsValid(op->inputcollid) && get_collation_isdeterministic(op->inputcollid) &&
			str_bound(&c->range, s, DatumGetTextPP(k->constvalue), op->inputcollid);
	if (!key_of(k, var->vartype, &x))
		return false;
	int_bound(&c->range, s, x);
	return true;
}

SiftV1UnitSink *
sift_v1_range_count_begin(const SiftSrcFoldArgs *a, const SiftV1Scan *scan, MemoryContext cx, SiftV1UnitSink *next)
{
	const SiftV1ReaderOps *rd = sift_v1_reader_of(a->host->rd_index == NULL);
	const SiftSrcLayout *lay;
	List	   *tests;
	RangeCount *c;
	MemoryContext old;
	bool		bound;

	if (rd->seg_range_count == NULL || !sift_v1_count_only(a, scan))
		return next;
	c = MemoryContextAllocZero(cx, sizeof(RangeCount));
	c->leaf = -1;
	c->range = (SiftColRange) {.lo = PG_INT64_MIN,.hi = PG_INT64_MAX};
	old = MemoryContextSwitchTo(cx);
	lay = rd->layout(a->host);
	tests = list_concat_copy(a->spec->vqual, a->quals);
	bound = tests != NIL;
	foreach_ptr(Node, t, tests)
		bound = bound && bind_test(c, a->host, lay, t);
	MemoryContextSwitchTo(old);
	if (!bound)
		return next;
	c->next = next;
	c->group = a->group;
	c->stats = a->stats;
	c->cx = cx;
	c->sink = (SiftV1UnitSink) {.offer = offer,.arg = c};
	return &c->sink;
}
