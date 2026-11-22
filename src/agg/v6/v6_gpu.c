/*
 * agg/v6/v6_gpu.c - the query of SiftAggV6 as the device is asked it: the quals of the scan (the forms agg/v6/plan.c keeps: column op constant, column IN constants, IS [NOT] NULL) as one
 * program of agg/vexpr.h and IN sets, the columns of the keys and the aggregates as slots of the leaves of the store, and the parts the device read as the generations the CPU scan leaves out.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "nodes/primnodes.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/gpu_cfg.h"
#include "agg/gpu_v6.h"
#include "agg/v6/v6_gpu.h"

typedef struct GpuQuery
{
	SiftGpuQuery q;
	SiftGpuIn	in[SIFT_VK_MAXCOLS];
	int			in_attno[SIFT_VK_MAXCOLS];
	SiftVexprProg where;
	MemoryContext cx;
	Relation	index;
} GpuQuery;

static int64 element_value(Oid elemtype, Datum d);

static int
slot_of_attno(GpuQuery *g, int attno)
{
	SiftGpuQuery *q = &g->q;
	const int	leaf = q->store->att_first[attno];

	for (int i = 0; i < q->ncols; i++)
		if (q->leaf[i] == leaf)
			return i;
	if (leaf < 0 || q->ncols == SIFT_VK_MAXCOLS)
		return -1;
	q->leaf[q->ncols] = leaf;
	return q->ncols++;
}

static bool
device_shape(const SiftV6Spec *s)
{
	if (s->nsets > 0 || s->nfuncs > 0 || s->nfilters > 0 || s->nbins > 0 || s->naggs == 0)
		return false;
	for (int j = 0; j < s->naggs; j++)
		if (s->agg[j].fclause != 0 || s->agg[j].nxcols > 0)
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
node_column(GpuQuery *g, const Var *v, SiftVecType as)
{
	const int	slot = slot_of_attno(g, v->varattno);
	SiftVexprNode *c;

	if (slot < 0)
		return NULL;
	c = node_new(g, SVX_COL, vec_type_of(v->vartype));
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

/* what the scan of the CPU also knows of a comparison of a whole number column: its zones rule out the granules it cannot pass */
static void
note_zone_test(GpuQuery *g, const Var *v, const Const *k, int cmp)
{
	SiftGpuQuery *q = &g->q;

	if (domain_of(v->vartype) != DOM_INT || cmp == SCMP_NE || k->constisnull || q->nzone == SIFT_GPU_ZONE_TESTS)
		return;
	q->zone[q->nzone++] = (SiftGpuZoneCmp) {.col = slot_of_attno(g, v->varattno),.op = cmp,.val = element_value(k->consttype, k->constvalue)};
}

/* column op constant, both of one domain: the comparison runs in the wider of their types */
static const SiftVexprNode *
compile_compare(GpuQuery *g, const OpExpr *op)
{
	const Var  *v = linitial(op->args);
	const Const *k = lsecond(op->args);
	const Domain dom = domain_of(v->vartype);
	const int	cmp = cmp_op_of(op->opno, g->index != NULL ? g->index->rd_opfamily[v->varattno - 1] : g->q.store->opfamily[v->varattno]);
	SiftVecType type;
	SiftVexprNode *n;
	const SiftVexprNode *col;

	if (dom == DOM_NONE || dom != domain_of(k->consttype) || cmp < 0)
		return NULL;
	type = dom == DOM_F64 ? SVEC_F64 : vec_type_of(v->vartype) == SVEC_I64 || vec_type_of(k->consttype) == SVEC_I64 ? SVEC_I64 : SVEC_I32;
	if ((col = node_column(g, v, type)) == NULL)
		return NULL;
	n = node_new(g, SVX_CMP, SVEC_BOOL);
	n->aux = cmp;
	n->args[n->nargs++] = col;
	n->args[n->nargs++] = node_const(g, k->consttype, type, k->constvalue);
	note_zone_test(g, v, k, cmp);
	return n;
}

static const SiftVexprNode *
compile_null_test(GpuQuery *g, const NullTest *nt)
{
	const Var  *v = (const Var *) nt->arg;
	const SiftVexprNode *col = node_column(g, v, vec_type_of(v->vartype));

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
	const Var  *v = linitial(sa->args);
	ArrayType  *arr = DatumGetArrayTypeP(((const Const *) lsecond(sa->args))->constvalue);
	const Oid	elemtype = ARR_ELEMTYPE(arr);
	const Domain dom = domain_of(v->vartype);
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int64	   *vals;

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
	g->in_attno[g->q.nin] = v->varattno;
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

/* The quals of the scan: IN lists to sets, the others to the one program of the WHERE */
static bool
compile_quals(GpuQuery *g, const SiftV6Spec *spec)
{
	const SiftVexprNode *root = NULL;

	foreach_ptr(Node, qual, spec->indexqual)
	{
		const SiftVexprNode *n;

		if (IsA(qual, ScalarArrayOpExpr))
		{
			if (!add_in_list(g, (const ScalarArrayOpExpr *) qual))
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
	if (root == NULL)
		return true;
	g->q.where = &g->where;
	return sift_vexpr_compile(root, &g->where);
}

/* The slots of the columns the sets, the keys and the arguments read, after those of the program */
static bool
assign_slots(GpuQuery *g, const SiftV6Spec *spec)
{
	for (int i = 0; i < g->q.nin; i++)
		if ((g->in[i].col = slot_of_attno(g, g->in_attno[i])) < 0)
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

		sift_gpu_f2_locate(plan, i, &part_id, &gens[i], &granule);
	}
	qsort(gens, plan->ngran, sizeof(uint64), gen_cmp);
	*n = 0;
	for (uint32 i = 0; i < plan->ngran; i++)
		if (*n == 0 || gens[*n - 1] != gens[i])
			gens[(*n)++] = gens[i];
	return gens;
}

SiftV6Group *
sift_v6_gpu_agg(const SiftV6Spec *spec, Relation heap, Relation index, Snapshot snap, double est_groups, MemoryContext gcx, TupleDesc desc, SiftV6Gpu *gpu)
{
	Relation	rel = index != NULL ? index : heap;
	GpuQuery   *g;
	SiftV6Group *groups;

	*gpu = (SiftV6Gpu) {0};
	if (sift_gpu_mode == SIFT_GPU_OFF || !device_shape(spec))
		return NULL;
	g = MemoryContextAllocZero(gcx, sizeof(GpuQuery));
	g->cx = gcx;
	g->index = index;
	g->q = (SiftGpuQuery) {.in = g->in,.rel = rel,.heap = index != NULL ? heap : NULL,.store = sift_f2_store_open(rel),.snap = snap,.spec = spec,
		.est_groups = spec->nkeys == 0 ? 1 : est_groups};
	if (!compile_quals(g, spec) || !assign_slots(g, spec))
		return NULL;
	groups = sift_gpu_v6_agg(&g->q, gcx, desc, &gpu->run);
	if (groups != NULL)
		gpu->skip = covered_gens(gpu->run->plan, gcx, &gpu->nskip);
	return groups;
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
