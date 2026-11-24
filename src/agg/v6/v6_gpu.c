/*
 * agg/v6/v6_gpu.c - the query of SiftAggV6 as the device is asked it: the quals of the scan (the forms agg/v6/plan.c keeps: column op constant, column IN constants, IS [NOT] NULL) as one
 * program of agg/vexpr.h and IN sets, the columns of the keys and the aggregates as slots of the leaves of the store, and the parts the device read as the generations the CPU scan leaves out.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/primnodes.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/gpu_cfg.h"
#include "agg/gpu_dict_agg.h"
#include "agg/gpu_v6.h"
#include "agg/v6/v6_gpu.h"
#include "agg/v6/v6_topn.h"
#include "core/compat.h"

typedef struct GpuQuery
{
	SiftGpuQuery q;
	SiftGpuIn	in[SIFT_VK_MAXCOLS];
	int			in_attno[SIFT_VK_MAXCOLS];
	SiftVexprProg where;
	SiftVexprProg sort;
	SiftVexprProg filter[SIFT_VK_MAXAGGS];
	SiftVexprProg key[SIFT_VK_MAXKEYS];
	SiftGpuDictQual dict;		/* the comparisons of a text column in the conjuncts of the scan */
	bool		zones;			/* the clause being compiled is a conjunct of the scan: its comparisons are zone tests too */
	MemoryContext cx;
	Relation	index;
	SiftV6Input *tin;			/* a top-N: the clauses are the table's, and this maps its attributes to those of the index; NULL: the clauses are already the index's */
} GpuQuery;

static int64 element_value(Oid elemtype, Datum d);

static int
slot_of_attno(GpuQuery *g, int attno)
{
	SiftGpuQuery *q = &g->q;
	const int	leaf = q->sch.att_first[attno];

	for (int i = 0; i < q->ncols; i++)
		if (q->leaf[i] == leaf)
			return i;
	if (leaf < 0 || q->ncols == SIFT_VK_MAXCOLS)
		return -1;
	q->leaf[q->ncols] = leaf;
	return q->ncols++;
}

static bool
plain_column(const SiftV6Col *c)
{
	return c->derive == 0 && c->jleaf == 0;
}

static bool
device_shape(const SiftV6Spec *s)
{
	if (s->nsets > 0 || s->nfuncs > 0 || s->naggs == 0 || s->jsonqual != NIL)
		return false;
	for (int i = 0; i < s->nkeys; i++)
		if (!plain_column(&s->key[i]))
			return false;
	for (int j = 0; j < s->naggs; j++)
		if (s->agg[j].nxcols > 0 || !plain_column(&s->agg[j].arg))
			return false;
	return true;
}

typedef enum Domain
{
	DOM_NONE,
	DOM_INT,
	DOM_DATE,
	DOM_TS,
	DOM_TSTZ,
	DOM_F64
} Domain;

static Domain
domain_of(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
			return DOM_INT;
		case DATEOID:
			return DOM_DATE;
		case TIMESTAMPOID:
			return DOM_TS;
		case TIMESTAMPTZOID:
			return DOM_TSTZ;
		case FLOAT4OID:
		case FLOAT8OID:
			return DOM_F64;
		default:
			return DOM_NONE;
	}
}

/* the zones of these hold the values the device compares: whole numbers, and dates and instants as the days and microseconds they are stored as */
static bool
zone_domain(Domain dom)
{
	return dom == DOM_INT || dom == DOM_DATE || dom == DOM_TS || dom == DOM_TSTZ;
}

static SiftVecType
vec_type_of(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
			return SVEC_I32;
		case FLOAT4OID:
		case FLOAT8OID:
			return SVEC_F64;
		default:
			return SVEC_I64;
	}
}

static SiftVexprNode *
node_new(GpuQuery *g, SiftVexprKind kind, SiftVecType type)
{
	SiftVexprNode *n = MemoryContextAllocZero(g->cx, sizeof(SiftVexprNode));

	n->kind = kind;
	n->type = type;
	return n;
}

static SiftVexprNode *
node_unary(GpuQuery *g, SiftVexprKind kind, SiftVecType type, const SiftVexprNode *arg)
{
	SiftVexprNode *n = node_new(g, kind, type);

	n->args[n->nargs++] = arg;
	return n;
}

