/*
 * tests/emit.c - T10: the programs the engine really makes reach the fast kernels. Trees of the engine's expressions (agg/vexpr/vexpr.h) are compiled by its own compiler (agg/vexpr/compile.c,
 * not a copy), translated for the device by the translator (agg/gpu/emit.c, the one the server runs), recognized by agg/vk/fast.c and run on the device; the answer is checked against the scalar
 * oracle on the translated program and against a plain evaluation of the predicates over the logical rows. Every conjunction of comparisons of a column with a constant must run on a fast kernel
 * (a program it does not recognize runs the expression machine, correctly and many times slower, and nothing else says so); the queries that are not that shape must not.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"

#include "agg/gpu/emit.h"
#include "agg/vk/fast.h"
#include "agg/vexpr/vexpr.h"
#include "agg_case.h"

bool		sift_agg_exact_float = false;

int
vcal_extract_scale(SiftVxField field, bool is_date)
{
	(void) field;
	(void) is_date;
	abort();
}

#define NPRED_MAX	(FAST_PRED_MAX + 1)
#define NODES		64

typedef struct T10Col
{
	int			c;
	SiftVecType type;			/* of the column as the engine reads it */
	SiftCellKind kind;
	int			scale;
	int64_t		lo,
				hi;				/* the values the data has */
	Oid			typid;
} T10Col;

static const T10Col cols[] = {
	{C_K0, SVEC_I32, SIFT_CELL_INT, 0, 0, 9, INT4OID},
	{C_K1, SVEC_I32, SIFT_CELL_INT, 0, 100, 139, INT4OID},
	{C_V32, SVEC_I32, SIFT_CELL_INT, 0, -1000, 1000, INT4OID},
	{C_V32, SVEC_I64, SIFT_CELL_INT, 0, -1000, 1000, INT8OID},
	{C_BIG, SVEC_I64, SIFT_CELL_INT, 0, ((int64_t) 1 << 62) - 1000, ((int64_t) 1 << 62) + 1000, INT8OID},
	{C_TS, SVEC_I64, SIFT_CELL_INT, 0, -3150000000000000LL, 3150000000000000LL, INT8OID},
	{C_DATE, SVEC_I32, SIFT_CELL_INT, 0, -36524, 36525, DATEOID},
	{C_NUM, SVEC_NUM64, SIFT_CELL_INT, 2, -9999999, 9999999, NUMERICOID},
	{C_DBL, SVEC_F64, SIFT_CELL_F8, 0, 1, 99999, FLOAT8OID},
};

#define NCOLS_T10		((int) (sizeof(cols) / sizeof(cols[0])))

typedef struct Pred
{
	int			col;			/* index into cols */
	SiftCmpOp	op;
	int64_t		k;				/* the constant: for a double, hundredths */
	bool		flip;			/* const op column */
	SiftVecType ctype;			/* the type of the constant */
	int			cscale;			/* a numeric constant's scale */
} Pred;

typedef struct Tree
{
	SiftVexprNode n[NODES];
	int			nn;
} Tree;

static SiftVexprNode *
node(Tree *t, SiftVexprKind kind, SiftVecType type)
{
	SiftVexprNode *n = &t->n[t->nn++];

	memset(n, 0, sizeof(*n));
	n->kind = kind;
	n->type = type;
	return n;
}

static const SiftVexprNode *
col_node(Tree *t, const T10Col *c)
{
	SiftVexprNode *n = node(t, SVX_COL, c->type);

	n->aux = c->c;
	n->scale = (int16) c->scale;
	return n;
}

static const SiftVexprNode *
const_node(Tree *t, const Pred *p)
{
	SiftVexprNode *n = node(t, SVX_CONST, p->ctype);

	n->scale = (int16) p->cscale;
	if (p->ctype == SVEC_F64)
		n->f64 = (double) p->k / 100.0;
	else
		n->i64 = cols[p->col].type == SVEC_F64 ? p->k / 100 : p->k;
	return n;
}

