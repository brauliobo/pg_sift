/*
 * agg/v6/v6_unnest_run.c - the aggregate scan of a LATERAL unnest of an array column (agg/v6/v6_unnest_run.h).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "access/tableam.h"
#include "executor/executor.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/v6_datum.h"
#include "agg/v6/v6_expr_str.h"
#include "agg/v6/v6_gather.h"
#include "agg/v6/v6_pg_expr.h"
#include "agg/v6/v6_plan_rewrite.h"
#include "agg/v6/v6_pipe.h"
#include "agg/v6/v6_star_run.h"
#include "agg/v6/v6_unnest.h"
#include "agg/v6/v6_unnest_jsonb.h"
#include "col/f2/f2_v6_unnest_run.h"
#include "agg/v6/v6_vet.h"
#include "agg/vexpr_str.h"
#include "col/am.h"
#include "col/f2/f2_sink_ext.h"
#include "col/f2/f2_v6_map_preagg.h"
#include "core/errctx.h"
#include "core/memtrack.h"

#define OUT_SLOTS	(SIFT_F2_PULL_MAX_COLS + SIFT_V6_XELEM_COLS + SIFT_V6_MAX_DERIVE + 1)
#define MONTH_SLOTS	4096

StaticAssertDecl(SIFT_V6_XELEM_COLS <= SIFT_F2_PULL_MAX_CHILD, "an element column is a child leaf of the pull");

typedef struct UnCol
{
	int			leaf;
	SiftV6Class cls;
	bool		month;			/* the month of the date, which PostgreSQL finds through timestamptz and the zone of the session: each distinct date is asked of it once */
} UnCol;

/* an arithmetic of the statement (SVD_ARITH): the vector of the pairs each of its columns is, and whether the column is a float8 */
typedef struct UnArith
{
	int			nin;
	int			in[SIFT_V6_MAX_CASE + 1];
	bool		f64[SIFT_V6_MAX_CASE + 1];
} UnArith;

typedef struct UnRun
{
	const SiftV6Spec *spec;
	const SiftV6Ext *x;
	SiftV6ScanStats *stats;
	SiftV6Vet	vet;
	SiftV6Pipe *pipe;
	SiftBatchSel bs;
	SiftBatchSel obs;			/* the pairs of a call of emit_pairs */
	int			ncols;
	UnCol		cols[SIFT_F2_PULL_MAX_COLS];
	UnArith		arith[SIFT_V6_MAX_DERIVE];
	SiftV6PgExpr *pg[SIFT_V6_MAX_DERIVE];	/* an SVD_PG_EXPR derivation: its evaluator, the vector of the pairs each of its inputs is, and its result */
	int			pgslot[SIFT_V6_MAX_DERIVE][SIFT_V6_MAX_COLS];
	SiftVxOut	pgout[SIFT_V6_MAX_DERIVE];
	int			nel;			/* the columns of the element */
	int			row_slot;		/* the slot of the number of the row of each pair, for an EXISTS over the elements (-1: none) */
	SiftVecType etype[SIFT_V6_XELEM_COLS];
	bool		eused[SIFT_V6_XELEM_COLS];	/* the statement reads the column; the others are read for the values their leaf cannot hold */
	AttrNumber	heap_att;		/* the array column of the table */
	int16		typlen;
	bool		typbyval;
	char		typalign;
	SiftV6RecFn *rec;
	EState	   *estate;			/* the quals of the table as a row of the heap is tested with them */
	ExprContext *econtext;
	ExprState  *hqual;
	ExprState  *arr;			/* the array of a row of the heap when it is an expression (SiftV6Ext.elem_array), else NULL: the column heap_att */
	SiftClause	where;
	bool		has_where;
	SiftVec   **wvecs;
	int		   *wcol;			/* the column of each vector the WHERE clause reads */
	SiftClause	ewhere;
	bool		has_ewhere;
	int			ecol[SIFT_V6_MAX_FCOLS];	/* the column of the element each vector of the element's clause is */
	int			nevec;
	void	   *ebuf[SIFT_V6_MAX_FCOLS];
	uint64	   *evalid[SIFT_V6_MAX_FCOLS];
	SiftVec		evin[SIFT_V6_MAX_FCOLS];
	SiftVec	   *evp[SIFT_V6_MAX_FCOLS];
	SiftVec	   *out[OUT_SLOTS];
	uint64	   *out_valid[OUT_SLOTS];
	void	   *out_data[OUT_SLOTS];
	SiftVec		outv[OUT_SLOTS];
	SiftVec		fv[SIFT_F2_PULL_MAX_COLS];	/* the columns of the rows of a batch, those that are months computed */
	int64	   *mdata[SIFT_F2_PULL_MAX_COLS];
	uint64	   *mvalid[SIFT_F2_PULL_MAX_COLS];
	int32	   *month_key;
	int32	   *month_val;
	uint32	   *pidx;
	uint32	   *eidx;
	MemoryContext bcx;			/* the arrays read from the heap for a batch */
} UnRun;