static SiftVexprNode *
node_const(GpuQuery *g, Oid typid, SiftVecType type, Datum d)
{
	SiftVexprNode *n = node_new(g, SVX_CONST, type);

	switch (typid)
	{
		case INT2OID:
			n->i64 = DatumGetInt16(d);
			break;
		case INT4OID:
		case DATEOID:
			n->i64 = DatumGetInt32(d);
			break;
		case FLOAT4OID:
			n->f64 = DatumGetFloat4(d);
			break;
		case FLOAT8OID:
			n->f64 = DatumGetFloat8(d);
			break;
		default:
			n->i64 = DatumGetInt64(d);
			break;
	}
	return n;
}

static const SiftVexprNode *
node_column(GpuQuery *g, int attno, Oid typid, SiftVecType as)
{
	const int	slot = slot_of_attno(g, attno);
	SiftVexprNode *c;

	if (slot < 0)
		return NULL;
	c = node_new(g, SVX_COL, vec_type_of(typid));
	c->aux = slot;
	return c->type == as ? c : node_unary(g, SVX_CAST, as, c);
}

static int
cmp_op_of(Oid opno, Oid opfamily)
{
	switch (get_op_opfamily_strategy(opno, opfamily))
	{
		case BTLessStrategyNumber:
			return SCMP_LT;
		case BTLessEqualStrategyNumber:
			return SCMP_LE;
		case BTEqualStrategyNumber:
			return SCMP_EQ;
		case BTGreaterEqualStrategyNumber:
			return SCMP_GE;
		case BTGreaterStrategyNumber:
			return SCMP_GT;
		default:
			return get_op_opfamily_strategy(get_negator(opno), opfamily) == BTEqualStrategyNumber ? SCMP_NE : -1;
	}
}

/* The attribute of the index a Var of a clause reads (those of the scan of the table are mapped through its input); 0: the index has no column of it */
static int
qual_attno(GpuQuery *g, const Var *v)
{
	int			slot;

	if (g->tin == NULL)
		return v->varattno;
	return (slot = sift_v6_input_add_var(g->tin, v)) < 0 ? 0 : g->tin->col[slot].attno;
}

static Oid
qual_opfamily(const GpuQuery *g, int attno)
{
	return g->index != NULL ? g->index->rd_opfamily[attno - 1] : g->q.sch.att_opfamily[attno];
}

/* Var op Const with a constant that is not NULL: what the device compares; a clause of any other form (a parameter, an expression) is the CPU's */
static bool
var_const(GpuQuery *g, const List *args, const Var **v, const Const **k, int *attno)
{
	if (list_length(args) != 2 || !IsA(linitial(args), Var) || !IsA(lsecond(args), Const) || ((const Const *) lsecond(args))->constisnull)
		return false;
	*v = linitial(args);
	*k = lsecond(args);
	return (*attno = qual_attno(g, *v)) > 0;
}

/* what the scan of the CPU also knows of a comparison of a whole number column: its zones rule out the granules it cannot pass */
static void
note_zone_test(GpuQuery *g, int attno, const Var *v, const Const *k, int cmp)
{
	SiftGpuQuery *q = &g->q;

	if (!g->zones || !zone_domain(domain_of(v->vartype)) || cmp == SCMP_NE || q->nzone == SIFT_GPU_ZONE_TESTS)
		return;
	q->zone[q->nzone++] = (SiftGpuZoneCmp) {.col = slot_of_attno(g, attno),.op = cmp,.val = element_value(k->consttype, k->constvalue)};
}

static bool
is_text(Oid typid)
{
	return typid == TEXTOID || typid == VARCHAROID;
}