static const SiftVexprNode *
cmp_node(Tree *t, const Pred *p)
{
	SiftVexprNode *n = node(t, SVX_CMP, SVEC_BOOL);
	const SiftVexprNode *a = col_node(t, &cols[p->col]),
			   *b = const_node(t, p);

	n->aux = p->op;
	n->nargs = 2;
	n->args[0] = p->flip ? b : a;
	n->args[1] = p->flip ? a : b;
	return n;
}

static const SiftVexprNode *
bin_node(Tree *t, SiftVexprKind kind, SiftVecType type, const SiftVexprNode *a, const SiftVexprNode *b)
{
	SiftVexprNode *n = node(t, kind, type);

	n->nargs = 2;
	n->args[0] = a;
	n->args[1] = b;
	return n;
}

/* the conjunction of preds[lo, hi) as a tree: shape 0 left deep, 1 right deep, 2 balanced */
static const SiftVexprNode *
and_tree(Tree *t, const Pred *preds, int lo, int hi, int shape)
{
	int			mid;

	if (hi - lo == 1)
		return cmp_node(t, &preds[lo]);
	mid = shape == 0 ? hi - 1 : shape == 1 ? lo + 1 : lo + (hi - lo) / 2;
	return bin_node(t, SVX_AND, SVEC_BOOL, and_tree(t, preds, lo, mid, shape), and_tree(t, preds, mid, hi, shape));
}

static bool
eval_pred(const Pred *p, int64_t v)
{
	const T10Col *c = &cols[p->col];
	int			cmp;

	if (c->type == SVEC_F64)
	{
		double		x = f8_from_key(v),
					k = (double) p->k / 100.0;

		cmp = x < k ? -1 : x > k ? 1 : 0;
	}
	else
		cmp = v < p->k ? -1 : v > p->k ? 1 : 0;
	if (p->flip)
		cmp = -cmp;
	return p->op == SCMP_EQ ? cmp == 0 : p->op == SCMP_NE ? cmp != 0 : p->op == SCMP_LT ? cmp < 0 : p->op == SCMP_LE ? cmp <= 0 : p->op == SCMP_GT ? cmp > 0 : cmp >= 0;
}

/* the rows of the logical data that the predicates keep: live, every column non-NULL and every comparison true */
static uint64_t
count_rows(const TData *d, const Pred *preds, int np)
{
	uint64_t	n = 0;

	for (uint32_t gi = 0; gi < d->ngran; gi++)
	{
		const TGran *g = &d->g[gi];

		for (uint32_t r = 0; r < g->nrows; r++)
		{
			bool		keep = !(g->has_dead && (g->dead[r >> 5] >> (r & 31) & 1));

			for (int i = 0; i < np && keep; i++)
			{
				const TCol *tc = &g->col[cols[preds[i].col].c];

				keep = tc->ok[r] && eval_pred(&preds[i], tc->v[r]);
			}
			n += keep;
		}
	}
	return n;
}

/* the query the server's translator is given, over the columns of the logical data */
typedef struct T10Query
{
	SiftStoreSchema sch;
	int			att_first[C_NCOLS + 2];
	SiftCellKind kinds[C_NCOLS];
	uint8		hints[C_NCOLS];
	SiftV6Col	key[2];
	SiftV6Agg	agg[SIFT_VK_MAXAGGS];
	SiftV6Spec	spec;
	SiftGpuQuery q;
	SiftGpuEmit em;
	SiftVexprProg prog[2 + SIFT_VK_MAXAGGS];
	SiftGpuIn	in;
	int64_t		in_vals[4];
} T10Query;