static SiftVecType
vec_type(SiftV6Class cls)
{
	return cls == SV6C_F64 ? SVEC_F64 : cls == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
}

/* the leaf a column of the scan is: the one of a key of a jsonb document a qual names, the one jleaf names, or the first one of the attribute */
static int
leaf_of(const SiftColRel *r, const SiftV6Col *c)
{
	if (c->attno >= SIFT_V6_JLEAF_VATT)
		return c->attno - SIFT_V6_JLEAF_VATT;
	return c->jleaf > 0 ? c->jleaf - 1 : r->first[c->attno];
}

static int
col_index(UnRun *u, const SiftColRel *r, const SiftV6Col *c, bool month)
{
	const int	leaf = leaf_of(r, c);

	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].leaf == leaf && u->cols[i].month == month)
			return i;
	if (u->ncols == SIFT_F2_PULL_MAX_COLS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the aggregate scan of an unnest reads at most %d columns of the table", SIFT_F2_PULL_MAX_COLS),
						errhint("Aggregate fewer columns of the table that is unnested.")));
	u->cols[u->ncols] = (UnCol) {.leaf = leaf,.cls = sift_v6_class(c->typid),.month = month};
	return u->ncols++;
}

static bool
key_is_month(const SiftV6Spec *spec, int key)
{
	for (int b = 0; b < spec->nbins; b++)
		if (spec->bin[b].key == key)
			return true;
	return false;
}

/* the vector of the pipe a column of the statement is read from: a column of the table, the column of the element, or what the pairs compute from them */
static int
slot_of(UnRun *u, const SiftColRel *r, const SiftV6Col *c, bool month)
{
	if (c->derive > 0)
		return u->ncols + u->nel + c->derive - 1;
	if (c->src == SV6SRC_ELEM)
	{
		u->eused[c->slot] = true;
		return u->ncols + c->slot;
	}
	if (c->src == SV6SRC_DIM)
		return sift_v6_pipe_dim_slot(u->pipe, c->dim, c->slot);
	return col_index(u, r, c, month);
}

static void
setup_cols(UnRun *u, const SiftColRel *r)
{
	const SiftV6Spec *spec = u->spec;

	for (int i = 0; i < spec->nkeys; i++)
		if (spec->key[i].src == SV6SRC_ATTR && spec->key[i].derive == 0)
			col_index(u, r, &spec->key[i], key_is_month(spec, i));
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
		if (sift_v6_vec_col(spec, v) != NULL && sift_v6_vec_col(spec, v)->src == SV6SRC_ATTR && sift_v6_vec_col(spec, v)->derive == 0)
			col_index(u, r, sift_v6_vec_col(spec, v), false);
	for (int d = 0; d < u->x->ndims; d++)
		for (int k = 0; k < u->x->dim[d].nkeys; k++)
			if (sift_v6_xdim_key_is_col(&u->x->dim[d], k) && u->x->dim[d].fsrc[k] == SV6SRC_ATTR)
			{
				const SiftV6Col key = sift_v6_xdim_key_col(&u->x->dim[d], k);

				col_index(u, r, &key, false);
			}
	for (int i = 0; i < u->x->nrcol; i++)
		if (u->x->rcol[i].src == SV6SRC_ATTR)
			col_index(u, r, &u->x->rcol[i], false);
	for (int k = 0; k < spec->nderive; k++)
	{
		const SiftV6Derive *d = &spec->derive[k];

		for (int c = 0; c <= d->ncond; c++)
			if ((c == 0 ? d->src : d->cond[c - 1]).src == SV6SRC_ATTR)
				col_index(u, r, c == 0 ? &d->src : &d->cond[c - 1], false);
		if (d->kind == SVD_PG_EXPR)
		{
			SiftV6PgItem it;

			sift_v6_pgexpr_item(spec, d, &it);
			for (int c = 0; c < it.nin; c++)
				if (it.col[c].src == SV6SRC_ATTR)
					col_index(u, r, &it.col[c], false);
		}
	}
	for (int i = 0; u->x->where != NIL && i < spec->nfcols; i++)
		if (spec->fcol[i].src == SV6SRC_ATTR)
			col_index(u, r, &spec->fcol[i], false);
}