/* column = 'constant' and column <> 'constant' of a text, in a conjunct of the scan: the comparison of its code with the code the constant has in each part, which the units of the scan set (agg/gpu_dict_agg.h) */
static const SiftVexprNode *
compile_text_compare(GpuQuery *g, const OpExpr *op, const Var *v, const Const *k, int attno)
{
	const int	cmp = cmp_op_of(op->opno, qual_opfamily(g, attno));
	SiftGpuDictQual *dq = &g->dict;
	const SiftVexprNode *col;
	SiftVexprNode *n;
	text	   *s;
	char	   *str;

	if ((cmp != SCMP_EQ && cmp != SCMP_NE) || !g->zones || !is_text(k->consttype) || dq->n == SIFT_GPU_DICT_CMPS || !get_collation_isdeterministic(op->inputcollid))
		return NULL;
	if ((col = node_column(g, attno, v->vartype, SVEC_I64)) == NULL)
		return NULL;
	s = DatumGetTextPP(k->constvalue);
	str = MemoryContextAlloc(g->cx, VARSIZE_ANY_EXHDR(s));
	memcpy(str, VARDATA_ANY(s), VARSIZE_ANY_EXHDR(s));
	dq->cmp[dq->n++] = (SiftGpuDictCmp) {.slot = slot_of_attno(g, attno),.ne = cmp == SCMP_NE,.str = str,.len = VARSIZE_ANY_EXHDR(s),
		.code = node_const(g, INT8OID, SVEC_I64, Int64GetDatum(0))};
	n = node_new(g, SVX_CMP, SVEC_BOOL);
	n->aux = cmp;
	n->args[n->nargs++] = col;
	n->args[n->nargs++] = dq->cmp[dq->n - 1].code;
	return n;
}

/* column op constant, both of one domain: the comparison runs in the wider of their types */
static const SiftVexprNode *
compile_compare(GpuQuery *g, const OpExpr *op)
{
	const Var  *v;
	const Const *k;
	int			attno;
	Domain		dom;
	int			cmp;
	SiftVecType type;
	SiftVexprNode *n;
	const SiftVexprNode *col;

	if (!var_const(g, op->args, &v, &k, &attno))
		return NULL;
	if (is_text(v->vartype))
		return compile_text_compare(g, op, v, k, attno);
	dom = domain_of(v->vartype);
	cmp = cmp_op_of(op->opno, qual_opfamily(g, attno));
	if (dom == DOM_NONE || dom != domain_of(k->consttype) || cmp < 0)
		return NULL;
	type = dom == DOM_F64 ? SVEC_F64 : vec_type_of(v->vartype) == SVEC_I64 || vec_type_of(k->consttype) == SVEC_I64 ? SVEC_I64 : SVEC_I32;
	if ((col = node_column(g, attno, v->vartype, type)) == NULL)
		return NULL;
	n = node_new(g, SVX_CMP, SVEC_BOOL);
	n->aux = cmp;
	n->args[n->nargs++] = col;
	n->args[n->nargs++] = node_const(g, k->consttype, type, k->constvalue);
	note_zone_test(g, attno, v, k, cmp);
	return n;
}

static const SiftVexprNode *
compile_null_test(GpuQuery *g, const NullTest *nt)
{
	const Var  *v = (const Var *) nt->arg;
	int			attno;
	const SiftVexprNode *col;

	if (!IsA(nt->arg, Var) || (attno = qual_attno(g, v)) <= 0)
		return NULL;
	col = node_column(g, attno, v->vartype, vec_type_of(v->vartype));
	return col == NULL ? NULL : node_unary(g, nt->nulltesttype == IS_NULL ? SVX_IS_NULL : SVX_IS_NOT_NULL, SVEC_BOOL, col);
}

static int64
element_value(Oid elemtype, Datum d)
{
	switch (elemtype)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

/* column IN (constants) of whole numbers or instants: the set of the device; false for any other list */
static bool
add_in_list(GpuQuery *g, const ScalarArrayOpExpr *sa)
{
	const Var  *v;
	const Const *k;
	int			attno;
	ArrayType  *arr;
	Oid			elemtype;
	Domain		dom;
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int64	   *vals;

	if (!var_const(g, sa->args, &v, &k, &attno) || !sa->useOr || cmp_op_of(sa->opno, qual_opfamily(g, attno)) != SCMP_EQ)
		return false;
	arr = DatumGetArrayTypeP(k->constvalue);
	elemtype = ARR_ELEMTYPE(arr);
	dom = domain_of(v->vartype);
	if (dom == DOM_NONE || dom == DOM_F64 || dom != domain_of(elemtype) || g->q.nin == SIFT_VK_MAXCOLS)
		return false;
	deconstruct_array(arr, elemtype, get_typlen(elemtype), get_typbyval(elemtype), TYPALIGN_INT, &elems, &nulls, &n);
	if (n == 0)
		return false;
	vals = MemoryContextAlloc(g->cx, n * sizeof(int64));
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			return false;
		vals[i] = element_value(elemtype, elems[i]);
	}
	g->in_attno[g->q.nin] = attno;
	g->in[g->q.nin++] = (SiftGpuIn) {.n = n,.vals = vals};
	return true;
}