static void
query_init(T10Query *x)
{
	memset(x, 0, sizeof(*x));
	for (int c = 0; c < NCOLS_T10; c++)
		x->kinds[cols[c].c] = cols[c].kind;
	for (int a = 0; a <= C_NCOLS; a++)
		x->att_first[a] = a == 0 ? -1 : a - 1;
	x->sch.att_first = x->att_first;
	x->sch.leaf_kinds = x->kinds;
	x->sch.leaf_hints = x->hints;
	x->spec.key = x->key;
	x->spec.agg = x->agg;
	x->q = (SiftGpuQuery) {.sch = x->sch, .spec = &x->spec, .ncols = C_NCOLS};
	for (int c = 0; c < C_NCOLS; c++)
		x->q.leaf[c] = c;
	sift_gpu_emit_init(&x->em);
}

static void
agg_of(T10Query *x, SiftV6Kind kind, int col)
{
	SiftV6Agg  *a = &x->agg[x->spec.naggs++];

	a->kind = kind;
	a->arg.attno = col + 1;
}

typedef struct T10Count
{
	int			simple,
				fast,
				other,
				other_fast;
} T10Count;

/* translates, checks the recognizer's verdict, and runs: the device against the oracle (case_check) and against the rows counted here (when preds is not NULL) */
static void
run_query(SiftVk *dev, const TData *d, T10Query *x, SiftVkMode mode, const char *name, const Pred *preds, int np, bool expect_fast, T10Count *cnt)
{
	Case		c;
	SiftVkResult res;
	const char *why;
	bool		fast;

	case_init(&c, d, mode);
	why = sift_gpu_emit_agg(&x->em, &x->q, &c.j);
	CHECK(why == NULL, "%s: the translator refused the query: %s", name, why ? why : "");
	if (why)
		return;
	memcpy(c.p.w, x->em.ops, x->em.nops * SIFT_VK_OP_WORDS * 4);
	c.p.n = x->em.nops;
	c.j.ngran = d->ngran;
	c.j.gran = d->vg;
	if (mode == SIFT_VK_DENSE)
	{
		c.j.dense_kmin[0] = x->key[0].attno == C_K1 + 1 ? 100 : 0;
		c.j.dense_span[0] = x->key[0].attno == C_K1 + 1 ? 41 : 11;
	}
	else if (mode == SIFT_VK_HASH)
		c.j.est_groups = 4096;
	c.j.nops = c.p.n;
	c.j.ops = c.p.w;
	fast = sift_vk_job_is_fast(&c.j);
	CHECK(fast == expect_fast, "%s: the recognizer says %s, expected %s", name, fast ? "fast" : "the machine", expect_fast ? "fast" : "the machine");
	if (preds)
	{
		cnt->simple++;
		cnt->fast += fast;
	}
	else
	{
		cnt->other++;
		cnt->other_fast += fast;
	}
	if (fast != expect_fast)
		return;
	CHECK(sift_vk_run(dev, &c.j, NULL, &res) == SIFT_VK_OK, "%s: %s", name, sift_vk_error(dev));
	CHECK(res.fast == fast, "%s: the device ran the %s, the recognizer said %s", name, res.fast ? "fast kernel" : "machine", fast ? "fast" : "machine");
	if (preds && mode == SIFT_VK_UNGROUPED)
		CHECK((uint64_t) res.acc[0] == count_rows(d, preds, np), "%s: %lld rows, the predicates keep %llu", name, (long long) res.acc[0], (unsigned long long) count_rows(d, preds, np));
	case_check(name, &c, d, 1e-9);
}

static Pred
random_pred(Rng *rng, int forced_col)
{
	int			ci = forced_col >= 0 ? forced_col : (int) (rng_next(rng) % NCOLS_T10);
	const T10Col *c = &cols[ci];
	Pred		p = {.col = ci, .op = (SiftCmpOp) (rng_next(rng) % 6), .flip = rng_next(rng) & 1, .ctype = c->type, .cscale = c->scale};

	p.k = rng_range(rng, c->lo, c->hi);
	if (c->type == SVEC_I32 || c->type == SVEC_I64)
	{
		bool		narrow = c->lo >= INT32_MIN && c->hi <= INT32_MAX;

		if (narrow && rng_next(rng) % 3 == 0)
			p.ctype = c->type == SVEC_I32 ? SVEC_I64 : SVEC_I32;
	}
	if (c->type == SVEC_F64 && rng_next(rng) % 4 == 0)
	{
		p.ctype = SVEC_I32;
		p.k = rng_range(rng, 1, 999) * 100;
	}
	return p;
}

