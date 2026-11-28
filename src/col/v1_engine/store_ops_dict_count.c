/*
 * col/v1_engine/store_ops_dict_count.c - SiftAggV6 answering count(*) of one test of a string leaf of a Format 1 index from the dictionaries of its segments (option count_columns).
 *
 * The dictionary of such a leaf keeps the rows of each string in byte order, so the rows of a segment that pass an equality, a range under the C collation or a LIKE prefix are two
 * bisections. A segment answers when every unit of it is exact (col/v1/scan/unit.c sift_col_unit_seg_count), once, at the first unit of it the source offers; its other units add
 * nothing. Any other unit goes to the next sink (the cubes) or is read, so an answer is the scan's.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/agg.h"
#include "agg/cube.h"
#include "agg/group/int.h"
#include "col/like/simd.h"
#include "col/source_fold.h"
#include "col/v1/store_ops.h"
#include "core/compat.h"

typedef struct DictCount
{
	SiftV1UnitSink sink;
	const SiftV1UnitSink *next;
	SiftV6Group *group;
	SiftV6ScanStats *stats;
	MemoryContext cx;
	int			leaf;
	SiftColOp	op;
	SiftColValue val;
	List	   *counted;		/* the segments answered */
	List	   *read;			/* the segments that are read */
} DictCount;

static bool
pass_on(DictCount *d, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	return d->next != NULL && d->next->offer(d->next->arg, rd, src, unit);
}

static bool
offer(void *arg, const SiftV1ReaderOps *rd, void *src, const SiftSrcUnit *unit)
{
	DictCount  *d = arg;
	const void *seg = unit->nsu == 0 ? rd->unit_seg(src, unit) : NULL;
	MemoryContext old;
	V6Acc		acc = {0};

	if (seg == NULL || list_member_ptr(d->read, seg) || !sift_v6_group_takes_cells(d->group))
		return pass_on(d, rd, src, unit);
	if (!list_member_ptr(d->counted, seg))
	{
		uint64		rows;
		const bool	ok = rd->seg_count(src, unit, d->leaf, d->op, &d->val, &rows);

		old = MemoryContextSwitchTo(d->cx);
		if (ok)
			d->counted = lappend(d->counted, (void *) seg);
		else
			d->read = lappend(d->read, (void *) seg);
		MemoryContextSwitchTo(old);
		if (!ok)
			return pass_on(d, rd, src, unit);
		acc.n = (int64) rows;
		sift_v6_group_merge_cells(d->group, NULL, &acc, 1);
	}
	d->stats->grans_preagg++;
	return true;
}

static SiftColOp
cmp_op(const OpExpr *op)
{
	static const struct
	{
		const char *name;
		SiftColOp	op;
	}			ops[] = {{"=", SIFT_COLOP_EQ}, {"<", SIFT_COLOP_LT}, {"<=", SIFT_COLOP_LE}, {">", SIFT_COLOP_GT}, {">=", SIFT_COLOP_GE}};
	const char *name = get_opname(op->opno);

	for (int i = 0; name != NULL && i < lengthof(ops); i++)
		if (strcmp(name, ops[i].name) == 0)
			return ops[i].op == SIFT_COLOP_EQ || sift_collate_is_c(op->inputcollid) ? ops[i].op : 0;
	return 0;
}

/* A text column of the heap, under the collation a COLLATE clause gives it */
static const Var *
text_var(const Node *n)
{
	const Var  *v;

	if (n != NULL && IsA(n, RelabelType) && ((const RelabelType *) n)->resulttype == TEXTOID)
		n = (const Node *) ((const RelabelType *) n)->arg;
	v = (const Var *) n;
	return n != NULL && IsA(n, Var) && v->varno == 1 && v->varlevelsup == 0 && v->varattno > 0 && v->vartype == TEXTOID ? v : NULL;
}

const SiftSrcLeaf *
sift_v1_heap_att_leaf(Relation index, const SiftSrcLayout *lay, int att)
{
	for (int i = 0; i < index->rd_index->indnatts; i++)
		if (index->rd_index->indkey.values[i] == att)
			return sift_v1_att_leaf(lay, i + 1);
	return NULL;
}