static const SiftVexprNode *
compile_qual(GpuQuery *g, const Node *qual)
{
	switch (nodeTag(qual))
	{
		case T_OpExpr:
			return compile_compare(g, (const OpExpr *) qual);
		case T_NullTest:
			return compile_null_test(g, (const NullTest *) qual);
		default:
			return NULL;
	}
}

/* The conjunction of quals as one program in prog (*has: there is one); in the quals of the scan the IN lists become sets and the comparisons zone tests, in those of a FILTER neither can be */
static bool
compile_conj(GpuQuery *g, const List *quals, SiftVexprProg *prog, bool scan, bool *has)
{
	const SiftVexprNode *root = NULL;

	g->zones = scan;
	foreach_ptr(Node, qual, quals)
	{
		const SiftVexprNode *n;

		if (IsA(qual, ScalarArrayOpExpr))
		{
			if (!scan || !add_in_list(g, (const ScalarArrayOpExpr *) qual))
				return false;
			continue;
		}
		if ((n = compile_qual(g, qual)) == NULL)
			return false;
		if (root == NULL)
			root = n;
		else
		{
			SiftVexprNode *both = node_new(g, SVX_AND, SVEC_BOOL);

			both->args[both->nargs++] = root;
			both->args[both->nargs++] = n;
			root = both;
		}
	}
	*has = root != NULL;
	if (scan)
		g->dict.where = root;
	return root == NULL ? scan : sift_vexpr_compile(root, prog);
}

/* The format of the relation of the query, when it feeds the device: its leaf map and the table of its device entries */
static bool
gpu_format(SiftGpuQuery *q, bool is_table)
{
	const SiftStoreOps *ops = sift_store_ops_for_oid(RelationGetRelid(q->rel), is_table);

	SiftStoreStats stats;

	if (ops == NULL || ops->gpu == NULL || !ops->schema(RelationGetRelid(q->rel), is_table, &q->sch) || !ops->stats(RelationGetRelid(q->rel), is_table, &stats))
		return false;
	q->ops = ops->gpu;
	q->live_rows = stats.live_rows;
	return true;
}

static bool
compile_quals(GpuQuery *g, const List *quals)
{
	bool		has;

	if (!compile_conj(g, quals, &g->where, true, &has))
		return false;
	if (has)
		g->q.where = &g->where;
	return true;
}

/* FILTER (WHERE ..) of each aggregate: the program of its clause */
static bool
compile_filters(GpuQuery *g, const SiftV6Spec *spec)
{
	for (int j = 0; j < spec->naggs; j++)
	{
		bool		has;

		if (spec->agg[j].fclause == 0)
			continue;
		if (!compile_conj(g, list_nth(spec->filter, spec->agg[j].fclause - 1), &g->filter[j], false, &has))
			return false;
		g->q.filter[j] = &g->filter[j];
	}
	return true;
}

/* The binned instants are timestamps of PostgreSQL's range, 4713 BC to 294276 AD, which in microseconds from 2000 lie within 2.2e17 of it, and an origin of that range is as near: that much and a stride more added to the
 * source puts it past the origin, so the integer division of the bin rounds down as timestamp_bin does and no instant of the range overflows */
#define BIN_SHIFT_MIN		INT64_C(320000000000000000)
#define BIN_STRIDE_MAX		INT64_C(1000000000000000000)

/* date_bin(stride, column, origin) of the key as one program: ((column - base) / stride) * stride + base, base = origin less a multiple of the stride that is at least BIN_SHIFT_MIN */
static bool
compile_bin(GpuQuery *g, const SiftV6Spec *spec, const SiftV6Bin *b)
{
	const SiftV6Col *k = &spec->key[b->key];
	const int64 shift = (BIN_SHIFT_MIN / b->stride_us + 1) * b->stride_us,
				base = b->origin - shift;
	const SiftVexprNode *col;
	SiftVexprNode *n;
	SiftVexprNode *sub,
			   *div,
			   *mul;

	if (b->stride_us <= 0 || b->stride_us > BIN_STRIDE_MAX || (col = node_column(g, k->attno, k->typid, SVEC_I64)) == NULL)
		return false;
	sub = node_new(g, SVX_SUB, SVEC_I64);
	sub->args[sub->nargs++] = col;
	sub->args[sub->nargs++] = node_const(g, INT8OID, SVEC_I64, Int64GetDatum(base));
	div = node_new(g, SVX_DIV, SVEC_I64);
	div->args[div->nargs++] = sub;
	div->args[div->nargs++] = node_const(g, INT8OID, SVEC_I64, Int64GetDatum(b->stride_us));
	mul = node_new(g, SVX_MUL, SVEC_I64);
	mul->args[mul->nargs++] = div;
	mul->args[mul->nargs++] = node_const(g, INT8OID, SVEC_I64, Int64GetDatum(b->stride_us));
	n = node_new(g, SVX_ADD, SVEC_I64);
	n->args[n->nargs++] = mul;
	n->args[n->nargs++] = node_const(g, INT8OID, SVEC_I64, Int64GetDatum(base));
	if (!sift_vexpr_compile(n, &g->key[b->key]))
		return false;
	g->q.key[b->key] = &g->key[b->key];
	return true;
}