static void
aggregates(T10Query *x, Rng *rng, SiftVkMode mode)
{
	static const int agg_cols[] = {C_V32, C_K1, C_DBL, C_BIG, C_NUM, C_K0};
	static const SiftV6Kind kinds[] = {SV6_COUNT, SV6_SUM, SV6_MIN, SV6_MAX};
	int			n = 1 + (int) (rng_next(rng) % 4);

	agg_of(x, SV6_COUNT_STAR, 0);
	for (int a = 1; a < n; a++)
		agg_of(x, kinds[rng_next(rng) % 4], agg_cols[rng_next(rng) % 6]);
	if (mode != SIFT_VK_UNGROUPED)
	{
		x->spec.nkeys = 1;
		x->key[0].attno = (rng_next(rng) & 1 ? C_K1 : C_K0) + 1;
		x->key[0].typid = INT4OID;
	}
}

static void
simple_query(SiftVk *dev, const TData *d, Rng *rng, int i, const Pred *preds, int np, int shape, SiftVkMode mode, T10Count *cnt)
{
	T10Query	   *x = malloc(sizeof(T10Query));
	Tree		t = {0};
	char		name[96];

	query_init(x);
	CHECK(sift_vexpr_compile(and_tree(&t, preds, 0, np, shape), &x->prog[0]), "query %d: the compiler refused the conjunction", i);
	x->q.where = &x->prog[0];
	aggregates(x, rng, mode);
	snprintf(name, sizeof(name), "T10 %s #%d (%d comparisons, tree %d)", mode == SIFT_VK_UNGROUPED ? "ungrouped" : mode == SIFT_VK_DENSE ? "dense" : "hash", i, np, shape);
	run_query(dev, d, x, mode, name, preds, np, true, cnt);
	free(x);
}