/* The test of the clause on the bytes of the strings of one leaf: an equality, a range in the C collation, or a LIKE whose pattern is a literal, a literal and %, or a literal between two % */
static bool
bind_test(DictCount *d, Relation host, const SiftSrcLayout *lay, const Node *clause)
{
	const OpExpr *op = (const OpExpr *) clause;
	const Var  *var;
	const Const *k;
	const SiftSrcLeaf *l;
	const text *t;
	uint8	   *lit;
	uint32		len;

	if (!IsA(clause, OpExpr) || list_length(op->args) != 2 || (var = text_var(linitial(op->args))) == NULL)
		return false;
	k = lsecond(op->args);
	if (!IsA(k, Const) || k->constisnull || k->consttype != TEXTOID || !OidIsValid(op->inputcollid) || !get_collation_isdeterministic(op->inputcollid))
		return false;
	l = sift_v1_heap_att_leaf(host, lay, var->varattno);
	if (l == NULL || l->wide || l->lossy || l->astext || l->vtype != SCV_CODE)
		return false;
	d->leaf = l->id;
	t = DatumGetTextPP(k->constvalue);
	len = VARSIZE_ANY_EXHDR(t);
	if (get_opcode(op->opno) != F_TEXTLIKE)
	{
		d->val = (SiftColValue) {.v.s = VARDATA_ANY(t),.len = len};
		return (d->op = cmp_op(op)) != 0;
	}
	lit = palloc(len + 1);
	switch (sift_like_parse((const uint8 *) VARDATA_ANY(t), len, lit, &len))
	{
		case SIFT_LIKE_PREFIX:
			d->op = SIFT_COLOP_PREFIX;
			break;
		case SIFT_LIKE_EQUAL:
			d->op = SIFT_COLOP_EQ;
			break;
		case SIFT_LIKE_CONTAINS:
			d->op = SIFT_COLOP_CONTAINS;
			break;
		default:
			return false;
	}
	d->val = (SiftColValue) {.v.s = (const char *) lit,.len = len};
	return true;
}

static bool
holds(const SiftColValue *hay, const SiftColValue *needle)
{
	for (uint32 i = 0; i + needle->len <= hay->len; i++)
		if (memcmp(hay->v.s + i, needle->v.s, needle->len) == 0)
			return true;
	return false;
}

/*
 * The clause adds nothing to the test d holds: the same test (the index qual and the clause of the batch of one WHERE condition are both there), or a LIKE '%lit%' that every string
 * the equality or the prefix of d passes holds (the scan key of the trigram signatures the index makes of a LIKE 'lit%')
 */
static bool
implied(const DictCount *d, Relation host, const SiftSrcLayout *lay, const Node *clause)
{
	DictCount	o = {0};

	if (!bind_test(&o, host, lay, clause) || o.leaf != d->leaf)
		return false;
	if (o.op == SIFT_COLOP_CONTAINS)
		return (d->op == SIFT_COLOP_EQ || d->op == SIFT_COLOP_PREFIX) && holds(&d->val, &o.val);
	return o.op == d->op && o.val.len == d->val.len && memcmp(o.val.v.s, d->val.v.s, o.val.len) == 0;
}

bool
sift_v1_count_only(const SiftSrcFoldArgs *a, const SiftV1Scan *scan)
{
	const SiftV6Spec *spec = a->spec;

	return a->host->rd_index != NULL && a->emit == NULL && a->star == NULL && a->gate == NULL && a->stats != NULL && sift_v6_spec_cubeable(spec) && spec->nkeys == 0 &&
		spec->naggs == 1 && spec->agg[0].kind == SV6_COUNT_STAR && spec->agg[0].fclause == 0 &&
		(scan == NULL || (scan->keep == NULL && scan->bridge == NULL && scan->ntypes == 0 && !scan->tid_range));
}

SiftV1UnitSink *
sift_v1_dict_count_begin(const SiftSrcFoldArgs *a, const SiftV1Scan *scan, MemoryContext cx, SiftV1UnitSink *next)
{
	const SiftV6Spec *spec = a->spec;
	const SiftV1ReaderOps *rd = sift_v1_reader_of(a->host->rd_index == NULL);
	const SiftSrcLayout *lay;
	List	   *tests = list_concat_copy(spec->vqual, a->quals);
	DictCount  *d;
	MemoryContext old;
	bool		bound = tests != NIL;

	if (rd->seg_count == NULL || !sift_v1_count_only(a, scan))
		return next;
	d = MemoryContextAllocZero(cx, sizeof(DictCount));
	old = MemoryContextSwitchTo(cx);
	lay = rd->layout(a->host);
	foreach_ptr(Node, t, tests)
		bound = bound && (foreach_current_index(t) == 0 ? bind_test(d, a->host, lay, t) : implied(d, a->host, lay, t));
	MemoryContextSwitchTo(old);
	if (!bound || d->op == SIFT_COLOP_CONTAINS)
		return next;
	d->next = next;
	d->group = a->group;
	d->stats = a->stats;
	d->cx = cx;
	d->sink = (SiftV1UnitSink) {.offer = offer,.arg = d};
	return &d->sink;
}