static bool
compile_bins(GpuQuery *g, const SiftV6Spec *spec)
{
	for (int i = 0; i < spec->nbins; i++)
		if (!compile_bin(g, spec, &spec->bin[i]))
			return false;
	return true;
}

/* The slots of the columns the sets read, after those of the program */
static bool
assign_in_slots(GpuQuery *g)
{
	for (int i = 0; i < g->q.nin; i++)
		if ((g->in[i].col = slot_of_attno(g, g->in_attno[i])) < 0)
			return false;
	return true;
}

/* The slots of the columns the sets, the keys and the arguments read, after those of the program */
static bool
assign_slots(GpuQuery *g, const SiftV6Spec *spec)
{
	if (!assign_in_slots(g))
		return false;
	for (int i = 0; i < spec->nkeys; i++)
		if (slot_of_attno(g, spec->key[i].attno) < 0)
			return false;
	for (int j = 0; j < spec->naggs; j++)
		if (sift_v6_kind_reads_column(spec->agg[j].kind) && slot_of_attno(g, spec->agg[j].arg.attno) < 0)
			return false;
	return true;
}

static int
gen_cmp(const void *a, const void *b)
{
	const uint64 x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/* The generations of the parts of the granules the device read, ascending and each once */
static uint64 *
covered_gens(const SiftGpuPlan *plan, MemoryContext cx, int *n)
{
	uint64	   *gens = MemoryContextAlloc(cx, Max(plan->ngran, 1) * sizeof(uint64));

	for (uint32 i = 0; i < plan->ngran; i++)
	{
		uint32		part_id,
					granule;

		plan->ops->locate(plan, i, &part_id, &gens[i], &granule);
	}
	qsort(gens, plan->ngran, sizeof(uint64), gen_cmp);
	*n = 0;
	for (uint32 i = 0; i < plan->ngran; i++)
		if (*n == 0 || gens[*n - 1] != gens[i])
			gens[(*n)++] = gens[i];
	return gens;
}

/*
 * The first one or two sort columns of ORDER BY .. LIMIT K of the groups, as the device orders them: a group key of whole numbers or the count of a COUNT aggregate. A group worse than K others in these is not
 * among the best K whatever the columns after them say, so the device may leave it out; a HAVING can leave K better groups out first.
 */
static SiftVkTopk
device_topk(const SiftV6Spec *s, bool having)
{
	SiftVkTopk	t = {0};

	if (having || s->topk == 0 || s->ntopk == 0)
		return t;
	for (int i = 0; i < Min(s->ntopk, 2); i++)
	{
		const SiftV6TopKItem *it = &s->topk_item[i];
		const int	agg = it->pos - s->nkeys;
		Oid			opfamily,
					opcintype;
		SiftCmpStrategy cmptype;
		bool		is_agg = agg >= 0;

		if (is_agg ? agg >= s->naggs || (s->agg[agg].kind != SV6_COUNT_STAR && s->agg[agg].kind != SV6_COUNT) : it->pos < 0 || sift_v6_class(s->key[it->pos].typid) == SV6C_TEXT)
			break;
		if (!get_ordering_op_properties(it->sortop, &opfamily, &opcintype, &cmptype))
			break;
		t.comp[t.ncomp++] = (uint32) is_agg | (uint32) (is_agg ? agg : it->pos) << 1 | (uint32) (cmptype == SIFT_CMP_GT) << 9 | (uint32) it->nulls_first << 10;
	}
	t.k = t.ncomp > 0 ? s->topk : 0;
	t.cover = (int) t.ncomp == s->ntopk;
	return t;
}

SiftV6Group *
sift_v6_gpu_agg(const SiftV6Spec *spec, Relation heap, Relation index, Snapshot snap, double est_groups, bool having, MemoryContext gcx, TupleDesc desc, SiftV6Gpu *gpu)
{
	Relation	rel = index != NULL ? index : heap;
	GpuQuery   *g;
	SiftV6Group *groups;

	*gpu = (SiftV6Gpu) {0};
	if (sift_gpu_mode == SIFT_GPU_OFF || spec->nderive > 0 || spec->vqual != NIL || !device_shape(spec))
		return NULL;
	g = MemoryContextAllocZero(gcx, sizeof(GpuQuery));
	g->cx = gcx;
	g->index = index;
	g->q = (SiftGpuQuery) {.in = g->in,.rel = rel,.heap = index != NULL ? heap : NULL,.snap = snap,.spec = spec,
		.est_groups = spec->nkeys == 0 ? 1 : spec->groups_est > 0 ? spec->groups_est : est_groups};
	if (!gpu_format(&g->q, index == NULL) || !compile_quals(g, spec->indexqual) || !compile_filters(g, spec) || !compile_bins(g, spec) || !assign_slots(g, spec))
		return NULL;
	g->q.topk = device_topk(spec, having);
	g->q.dict = g->dict.n > 0 ? &g->dict : NULL;
	groups = sift_gpu_v6_agg(&g->q, gcx, desc, &gpu->run);
	if (groups != NULL)
		gpu->skip = covered_gens(gpu->run->plan, gcx, &gpu->nskip);
	return groups;
}

/* ORDER BY one whole number, date or instant, no ties: what the device ranks (a float key orders -0 and NaN as the tournament does not) */
static bool
topn_shape(const SiftV6TopNSpec *spec)
{
	const Domain dom = domain_of(spec->key[0].typid);

	return spec->nkeys == 1 && !spec->with_ties && dom != DOM_NONE && dom != DOM_F64;
}

bool
sift_v6_gpu_topn_forced(const SiftV6TopNSpec *spec)
{
	return sift_gpu_mode == SIFT_GPU_ON && topn_shape(spec);
}

/* The sort program is the column of the key */
static bool
topn_key(GpuQuery *g, const SiftV6TopNSpec *spec)
{
	const SiftVexprNode *col;

	if (!topn_shape(spec))
		return false;
	if ((col = node_column(g, spec->key[0].attno, spec->key[0].typid, vec_type_of(spec->key[0].typid))) == NULL)
		return false;
	g->q.sort = &g->sort;
	return sift_vexpr_compile(col, &g->sort);
}

bool
sift_v6_gpu_topn(const SiftV6TopNSpec *spec, SiftV6Input *in, const List *quals, Relation heap, Relation index, Snapshot snap, bool prunable, MemoryContext cx,
				 SiftV6Gpu *gpu, SiftV6TopNDevice *dev)
{
	GpuQuery   *g;
	SiftGpuCand *cand;
	uint32		ncand;

	*gpu = (SiftV6Gpu) {0};
	if (sift_gpu_mode == SIFT_GPU_OFF)
		return false;
	g = MemoryContextAllocZero(cx, sizeof(GpuQuery));
	g->cx = cx;
	g->index = index;
	g->tin = in;
	g->q = (SiftGpuQuery) {.in = g->in,.rel = index,.heap = heap,.snap = snap};
	if (!gpu_format(&g->q, false) || !compile_quals(g, quals) || g->dict.n > 0 || !topn_key(g, spec) || !assign_in_slots(g))
		return false;
	if (!sift_gpu_v6_topn(&g->q, (uint32) spec->rows, spec->order[0].desc, spec->order[0].nulls_first, prunable, cx, &gpu->run, &cand, &ncand))
		return false;
	gpu->skip = covered_gens(gpu->run->plan, cx, &gpu->nskip);
	*dev = (SiftV6TopNDevice) {.cand = cand,.ncand = ncand,.skip = gpu->skip,.nskip = gpu->nskip};
	return true;
}

void
sift_v6_gpu_explain(const SiftV6Gpu *gpu, ExplainState *es)
{
	sift_gpu_v6_explain(gpu->run, es);
}

void
sift_v6_gpu_end(SiftV6Gpu *gpu)
{
	sift_gpu_v6_end(gpu->run);
}