/* the queries that are not a conjunction of comparisons of columns with constants: the machine runs them, and the answer must still be right */
static void
other_queries(SiftVk *dev, const TData *d, Rng *rng, T10Count *cnt)
{
	static const char *const names[] = {"OR", "NOT", "arithmetic", "column against column", "IN list", "FILTER", "past the comparison cap", "numeric constant of another scale"};

	for (int v = 0; v < 8; v++)
	{
		T10Query	   *x = malloc(sizeof(T10Query));
		Tree		t = {0};
		Pred		a = random_pred(rng, 1),
					b = random_pred(rng, 2),
					over[NPRED_MAX];
		const SiftVexprNode *w = NULL;
		char		name[96];

		query_init(x);
		a.ctype = SVEC_I32;
		b.ctype = SVEC_I32;
		if (v == 0)
			w = bin_node(&t, SVX_OR, SVEC_BOOL, cmp_node(&t, &a), cmp_node(&t, &b));
		else if (v == 1)
		{
			SiftVexprNode *n = node(&t, SVX_NOT, SVEC_BOOL);

			n->nargs = 1;
			n->args[0] = cmp_node(&t, &a);
			w = n;
		}
		else if (v == 2)
		{
			const SiftVexprNode *sum = bin_node(&t, SVX_ADD, SVEC_I32, col_node(&t, &cols[1]), col_node(&t, &cols[2]));
			SiftVexprNode *k = node(&t, SVX_CONST, SVEC_I32);
			SiftVexprNode *n = node(&t, SVX_CMP, SVEC_BOOL);

			k->i64 = 100;
			n->aux = SCMP_GT;
			n->nargs = 2;
			n->args[0] = sum;
			n->args[1] = k;
			w = n;
		}
		else if (v == 3)
		{
			SiftVexprNode *n = node(&t, SVX_CMP, SVEC_BOOL);

			n->aux = SCMP_LT;
			n->nargs = 2;
			n->args[0] = col_node(&t, &cols[0]);
			n->args[1] = col_node(&t, &cols[1]);
			w = n;
		}
		else if (v == 6)
		{
			for (int i = 0; i < NPRED_MAX; i++)
				over[i] = random_pred(rng, i % 3);
			w = and_tree(&t, over, 0, NPRED_MAX, 0);
		}
		else if (v == 7)
		{
			a = random_pred(rng, 7);
			a.cscale = 0;
			a.k = rng_range(rng, -1000, 1000);
			w = cmp_node(&t, &a);
		}
		else
			w = cmp_node(&t, &a);
		CHECK(sift_vexpr_compile(w, &x->prog[0]), "%s: the compiler refused the tree", names[v]);
		x->q.where = &x->prog[0];
		if (v == 4)
		{
			x->in = (SiftGpuIn) {.col = C_K1, .n = 3, .vals = x->in_vals};
			x->in_vals[0] = 100;
			x->in_vals[1] = 120;
			x->in_vals[2] = 139;
			x->q.nin = 1;
			x->q.in = &x->in;
			x->q.where = NULL;
		}
		aggregates(x, rng, SIFT_VK_UNGROUPED);
		if (v == 5)
		{
			if (x->spec.naggs < 2)
				agg_of(x, SV6_SUM, C_V32);
			Tree		ft = {0};

			CHECK(sift_vexpr_compile(cmp_node(&ft, &a), &x->prog[1]), "FILTER: the compiler refused the tree");
			x->q.filter[1] = &x->prog[1];
			x->q.where = NULL;
		}
		snprintf(name, sizeof(name), "T10 other: %s", names[v]);
		run_query(dev, d, x, SIFT_VK_UNGROUPED, name, NULL, 0, false, cnt);
		free(x);
	}
}

void
test_emit(SiftVk *dev, Rng *rng)
{
	TData		d;
	T10Count	cnt = {0};
	int			qn = 0;

	build_base(&d, 6, rng, 5001);
	printf("T10 the engine's compiler and translator against the fast kernels\n");
	for (int ci = 0; ci < NCOLS_T10; ci++)
		for (int op = 0; op < 6; op++)
			for (int flip = 0; flip < 2; flip++)
			{
				Pred		p = random_pred(rng, ci);

				p.op = (SiftCmpOp) op;
				p.flip = flip;
				p.ctype = cols[ci].type;
				p.cscale = cols[ci].scale;
				p.k = cols[ci].lo / 2 + cols[ci].hi / 2 + rng_range(rng, -50, 50);
				simple_query(dev, &d, rng, qn++, &p, 1, 0, SIFT_VK_UNGROUPED, &cnt);
			}
	for (int i = 0; i < 90; i++)
	{
		Pred		preds[NPRED_MAX];
		int			np = 2 + i % (FAST_PRED_MAX - 1);
		SiftVkMode	mode = i % 5 == 0 ? SIFT_VK_DENSE : i % 5 == 1 ? SIFT_VK_HASH : SIFT_VK_UNGROUPED;

		for (int p = 0; p < np; p++)
			preds[p] = random_pred(rng, -1);
		simple_query(dev, &d, rng, qn++, preds, np, (i / 3) % 3, mode, &cnt);
	}
	other_queries(dev, &d, rng, &cnt);
	printf("T10: %d queries of columns against constants, %d on a fast kernel; %d other queries, %d on a fast kernel\n", cnt.simple, cnt.fast, cnt.other, cnt.other_fast);
	CHECK(cnt.fast == cnt.simple && cnt.other_fast == 0, "T10: %d of %d simple queries on a fast kernel, %d of the others", cnt.fast, cnt.simple, cnt.other_fast);
	td_free(&d);
}