static SiftF2PullSpec
pull_spec(const UnRun *u)
{
	SiftF2PullSpec ps = {.ncols = u->ncols,.nmasks = 0,.group_leaf = u->x->elem.group_leaf,.nchild = u->nel};

	for (int i = 0; i < u->ncols; i++)
		ps.col[i] = (SiftF2PullCol) {.leaf = u->cols[i].leaf,.type = vec_type(u->cols[i].cls)};
	for (int c = 0; c < u->nel; c++)
		ps.child[c] = (SiftF2PullCol) {.leaf = u->x->elem.col_leaf[c],.type = u->etype[c]};
	return ps;
}

static SiftVecType
pg_vec_type(const SiftV6Spec *spec, const SiftV6Derive *d)
{
	SiftV6PgItem it;

	sift_v6_pgexpr_item(spec, d, &it);
	return sift_v6_result_vec_type(it.restype);
}

/* the evaluator of the derivation k, over the vectors of the pairs of its inputs */
static void
bind_pg(UnRun *u, const SiftColRel *r, int k)
{
	SiftV6PgItem it;
	SiftV6PgIn	pin[SIFT_V6_MAX_COLS];

	sift_v6_pgexpr_item(u->spec, &u->spec->derive[k], &it);
	for (int c = 0; c < it.nin; c++)
	{
		u->pgslot[k][c] = slot_of(u, r, &it.col[c], false);
		pin[c] = (SiftV6PgIn) {.typid = it.col[c].typid,.scale = it.col[c].scale};
	}
	u->pg[k] = sift_v6_pgexpr_open(CurrentMemoryContext, it.expr, it.nin, pin);
	u->pg[k]->restype = it.restype;
	sift_vx_out_init(&u->pgout[k], CurrentMemoryContext);
}

static void
setup_pipe(UnRun *u, const SiftColRel *r, SiftV6Group *g, const SiftV6StarRun *star)
{
	const SiftV6Spec *spec = u->spec;
	SiftVecType type[OUT_SLOTS];

	for (int i = 0; i < u->ncols; i++)
		type[i] = vec_type(u->cols[i].cls);
	for (int c = 0; c < u->nel; c++)
		type[u->ncols + c] = u->etype[c];
	for (int k = 0; k < spec->nderive; k++)
		type[u->ncols + u->nel + k] = spec->derive[k].kind == SVD_PG_EXPR ? pg_vec_type(spec, &spec->derive[k]) : SVEC_F64;
	if (u->x->elem.semi)
		type[u->row_slot = u->ncols + u->nel + spec->nderive] = SVEC_I64;
	u->pipe = sift_v6_pipe_new(CurrentMemoryContext, spec, g, NULL, u->ncols + u->nel + spec->nderive + (u->row_slot >= 0), type);
	if (u->row_slot >= 0)
		sift_v6_pipe_semi(u->pipe, u->row_slot);
	if (star != NULL)
		sift_v6_star_attach(star, u->pipe);
	for (int d = 0; d < u->x->ndims; d++)
		for (int k = 0; k < u->x->dim[d].nkeys; k++)
			if (sift_v6_xdim_key_is_col(&u->x->dim[d], k))
			{
				const SiftV6Col key = sift_v6_xdim_key_col(&u->x->dim[d], k);

				u->pipe->fslot[d][k] = slot_of(u, r, &key, false);
			}
	for (int i = 0; i < u->x->nrcol; i++)
		u->pipe->rslot[i] = slot_of(u, r, &u->x->rcol[i], false);
	for (int i = 0; i < spec->nkeys; i++)
		u->pipe->key_slot[i] = slot_of(u, r, &spec->key[i], key_is_month(spec, i));
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
	{
		const SiftV6Col *c = sift_v6_vec_col(spec, v);

		u->pipe->arg_slot[v] = c == NULL ? -1 : slot_of(u, r, c, false);
	}
	for (int k = 0; k < spec->nderive; k++)
	{
		const SiftV6Derive *d = &spec->derive[k];
		UnArith    *ar = &u->arith[k];

		if (d->kind == SVD_PG_EXPR)
		{
			bind_pg(u, r, k);
			continue;
		}
		ar->nin = d->ncond + 1;
		for (int c = 0; c < ar->nin; c++)
		{
			const SiftV6Col *col = c == 0 ? &d->src : &d->cond[c - 1];

			ar->in[c] = slot_of(u, r, col, false);
			ar->f64[c] = col->typid == FLOAT8OID;
		}
	}
	for (int s = 0; s < u->ncols + u->nel + spec->nderive + (u->row_slot >= 0); s++)
	{
		u->out_data[s] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sift_gather_width(type[s]) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		u->out_valid[s] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		u->out[s] = &u->outv[s];
	}
}

/* the month of a date the way PostgreSQL finds it: date_trunc('month', date)::date, each distinct date once */
static int32
month_of(UnRun *u, int32 day)
{
	uint32		at = ((uint32) day * 2654435761u) >> 20;

	for (uint32 probe = 0; probe < MONTH_SLOTS; probe++, at = (at + 1) % MONTH_SLOTS)
	{
		if (u->month_val[at] == 0 && u->month_key[at] == 0)
		{
			const Datum ts = DirectFunctionCall1(date_timestamptz, DateADTGetDatum(day));
			const Datum m = DirectFunctionCall2(timestamptz_trunc, CStringGetTextDatum("month"), ts);

			u->month_key[at] = day;
			u->month_val[at] = DatumGetDateADT(DirectFunctionCall1(timestamptz_date, m));
			return u->month_val[at];
		}
		if (u->month_key[at] == day)
			return u->month_val[at];
	}
	return DatumGetDateADT(DirectFunctionCall1(timestamptz_date, DirectFunctionCall2(timestamptz_trunc, CStringGetTextDatum("month"), DirectFunctionCall1(date_timestamptz, DateADTGetDatum(day)))));
}

/* the columns of the rows of the batch the statement computes from them, for the rows it keeps */
static void
compute_columns(UnRun *u, const SiftF2ExtBatch *b, const SiftBatchSel *bs)
{
	for (int i = 0; i < u->ncols; i++)
	{
		const SiftVec *src = &b->col[i];

		u->fv[i] = *src;
		if (!u->cols[i].month)
			continue;
		memset(u->mvalid[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
		for (uint32 w = 0; w < (uint32) ((b->n + 63) / 64); w++)
			for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
			{
				const uint32 r = w * 64 + (uint32) __builtin_ctzll(bits);

				if (src->all_valid || (src->valid[r >> 6] >> (r & 63)) & 1)
				{
					u->mdata[i][r] = month_of(u, (int32) ((const int64 *) src->data)[r]);
					u->mvalid[i][r >> 6] |= UINT64CONST(1) << (r & 63);
				}
			}
		u->fv[i] = (SiftVec) {.data = u->mdata[i],.valid = u->mvalid[i],.len = b->n,.all_valid = false,.dense = true,.type = SVEC_I64};
	}
}

/* the cell of a column of the pairs as a float8: a float8 column, or an integer PostgreSQL casts to float8 */
static inline double
cell_f64(const SiftVec *v, bool f64, uint32 r)
{
	return f64 ? ((const double *) v->data)[r] : (double) ((const int64 *) v->data)[r];
}

/* the arithmetic of derivation k on the m pairs: the operations of PostgreSQL's float8pl, float8mi and float8mul, which raise as they do, over the pairs whose columns are all there */
static void
arith_pairs(UnRun *u, int k, uint32 m)
{
	const SiftV6Derive *d = &u->spec->derive[k];
	const UnArith *ar = &u->arith[k];
	const int	slot = u->ncols + u->nel + k;
	double	   *out = u->out_data[slot];
	uint64	   *valid = u->out_valid[slot];
	double		kconst;

	memcpy(&kconst, &d->aux, sizeof(kconst));
	memset(valid, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint32 r = 0; r < m; r++)
	{
		double		st[SIFT_ARITH_MAX_STACK];
		int			sp = 0;
		bool		ok = true;

		for (int c = 0; c < ar->nin && ok; c++)
			ok = u->out[ar->in[c]]->all_valid || ((u->out[ar->in[c]]->valid[r >> 6] >> (r & 63)) & 1);
		if (!ok)
			continue;
		for (int t = 0;; t++)
		{
			const uint8 tok = (uint8) ((uint64) d->cval[t / 8] >> (8 * (t % 8)));

			if (tok == 0)
				break;
			if (tok >= SIFT_ARITH_COL && tok < SIFT_ARITH_CONST)
				st[sp++] = cell_f64(u->out[ar->in[tok - SIFT_ARITH_COL]], ar->f64[tok - SIFT_ARITH_COL], r);
			else if (tok == SIFT_ARITH_CONST)
				st[sp++] = kconst;
			else
			{
				const double b = st[--sp],
							a = st[--sp];

				st[sp++] = tok == SIFT_ARITH_ADD ? float8_pl(a, b) : tok == SIFT_ARITH_SUB ? float8_mi(a, b) : float8_mul(a, b);
			}
		}
		out[r] = st[0];
		valid[r >> 6] |= UINT64CONST(1) << (r & 63);
	}
	u->outv[slot] = (SiftVec) {.data = out,.valid = valid,.len = (uint16) m,.all_valid = false,.dense = true,.type = SVEC_F64};
}

/* the PostgreSQL expression of derivation k on the pairs of obs: its inputs are the vectors of the pairs, its result the vector of the slot of the derivation */
static void
pg_pairs(UnRun *u, int k)
{
	const SiftVec *in[SIFT_V6_MAX_COLS];
	const int	nin = u->pg[k]->nin;

	for (int c = 0; c < nin; c++)
		in[c] = u->out[u->pgslot[k][c]];
	sift_v6_pgexpr_derive(u->pg[k], in, &u->obs, &u->pgout[k]);
	u->out[u->ncols + u->nel + k] = &u->pgout[k].vec;
}

/* the number of the row of each pair, the one slot that tells an EXISTS which pairs are of one row */
static void
row_numbers(UnRun *u, uint32 m)
{
	int64	   *rows = u->out_data[u->row_slot];

	for (uint32 i = 0; i < m; i++)
		rows[i] = u->pidx[i];
	memset(u->out_valid[u->row_slot], 0xff, SIFT_VEC_WORDS * sizeof(uint64));
	u->outv[u->row_slot] = (SiftVec) {.data = rows,.valid = u->out_valid[u->row_slot],.len = (uint16) m,.all_valid = true,.dense = true,.type = SVEC_I64};
}

/* the pairs of (row, element) into the vectors of the pairs, and on to the groups: pidx[i] names the row of fact and eidx[i] the element of elems of pair i (SIFT_GATHER_NONE: a NULL one) */
static void
emit_pairs(UnRun *u, const SiftVec *fact, const SiftVec *elems, uint32 m)
{
	SiftBatchSel *bs = &u->obs;

	for (int i = 0; i < u->ncols; i++)
		sift_vec_gather(u->out[i], u->out_valid[i], u->out_data[i], &fact[i], u->pidx, m);
	for (int c = 0; c < u->nel; c++)
		if (u->eused[c])
			sift_vec_gather(u->out[u->ncols + c], u->out_valid[u->ncols + c], u->out_data[u->ncols + c], &elems[c], u->eidx, m);
	memset(bs->mask, 0, sizeof(bs->mask));
	for (uint32 i = 0; i < m; i++)
		bs->mask[i >> 6] |= UINT64CONST(1) << (i & 63);
	bs->n_raw = bs->n_active = (uint16) m;
	bs->all_active = true;
	bs->has_sel = false;
	for (int k = 0; k < u->spec->nderive; k++)
		if (u->spec->derive[k].kind == SVD_PG_EXPR)
			pg_pairs(u, k);
		else
			arith_pairs(u, k, m);
	if (u->row_slot >= 0)
		row_numbers(u, m);
	sift_v6_pipe_run(u->pipe, u->out, bs);
}

/* the elements the clause of the element keeps, as a bitmap of n_elems bits, from the columns ev of the elements */
static void
eval_elems(UnRun *u, const SiftVec *ev, uint32 n_elems, uint64 *keep_all)
{
	for (uint32 at = 0; at < n_elems; at += SIFT_VEC_LEN)
	{
		const uint32 cnt = Min(SIFT_VEC_LEN, n_elems - at);
		const uint32 words = (cnt + 63) / 64;
		uint64		keep[SIFT_VEC_WORDS];

		for (int v = 0; v < u->nevec; v++)
		{
			const SiftVec *e = &ev[u->ecol[v]];
			const size_t w = sift_gather_width(e->type);

			memcpy(u->ebuf[v], (const char *) e->data + (size_t) at * w, (size_t) cnt * w);
			memset(u->evalid[v], 0, SIFT_VEC_WORDS * sizeof(uint64));
			memcpy(u->evalid[v], e->valid + at / 64, words * sizeof(uint64));
			if (cnt & 63)
				u->evalid[v][words - 1] &= (UINT64CONST(1) << (cnt & 63)) - 1;
			u->evin[v] = (SiftVec) {.data = u->ebuf[v],.valid = u->evalid[v],.len = (uint16) cnt,.all_valid = false,.dense = true,.type = e->type};
		}
		sift_clause_eval(&u->ewhere, u->evp, cnt, keep);
		memcpy(keep_all + at / 64, keep, words * sizeof(uint64));
	}
}

/* the elements of the array of a row as columns: unnest(array) of a NULL or empty array has none */
static uint32
array_elems(UnRun *u, Datum d, SiftVec *ev)
{
	Datum	   *elems = NULL;
	bool	   *nulls = NULL;
	int			nel = 0;
	const size_t w = sift_gather_width(u->etype[0]);

	deconstruct_array(DatumGetArrayTypeP(d), u->x->elem.type, u->typlen, u->typbyval, u->typalign, &elems, &nulls, &nel);
	ev[0] = (SiftVec) {.type = u->etype[0],.all_valid = false};
	ev[0].data = MemoryContextAllocAligned(u->bcx, Max((size_t) nel, 1) * w + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	ev[0].valid = palloc0(((nel + 63) / 64 + 1) * sizeof(uint64));
	for (int k = 0; k < nel; k++)
	{
		if (nulls[k])
			continue;
		ev[0].valid[k >> 6] |= UINT64CONST(1) << (k & 63);
		if (u->etype[0] == SVEC_STRVIEW)
		{
			struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(elems[k]));

			((SiftStrView *) ev[0].data)[k] = sift_vstr_make(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
		}
		else
			((int64 *) ev[0].data)[k] = u->x->elem.type == INT2OID ? DatumGetInt16(elems[k]) : u->x->elem.type == INT4OID ? DatumGetInt32(elems[k]) : DatumGetInt64(elems[k]);
	}
	return (uint32) nel;
}

static bool
any_kept(const uint64 *keep, uint32 nel)
{
	for (uint32 k = 0; k < nel; k++)
		if (keep == NULL || ((keep[k >> 6] >> (k & 63)) & 1))
			return true;
	return false;
}

/* the pairs of one row whose elements are in ev: those the bitmap keeps (NULL: all), and for a left join a NULL element when it has none; an EXISTS makes the row one pair when an element is kept */
static void
emit_row(UnRun *u, const SiftVec *fact, const SiftVec *ev, const uint64 *keep, uint32 nel, uint32 row)
{
	uint32		m = 0;

	if (u->x->elem.semi)
	{
		if (any_kept(keep, nel))
		{
			u->pidx[0] = row;
			u->eidx[0] = SIFT_GATHER_NONE;
			emit_pairs(u, fact, ev, 1);
		}
		return;
	}
	if (nel == 0 && u->x->elem.left)
	{
		u->pidx[0] = row;
		u->eidx[0] = SIFT_GATHER_NONE;
		emit_pairs(u, fact, ev, 1);
		return;
	}
	for (uint32 k = 0; k < nel; k++)
		if (keep == NULL || ((keep[k >> 6] >> (k & 63)) & 1))
		{
			u->pidx[m] = row;
			u->eidx[m++] = k;
			if (m == SIFT_VEC_LEN)
			{
				emit_pairs(u, fact, ev, m);
				m = 0;
			}
		}
	if (m > 0)
		emit_pairs(u, fact, ev, m);
}

/* a row whose array the index cannot hold: the array of its heap row taken apart as the function does, NULL and empty arrays included, errors included */
static void
emit_heap_row(UnRun *u, const SiftVec *fact, uint64 tid, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	SiftVec		ev[SIFT_V6_XELEM_COLS] = {0};
	uint64	   *keep = NULL;
	uint32		nel = 0;
	bool		isnull,
				found;
	Datum		d;

	found = sift_v6_vet_fetch(&u->vet, tid);
	SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its array is read");
	if (u->econtext != NULL)
	{
		u->econtext->ecxt_scantuple = u->vet.slot;
		ResetExprContext(u->econtext);
	}
	if (u->hqual != NULL && !ExecQual(u->hqual, u->econtext))
	{
		MemoryContextSwitchTo(old);
		return;
	}
	d = u->arr != NULL ? ExecEvalExprSwitchContext(u->arr, u->econtext, &isnull) : slot_getattr(u->vet.slot, u->heap_att, &isnull);
	if (!isnull)
		nel = u->rec != NULL ? sift_v6_recfn_call(u->rec, d, u->bcx, ev) : array_elems(u, d, ev);
	if (u->has_ewhere && nel > 0)
	{
		keep = palloc0(((nel + 63) / 64 + 1) * sizeof(uint64));
		eval_elems(u, ev, nel, keep);
	}
	if (u->rec != NULL)
		sift_v6_recfn_values(u->rec, keep, u->bcx, ev, nel);
	emit_row(u, fact, ev, keep, nel, row);
	MemoryContextSwitchTo(old);
}

static void
restrict_where(UnRun *u, const SiftF2ExtBatch *b)
{
	SiftBatchSel *bs = &u->bs;
	uint64		keep[SIFT_VEC_WORDS];

	for (int i = 0; i < u->spec->nfcols; i++)
		u->wvecs[i] = u->wcol[i] < 0 ? NULL : (SiftVec *) &b->col[u->wcol[i]];
	sift_clause_eval(&u->where, u->wvecs, b->n, keep);
	for (int w = 0; w < (b->n + 63) / 64; w++)
		bs->mask[w] &= keep[w] | b->elem->poison[w];
}

/* the rows of the batch with an element the clause of the element keeps: one pair for each, whose columns are those of the table; the rows the index cannot hold are the caller's, from the heap */
static void
emit_semi(UnRun *u, const SiftF2ExtBatch *b, const uint64 *keep, uint64 *fallback)
{
	const SiftF2ElemStream *es = b->elem;
	const uint32 words = (b->n + 63) / 64;
	uint64		held[SIFT_VEC_WORDS],
				yes[SIFT_VEC_WORDS],
				known[SIFT_VEC_WORDS];
	uint32		m = 0;

	for (uint32 w = 0; w < words; w++)
	{
		fallback[w] = u->bs.mask[w] & es->poison[w];
		held[w] = u->bs.mask[w] & ~es->poison[w];
	}
	sift_unnest_any(&es->layout, keep, held, es->doc_valid, yes, known);
	for (uint32 w = 0; w < words; w++)
		for (uint64 bits = yes[w]; bits != 0; bits &= bits - 1)
		{
			u->pidx[m] = w * 64 + (uint32) __builtin_ctzll(bits);
			u->eidx[m++] = SIFT_GATHER_NONE;
			if (m == SIFT_VEC_LEN)
			{
				emit_pairs(u, u->fv, NULL, m);
				m = 0;
			}
		}
	if (m > 0)
		emit_pairs(u, u->fv, NULL, m);
}

static void
run_batch(UnRun *u, const SiftF2ExtBatch *b)
{
	SiftBatchSel *bs = &u->bs;
	const int	n = b->n;
	const int	words = (n + 63) / 64;
	const SiftF2ElemStream *es = b->elem;
	uint64		fallback[SIFT_VEC_WORDS] = {0};
	SiftVec		ev[SIFT_V6_XELEM_COLS];

	u->stats->batches++;
	if (u->row_slot >= 0)
		sift_v6_pipe_semi_reset(u->pipe);
	memset(bs->mask, 0, sizeof(bs->mask));
	memcpy(bs->mask, b->live, words * sizeof(uint64));
	if (n & 63)
		bs->mask[words - 1] &= (UINT64CONST(1) << (n & 63)) - 1;
	if (u->has_where)
		restrict_where(u, b);
	if (sift_v6_vet_mask(&u->vet, b->tid, 0, false, n, bs) == 0)
		return;
	u->stats->rows += bs->n_active;
	MemoryContextReset(u->bcx);
	compute_columns(u, b, bs);
	for (int c = 0; c < u->nel; c++)
		ev[c] = (SiftVec) {.type = u->etype[c],.data = (void *) es->child_data[c],.valid = (uint64 *) es->child_valid[c],.all_valid = false};
	if (es->layout.n_parents < (uint32) n)
		memcpy(fallback, bs->mask, words * sizeof(uint64));
	else
	{
		uint64	   *keep = NULL;
		SiftUnnestIn in = {.lay = &es->layout,.parent_mask = bs->mask,.poison = es->poison,.doc_valid = es->doc_valid,.left = u->x->elem.left};
		SiftUnnestCursor cur = {0};

		if (u->has_ewhere)
		{
			keep = MemoryContextAllocZero(u->bcx, ((es->n_elems + 63) / 64 + 1) * sizeof(uint64));
			eval_elems(u, ev, es->n_elems, keep);
			in.elem_mask = keep;
		}
		if (u->x->elem.semi)
			emit_semi(u, b, keep, fallback);
		else
			while (!sift_unnest_done(&in, &cur))
			{
				const uint32 m = sift_unnest_expand(&in, &cur, u->pidx, u->eidx, SIFT_VEC_LEN, fallback);

				if (m > 0)
					emit_pairs(u, u->fv, ev, m);
			}
	}
	for (int w = 0; w < words; w++)
		for (uint64 bits = fallback[w] & bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 row = w * 64 + __builtin_ctzll(bits);

			emit_heap_row(u, u->fv, b->tid[row], row);
		}
}

static void
setup_clauses(UnRun *u, const SiftColRel *r)
{
	const SiftV6Spec *spec = u->spec;
	bool		bound;

	if (u->x->where != NIL)
	{
		u->has_where = true;
		bound = sift_v6_clause_bind(CurrentMemoryContext, u->x->where, spec, &u->where);
		SIFT_INVARIANT(bound, "the WHERE clause of the aggregate scan of an unnest compiled when it was planned");
		u->wvecs = palloc(Max(spec->nfcols, 1) * sizeof(SiftVec *));
		u->wcol = palloc(Max(spec->nfcols, 1) * sizeof(int));
		for (int i = 0; i < spec->nfcols; i++)
			u->wcol[i] = spec->fcol[i].src == SV6SRC_ATTR ? col_index(u, r, &spec->fcol[i], false) : -1;
	}
	if (u->x->elem_where != NIL)
	{
		u->has_ewhere = true;
		bound = sift_v6_unnest_elem_clause(CurrentMemoryContext, u->x->elem_where, &u->ewhere, u->ecol, &u->nevec);
		SIFT_INVARIANT(bound, "the clause of the element of the aggregate scan of an unnest compiled when it was planned");
		for (int v = 0; v < u->nevec; v++)
		{
			u->ebuf[v] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
			u->evalid[v] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
			u->evp[v] = &u->evin[v];
		}
	}
}

void
sift_v6_unnest_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, Snapshot snap, SiftV6ScanStats *stats, const SiftV6StarRun *star)
{
	UnRun		run = {0};
	UnRun	   *u = &run;
	SiftF2PullSpec ps;
	SiftF2Pull *pull;
	SiftV6MapPreagg *map;
	const SiftF2ExtBatch *b;

	u->spec = spec;
	u->row_slot = -1;
	u->x = spec->ext;
	u->stats = stats;
	u->nel = u->x->elem.ncols;
	for (int c = 0; c < u->nel; c++)
		u->etype[c] = vec_type(sift_v6_class(u->x->elem.col_type[c]));
	u->heap_att = index->rd_index->indkey.values[u->x->elem.attno - 1];
	if (u->x->elem.fn == SV6E_UNNEST)
		get_typlenbyvalalign(u->x->elem.type, &u->typlen, &u->typbyval, &u->typalign);
	else
		u->rec = sift_v6_recfn_new(CurrentMemoryContext, u->x);
	u->bcx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 unnest heap rows");
	if (u->x->heap_where != NIL || u->x->elem_array != NIL)
	{
		MemoryContext old = CurrentMemoryContext;

		u->estate = CreateExecutorState();
		MemoryContextSwitchTo(u->estate->es_query_cxt);
		u->hqual = u->x->heap_where != NIL ? ExecPrepareQual(u->x->heap_where, u->estate) : NULL;
		u->arr = u->x->elem_array != NIL ? ExecPrepareExpr(linitial(u->x->elem_array), u->estate) : NULL;
		u->econtext = CreateExprContext(u->estate);
		MemoryContextSwitchTo(old);
	}
	u->pidx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	u->eidx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	u->month_key = palloc0(MONTH_SLOTS * sizeof(int32));
	u->month_val = palloc0(MONTH_SLOTS * sizeof(int32));
	setup_cols(u, sift_col_rel(index));
	setup_clauses(u, sift_col_rel(index));
	setup_pipe(u, sift_col_rel(index), g, star);
	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].month)
		{
			u->mdata[i] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
			u->mvalid[i] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		}
	sift_v6_vet_begin(&u->vet, heap, snap, stats);
	ps = pull_spec(u);
	map = sift_v6_map_preagg_begin(spec, g, &u->vet, stats);
	ps.gran_sink = map != NULL ? sift_v6_map_preagg_try_granule : NULL;
	ps.gran_arg = map;
	pull = sift_f2_pull_begin(index, snap, &ps);
	while ((b = sift_f2_pull_next(pull)) != NULL)
		run_batch(u, b);
	sift_f2_pull_end(pull);
	if (map != NULL)
		sift_v6_map_preagg_end(map);
	sift_v6_pipe_finish(u->pipe);
	sift_v6_vet_end(&u->vet);
	if (u->estate != NULL)
		FreeExecutorState(u->estate);
	MemoryContextDelete(u->bcx);
}
