/*
 * agg/v6/v6_unnest_run.c - the aggregate scan of a LATERAL unnest of an array column (agg/v6/v6_unnest_run.h).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "access/tableam.h"
#include "executor/executor.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
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
#include "col/f2/f2_v6_unnest_test.h"
#include "core/errctx.h"
#include "core/memtrack.h"

#define OUT_SLOTS	(SIFT_F2_PULL_MAX_COLS + SIFT_V6_XELEM_COLS + SIFT_V6_MAX_DERIVE + 1 + SIFT_V6_XTESTS)
#define MONTH_SLOTS	4096

StaticAssertDecl(SIFT_V6_XELEM_COLS <= SIFT_F2_PULL_MAX_CHILD, "an element column is a child leaf of the pull");

typedef struct UnCol
{
	int			leaf;
	SiftV6Class cls;
	bool		month;			/* the month of the date, which PostgreSQL finds through timestamptz and the zone of the session: each distinct date is asked of it once */
	bool		json;			/* the leaf of a key of a jsonb document: a row that holds a value of another JSON type than the leaf's has it as the payload of an exception */
	bool		read;			/* the statement reads the column itself; it may be only the source of a member (keyed) */
	AttrNumber	heap_att;
	const char *key;
	int			keylen;
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
	Relation	index;
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
	Relation	index;
	bool		keyed;			/* SV6E_KEYS: a row of the table is one pair, the members its keys name */
	int			nchild;			/* the element columns of the pull and how each is decoded: those of the statement, or the keys and the values of the map when keyed */
	SiftVecType ctype[SIFT_V6_XELEM_COLS];
	FmgrInfo	jtext;			/* jsonb_object_field_text: the text of the key of a document read from the heap */
	ExprState  *cexpr[SIFT_V6_XELEM_COLS];	/* keyed: the expression each column is, with which a row of the heap is given its value */
	uint32	   *kidx[SIFT_V6_XELEM_COLS];	/* keyed: the member of the map each column of the pair has (SIFT_GATHER_NONE: none) */
	int			lcol[SIFT_V6_XELEM_COLS];	/* keyed: the column of the batch that is the leaf of a column that is no member of the map (-1: a member) */
	uint32	   *elem_of[SIFT_V6_XELEM_COLS];	/* keyed: the member each key names in each row of the batch */
	SiftClause	pwhere;
	bool		has_pwhere;
	int			pcol[SIFT_V6_MAX_FCOLS];
	int			npvec;
	SiftVec    *pvp[SIFT_V6_MAX_FCOLS];
	EState	   *estate;			/* the quals of the table as a row of the heap is tested with them */
	ExprContext *econtext;
	ExprState  *hqual;
	ExprState  *arr;			/* the array of a row of the heap when it is an expression (SiftV6Ext.elem_array), else NULL: the column heap_att */
	SiftClause	where;
	bool		has_where;
	SiftVec   **wvecs;
	int		   *wcol;			/* the column of each vector the WHERE clause reads */
	SiftF2ElemEval *eev;		/* the quals of the element of a lateral unnest */
	SiftF2RowTests *tests;		/* the tests of the elements of a row (SiftV6Ext.test), NULL without any */
	int			ntests;
	int			outer_slot[SIFT_V6_XTESTS * SIFT_V6_XCMPS];	/* the column of the batch each comparison of a test is with */
	AttrNumber	outer_att[SIFT_V6_XTESTS * SIFT_V6_XCMPS];	/* and its column of the heap */
	ExprState  *heap_test[SIFT_V6_XTESTS];	/* a jsonpath test: the call a row the leaves cannot hold is decided by */
	bool		flag_used[SIFT_V6_XTESTS];	/* a FILTER of the statement reads the flag of the test */
	uint64	   *flag_valid[SIFT_V6_XTESTS];
	int64	   *flag_data[SIFT_V6_XTESTS];
	SiftVec		flagv[SIFT_V6_XTESTS];	/* the flag of each row of the batch */
	uint64		held[SIFT_VEC_WORDS];	/* the rows of the batch the tests are decided for (those the layout holds) */
	uint64		yes[SIFT_V6_XTESTS][SIFT_VEC_WORDS];
	uint64		known[SIFT_V6_XTESTS][SIFT_VEC_WORDS];
	SiftVec	   *out[OUT_SLOTS];
	uint64	   *out_valid[OUT_SLOTS];
	void	   *out_data[OUT_SLOTS];
	SiftVec		outv[OUT_SLOTS];
	SiftVec		fv[SIFT_F2_PULL_MAX_COLS];	/* the columns of the rows of a batch, those that are months computed or keys of a document with the values of their exceptions */
	int64	   *mdata[SIFT_F2_PULL_MAX_COLS];
	uint64	   *mvalid[SIFT_F2_PULL_MAX_COLS];
	void	   *jdata[SIFT_F2_PULL_MAX_COLS];
	uint64	   *jvalid[SIFT_F2_PULL_MAX_COLS];
	SiftF2Pull *pull;
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

/* the leaf a column of the scan is: the one of a key of a jsonb document a qual or an expression names, the one jleaf names, or the first one of the attribute */
static int
leaf_of(const SiftColRel *r, const SiftV6Col *c)
{
	int			jleaf;
	bool		jtype;

	if (sift_v6_input_attno_json(c->attno, &jleaf, &jtype))
		return jleaf - 1;
	return c->jleaf > 0 ? c->jleaf - 1 : r->first[c->attno];
}

static int col_index(UnRun *u, const SiftColRel *r, const SiftV6Col *c, bool month);

/* the column of the batch that is leaf `leaf`, which the statement may not read itself: it is the source of the value of a member that is a column of the index */
static int
source_col(UnRun *u, const SiftColRel *r, int leaf)
{
	const SiftV6Col c = {.attno = 1,.typid = FLOAT8OID,.jleaf = leaf + 1};
	const int	before = u->ncols;
	const int	i = col_index(u, r, &c, false);

	if (u->ncols > before)
		u->cols[i].read = false;
	return i;
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
	u->cols[u->ncols] = (UnCol) {.leaf = leaf,.cls = sift_v6_class(c->typid),.month = month,.json = c->jleaf > 0 || c->attno >= SIFT_V6_JLEAF_VATT,.read = true};
	if (sift_f2_rel(u->index)->key_leaf[leaf])
	{
		const SiftF2Rel *f = sift_f2_rel(u->index);

		u->cols[u->ncols].json = true;
		u->cols[u->ncols].key = f->key[leaf];
		u->cols[u->ncols].keylen = f->key_len[leaf];
		u->cols[u->ncols].heap_att = u->index->rd_index->indkey.values[f->key_attno[leaf] - 1];
	}
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

/* the column is the flag of a test (the virtual attribute a FILTER reads it by) */
static bool
is_flag(const SiftV6Col *c)
{
	return c->src == SV6SRC_ATTR && c->derive == 0 && c->jleaf == 0 && c->attno >= SIFT_V6_XFLAG_VATT && c->attno < SIFT_V6_XFLAG_VATT + SIFT_V6_XTESTS;
}

/* the vector of the pipe the flag of test k is: after the derivations */
static int
flag_slot(const UnRun *u, int k)
{
	return u->ncols + u->nel + u->spec->nderive + k;
}

/* the vector of the pipe a column of the statement is read from: a column of the table, the column of the element, or what the pairs compute from them */
static int
slot_of(UnRun *u, const SiftColRel *r, const SiftV6Col *c, bool month)
{
	if (is_flag(c))
	{
		u->flag_used[c->attno - SIFT_V6_XFLAG_VATT] = true;
		return flag_slot(u, c->attno - SIFT_V6_XFLAG_VATT);
	}
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
		if (sift_v6_vec_col(spec, v) != NULL && sift_v6_vec_col(spec, v)->src == SV6SRC_ATTR && sift_v6_vec_col(spec, v)->derive == 0 && !is_flag(sift_v6_vec_col(spec, v)))
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
		if (spec->fcol[i].src == SV6SRC_ATTR && !is_flag(&spec->fcol[i]))
			col_index(u, r, &spec->fcol[i], false);
	for (int k = 0; k < u->ntests; k++)
		for (int c = 0; c < u->x->test[k].ncmps; c++)
		{
			const SiftV6XCmp *cmp = &u->x->test[k].cmp[c];
			const SiftV6Col outer = {.attno = cmp->outer,.typid = cmp->outer_type};

			u->outer_slot[k * SIFT_V6_XCMPS + c] = col_index(u, r, &outer, false);
		}
}

static SiftF2PullSpec
pull_spec(const UnRun *u)
{
	SiftF2PullSpec ps = {.ncols = u->ncols,.nmasks = 0,.group_leaf = u->x->elem.group_leaf,.nchild = u->nchild,.poison_rows = u->keyed};

	for (int i = 0; i < u->ncols; i++)
		ps.col[i] = (SiftF2PullCol) {.leaf = u->cols[i].leaf,.type = vec_type(u->cols[i].cls)};
	for (int c = 0; c < u->nchild; c++)
		ps.child[c] = (SiftF2PullCol) {.leaf = u->keyed ? (c == 0 ? u->x->elem.key_leaf : u->x->elem.val_leaf) : u->x->elem.col_leaf[c],.type = u->ctype[c],
									   .kind = !u->keyed && u->x->elem.col_kind[c]};
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
	const int	nslots = u->ncols + u->nel + spec->nderive + u->ntests + (u->x->elem.semi ? 1 : 0);

	for (int i = 0; i < u->ncols; i++)
		type[i] = vec_type(u->cols[i].cls);
	for (int c = 0; c < u->nel; c++)
		type[u->ncols + c] = u->etype[c];
	for (int k = 0; k < spec->nderive; k++)
		type[u->ncols + u->nel + k] = spec->derive[k].kind == SVD_PG_EXPR ? pg_vec_type(spec, &spec->derive[k]) : SVEC_F64;
	for (int k = 0; k < u->ntests; k++)
		type[flag_slot(u, k)] = SVEC_I64;
	if (u->x->elem.semi)
		type[u->row_slot = nslots - 1] = SVEC_I64;
	u->pipe = sift_v6_pipe_new(CurrentMemoryContext, spec, g, NULL, nslots, type);
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
	for (int s = 0; s < nslots; s++)
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

/* a leaf of a key of a document, with the value of the rows it could not hold read from the payload of their exception, for the rows the statement keeps */
static void
json_column(UnRun *u, int i, const SiftF2ExtBatch *b, const SiftBatchSel *bs)
{
	const SiftVec *src = &b->col[i];
	const size_t width = sift_gather_width(src->type);
	const int	words = (b->n + 63) / 64;

	memcpy(u->jdata[i], src->data, b->n * width);
	if (src->all_valid)
		memset(u->jvalid[i], 0xff, words * sizeof(uint64));
	else
		memcpy(u->jvalid[i], src->valid, words * sizeof(uint64));
	for (int w = 0; w < words; w++)
		for (uint64 bits = bs->mask[w] & ~u->jvalid[i][w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));
			const char *text = NULL;
			uint32		tlen = 0,
						len;
			double		f = 0;
			const char *payload = sift_f2_pull_payload(u->pull, i, r, &len);

			if (payload == NULL || !sift_v6_json_exception(u->cols[i].cls, payload, len, u->bcx, &text, &tlen, &f))
				continue;
			if (u->cols[i].cls == SV6C_F64)
				((double *) u->jdata[i])[r] = f;
			else
				((SiftStrView *) u->jdata[i])[r] = sift_vstr_make(text, tlen);
			u->jvalid[i][r >> 6] |= UINT64CONST(1) << (r & 63);
		}
	u->fv[i].data = u->jdata[i];
	u->fv[i].valid = u->jvalid[i];
	u->fv[i].all_valid = false;
}

/* the columns of the rows of the batch the statement computes from them, for the rows it keeps */
static void
compute_columns(UnRun *u, const SiftF2ExtBatch *b, const SiftBatchSel *bs)
{
	for (int i = 0; i < u->ncols; i++)
	{
		const SiftVec *src = &b->col[i];

		u->fv[i] = *src;
		if (u->cols[i].json)
			json_column(u, i, b, bs);
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

/* the pairs the quals over the members of the keys keep (SV6E_KEYS) */
static void
restrict_pairs(UnRun *u, uint32 m)
{
	SiftBatchSel *bs = &u->obs;
	uint64		keep[SIFT_VEC_WORDS];
	uint32		n = 0;

	for (int v = 0; v < u->npvec; v++)
		u->pvp[v] = u->out[u->ncols + u->pcol[v]];
	sift_clause_eval(&u->pwhere, u->pvp, m, keep);
	for (uint32 w = 0; w < (m + 63) / 64; w++)
	{
		bs->mask[w] &= keep[w];
		n += (uint32) sift_pc64(bs->mask[w]);
	}
	bs->n_active = (uint16) n;
	bs->all_active = n == m;
}

/* the pairs the columns of the pairs hold, the first m of them, on to the groups */
static void
run_pairs(UnRun *u, uint32 m)
{
	SiftBatchSel *bs = &u->obs;

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
	if (u->has_pwhere)
		restrict_pairs(u, m);
	if (bs->n_active > 0)
		sift_v6_pipe_run(u->pipe, u->out, bs);
}

/* the pairs of (row, element) into the vectors of the pairs, and on to the groups: pidx[i] names the row of fact and eidx[i] the element of elems of pair i (SIFT_GATHER_NONE: a NULL one) */
static void
emit_pairs(UnRun *u, const SiftVec *fact, const SiftVec *elems, uint32 m)
{
	for (int i = 0; i < u->ncols; i++)
		sift_vec_gather(u->out[i], u->out_valid[i], u->out_data[i], &fact[i], u->pidx, m);
	for (int c = 0; c < u->nel; c++)
		if (u->eused[c])
			sift_vec_gather(u->out[u->ncols + c], u->out_valid[u->ncols + c], u->out_data[u->ncols + c], &elems[c], u->eidx, m);
	for (int k = 0; k < u->ntests; k++)
		if (u->flag_used[k])
			sift_vec_gather(u->out[flag_slot(u, k)], u->out_valid[flag_slot(u, k)], u->out_data[flag_slot(u, k)], &u->flagv[k], u->pidx, m);
	run_pairs(u, m);
}

/*
 * The integer PostgreSQL's cast of the jsonb makes of the number the float stands for: numeric rounds half away from zero, as round() does (a float is exactly the boundary of two integers or
 * it is not, whatever its shortest decimal is). A float the type cannot hold is a row the heap is asked: the cast raises there, where the quals before it say the row reaches it.
 */
static bool
int_fits(double f, Oid type)
{
	const double r = round(f);

	return type == INT2OID ? (r >= -32768.0 && r <= 32767.0) : type == INT4OID ? (r >= -2147483648.0 && r <= 2147483647.0) :
		(r >= -9223372036854775808.0 && r < 9223372036854775808.0);
}

/* the pairs of the rows pidx names, each with the members its keys name (kidx): the values of the map are the float8 vector vals, and a column that is an integer takes the cast of its float */
static void
emit_keyed(UnRun *u, const SiftVec *fact, const SiftVec *vals, uint32 m)
{
	for (int i = 0; i < u->ncols; i++)
		sift_vec_gather(u->out[i], u->out_valid[i], u->out_data[i], &fact[i], u->pidx, m);
	for (int c = 0; c < u->nel; c++)
	{
		const int	s = u->ncols + c;
		const Oid	type = u->x->elem.col_type[c];

		if (u->lcol[c] >= 0)
			sift_vec_gather(u->out[s], u->out_valid[s], u->out_data[s], &fact[u->lcol[c]], u->pidx, m);
		else
			sift_vec_gather(u->out[s], u->out_valid[s], u->out_data[s], vals, u->kidx[c], m);
		if (type == FLOAT8OID)
			continue;
		for (uint32 i = 0; i < m; i++)
			if ((u->out_valid[s][i >> 6] >> (i & 63)) & 1)
				((int64 *) u->out_data[s])[i] = (int64) round(((double *) u->out_data[s])[i]);
		u->outv[s].type = SVEC_I64;
	}
	run_pairs(u, m);
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

/* the pairs of one row whose elements are in ev: those the bitmap keeps (NULL: all), and for a left join a NULL element when it has none */
static void
emit_row(UnRun *u, const SiftVec *fact, const SiftVec *ev, const uint64 *keep, uint32 nel, uint32 row)
{
	uint32		m = 0;

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

/* the row of the heap at tid is read, and the quals of the table as the statement has them hold of it */
static bool
heap_row_kept(UnRun *u, uint64 tid)
{
	const bool	found = sift_v6_vet_fetch(&u->vet, tid);

	SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its array is read");
	if (u->econtext != NULL)
	{
		u->econtext->ecxt_scantuple = u->vet.slot;
		ResetExprContext(u->econtext);
	}
	return u->hqual == NULL || ExecQual(u->hqual, u->econtext);
}

/* the array of the row read from the heap taken apart as the function does, NULL and empty arrays included, errors included: its elements in ev, and how many */
static uint32
heap_elems(UnRun *u, SiftVec *ev)
{
	bool		isnull;
	const Datum d = u->arr != NULL ? ExecEvalExprSwitchContext(u->arr, u->econtext, &isnull) : slot_getattr(u->vet.slot, u->heap_att, &isnull);

	if (isnull)
		return 0;
	return u->rec != NULL ? sift_v6_recfn_call(u->rec, d, u->bcx, ev) : array_elems(u, d, ev);
}

/* a row whose array the index cannot hold: the array of its heap row taken apart */
static void
emit_heap_row(UnRun *u, const SiftVec *fact, uint64 tid, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	SiftVec		ev[SIFT_V6_XELEM_COLS] = {0};
	uint64	   *keep = NULL;
	uint32		nel;

	if (heap_row_kept(u, tid))
	{
		nel = heap_elems(u, ev);
		if (u->eev != NULL && nel > 0)
		{
			keep = palloc0(((nel + 63) / 64 + 1) * sizeof(uint64));
			sift_f2_elem_eval(u->eev, ev, 0, nel, keep);
		}
		if (u->rec != NULL)
			sift_v6_recfn_values(u->rec, keep, u->bcx, ev, nel);
		emit_row(u, fact, ev, keep, nel, row);
	}
	MemoryContextSwitchTo(old);
}

typedef union HeapCell
{
	int64		i;
	double		f;
} HeapCell;

/* the value of the column of the heap row that comparison c of test k is with, as a vector of the one value */
static SiftVec
heap_outer(UnRun *u, int k, int c, HeapCell *cell, uint64 *valid)
{
	const Oid	type = u->x->test[k].cmp[c].outer_type;
	bool		isnull;
	const Datum d = slot_getattr(u->vet.slot, u->outer_att[k * SIFT_V6_XCMPS + c], &isnull);

	*valid = !isnull;
	if (!isnull)
	{
		if (type == FLOAT8OID)
			cell->f = DatumGetFloat8(d);
		else
			cell->i = type == INT2OID ? DatumGetInt16(d) : type == INT4OID ? DatumGetInt32(d) : DatumGetInt64(d);
	}
	return (SiftVec) {.data = cell,.valid = valid,.len = 1,.all_valid = false,.dense = true,.type = type == FLOAT8OID ? SVEC_F64 : SVEC_I64};
}

/* every test of the heap row: a jsonpath is the call itself, an EXISTS the test of the elements the function gives */
static void
heap_flags(UnRun *u, bool *yes, bool *known)
{
	SiftVec		ev[SIFT_V6_XELEM_COLS] = {0};
	uint32		nel = 0;
	bool		have = false;

	for (int k = 0; k < u->ntests; k++)
	{
		SiftVec		outer[SIFT_V6_XCMPS];
		HeapCell	cell[SIFT_V6_XCMPS];
		uint64		valid[SIFT_V6_XCMPS];
		bool		isnull;

		known[k] = true;
		yes[k] = false;
		if (u->heap_test[k] != NULL)
		{
			const Datum d = ExecEvalExprSwitchContext(u->heap_test[k], u->econtext, &isnull);

			known[k] = !isnull;
			yes[k] = !isnull && DatumGetBool(d);
			continue;
		}
		if (!have)
			nel = heap_elems(u, ev);
		have = true;
		for (int c = 0; c < u->x->test[k].ncmps; c++)
			outer[c] = heap_outer(u, k, c, &cell[c], &valid[c]);
		yes[k] = nel > 0 && sift_f2_row_test_one(u->tests, k, ev, nel, outer);
	}
}

static bool
need_met(const SiftV6XTest *xt, bool yes, bool known)
{
	return xt->need == 0 || (xt->need == 1 ? yes : xt->nullable ? known && !yes : !yes);
}

/* a row the leaves cannot hold: its tests from the heap row, the pair it gives when the statement keeps it */
static void
emit_test_heap_row(UnRun *u, uint64 tid, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	bool		yes[SIFT_V6_XTESTS],
				known[SIFT_V6_XTESTS];
	bool		kept = heap_row_kept(u, tid);

	if (kept)
		heap_flags(u, yes, known);
	for (int k = 0; kept && k < u->ntests; k++)
		kept = need_met(&u->x->test[k], yes[k], known[k]);
	for (int k = 0; kept && k < u->ntests; k++)
	{
		u->flag_data[k][row] = yes[k];
		u->flag_valid[k][row >> 6] |= (uint64) (!u->x->test[k].nullable || known[k]) << (row & 63);
	}
	if (kept)
	{
		u->pidx[0] = row;
		u->eidx[0] = SIFT_GATHER_NONE;
		emit_pairs(u, u->fv, NULL, 1);
	}
	MemoryContextSwitchTo(old);
}

/* the member of column c of the row as a float, NULL (false) when the row has none */
static bool
member_of(const UnRun *u, const SiftVec *vals, int c, uint32 row, bool has_doc, double *f)
{
	const SiftVec *v = u->lcol[c] >= 0 ? &u->fv[u->lcol[c]] : vals;
	const uint32 at = u->lcol[c] >= 0 ? row : has_doc ? u->elem_of[c][row] : SIFT_GATHER_NONE;

	if (at == SIFT_GATHER_NONE || !(v->all_valid || ((v->valid[at >> 6] >> (at & 63)) & 1)))
		return false;
	*f = ((const double *) v->data)[at];
	return true;
}

/* the integer columns of the row can all be the integers of their casts: the row that cannot is the heap's, where the cast raises when the row reaches it */
static bool
members_fit(const UnRun *u, const SiftVec *vals, uint32 row, bool has_doc)
{
	for (int c = 0; c < u->nel; c++)
	{
		double		f;

		if (u->x->elem.col_type[c] != FLOAT8OID && member_of(u, vals, c, row, has_doc, &f) && !int_fits(f, u->x->elem.col_type[c]))
			return false;
	}
	return true;
}

/* the rows of the batch the map's leaves hold: one pair each, with the member each key of the statement names; the rows whose map they cannot hold are for the heap */
static void
run_keyed(UnRun *u, const SiftF2ExtBatch *b, const SiftVec *vals, uint64 *fallback)
{
	const SiftF2ElemStream *es = b->elem;
	const int	words = (b->n + 63) / 64;
	uint64		rows[SIFT_VEC_WORDS];
	uint32		m = 0;

	for (int w = 0; w < words; w++)
	{
		fallback[w] = u->bs.mask[w] & es->poison[w];
		rows[w] = u->bs.mask[w] & ~es->poison[w];
	}
	for (int c = 0; c < u->nel; c++)
	{
		const char *key = strVal(list_nth(u->x->elem_names, c));

		if (u->lcol[c] < 0)
			sift_map_lookup(&es->layout, es->child_data[0], key, (uint32) strlen(key), rows, u->elem_of[c]);
	}
	for (int w = 0; w < words; w++)
		for (uint64 bits = rows[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 row = w * 64 + (uint32) __builtin_ctzll(bits);
			const bool	has_doc = (es->doc_valid[row >> 6] >> (row & 63)) & 1;

			if (!members_fit(u, vals, row, has_doc))
			{
				fallback[w] |= UINT64CONST(1) << (row & 63);
				continue;
			}
			u->pidx[m] = row;
			for (int c = 0; c < u->nel; c++)
				if (u->lcol[c] < 0)
					u->kidx[c][m] = has_doc ? u->elem_of[c][row] : SIFT_GATHER_NONE;
			if (++m == SIFT_VEC_LEN)
			{
				emit_keyed(u, u->fv, vals, m);
				m = 0;
			}
		}
	if (m > 0)
		emit_keyed(u, u->fv, vals, m);
}

/* the text of the key of a document of the heap row (or its float8) as the cell of a column of one pair: what the statement's `->>` gives, errors included */
static void
heap_json_cell(UnRun *u, int i)
{
	const UnCol *c = &u->cols[i];
	const bool	f64 = c->cls == SV6C_F64;
	SiftVec    *out = u->out[i];
	LOCAL_FCINFO(fcinfo, 2);
	bool		isnull;
	Datum		doc = slot_getattr(u->vet.slot, c->heap_att, &isnull);
	Datum		txt = 0;

	memset(u->out_valid[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
	*out = (SiftVec) {.data = u->out_data[i],.valid = u->out_valid[i],.len = 1,.all_valid = false,.dense = true,.type = f64 ? SVEC_F64 : SVEC_STRVIEW};
	if (isnull)
		return;
	InitFunctionCallInfoData(*fcinfo, &u->jtext, 2, InvalidOid, NULL, NULL);
	fcinfo->args[0] = (NullableDatum) {doc, false};
	fcinfo->args[1] = (NullableDatum) {PointerGetDatum(cstring_to_text_with_len(c->key, c->keylen)), false};
	txt = FunctionCallInvoke(fcinfo);
	if (fcinfo->isnull)
		return;
	u->out_valid[i][0] = 1;
	if (f64)
		((double *) u->out_data[i])[0] = DatumGetFloat8(DirectFunctionCall1(float8in, CStringGetDatum(TextDatumGetCString(txt))));
	else
	{
		struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(txt));
		const uint32 len = (uint32) VARSIZE_ANY_EXHDR(t);
		char	   *copy = MemoryContextAlloc(u->bcx, Max(len, 1));

		memcpy(copy, VARDATA_ANY(t), len);
		((SiftStrView *) u->out_data[i])[0] = sift_vstr_make(copy, len);
	}
}

/* the member the expression of column c of the statement gives for the heap row, as the cell of a column of one pair */
static void
heap_member_cell(UnRun *u, int c)
{
	const int	s = u->ncols + c;
	const Oid	type = u->x->elem.col_type[c];
	bool		isnull;
	Datum		d = ExecEvalExprSwitchContext(u->cexpr[c], u->econtext, &isnull);

	memset(u->out_valid[s], 0, SIFT_VEC_WORDS * sizeof(uint64));
	u->outv[s] = (SiftVec) {.data = u->out_data[s],.valid = u->out_valid[s],.len = 1,.all_valid = false,.dense = true,.type = type == FLOAT8OID ? SVEC_F64 : SVEC_I64};
	if (isnull)
		return;
	u->out_valid[s][0] = 1;
	if (type == FLOAT8OID)
		((double *) u->out_data[s])[0] = DatumGetFloat8(d);
	else
		((int64 *) u->out_data[s])[0] = type == INT2OID ? DatumGetInt16(d) : type == INT4OID ? DatumGetInt32(d) : DatumGetInt64(d);
}

/* a row whose map the index cannot hold: the row of the heap is the pair, each column read from it as the statement reads it, errors included */
static void
emit_heap_keys(UnRun *u, const SiftVec *fact, uint64 tid, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	const bool	found = sift_v6_vet_fetch(&u->vet, tid);

	SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its map is read");
	u->econtext->ecxt_scantuple = u->vet.slot;
	ResetExprContext(u->econtext);
	if (u->hqual == NULL || ExecQual(u->hqual, u->econtext))
	{
		u->pidx[0] = row;
		for (int i = 0; i < u->ncols; i++)
			if (u->cols[i].json && u->cols[i].read)
				heap_json_cell(u, i);
			else
				sift_vec_gather(u->out[i], u->out_valid[i], u->out_data[i], &fact[i], u->pidx, 1);
		for (int c = 0; c < u->nel; c++)
			heap_member_cell(u, c);
		run_pairs(u, 1);
	}
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

static void
elem_vecs(const UnRun *u, const SiftF2ElemStream *es, SiftVec *ev)
{
	for (int c = 0; c < u->nchild; c++)
		ev[c] = (SiftVec) {.type = u->ctype[c],.data = (void *) es->child_data[c],.valid = (uint64 *) es->child_valid[c],.all_valid = false};
}

/* the tests of the rows of the batch the layout holds, and the rows of those that fail one the statement keeps its rows by leave the batch; the others are the heap's */
static void
decide_tests(UnRun *u, const SiftF2ExtBatch *b)
{
	const SiftF2ElemStream *es = b->elem;
	const uint32 words = (b->n + 63) / 64;
	SiftVec		ev[SIFT_V6_XELEM_COLS];

	memset(u->held, 0, sizeof(u->held));
	if (es->layout.n_parents < (uint32) b->n)
		return;
	for (uint32 w = 0; w < words; w++)
		u->held[w] = u->bs.mask[w] & ~es->poison[w];
	elem_vecs(u, es, ev);
	sift_f2_row_tests_eval(u->tests, es, ev, b->col, u->outer_slot, u->held, u->yes, u->known);
	for (int k = 0; k < u->ntests; k++)
	{
		const SiftV6XTest *xt = &u->x->test[k];

		for (uint32 w = 0; w < words; w++)
		{
			const uint64 pass = xt->need == 0 ? ~UINT64CONST(0) : xt->need == 1 ? u->yes[k][w] : xt->nullable ? u->known[k][w] & ~u->yes[k][w] : ~u->yes[k][w];

			u->bs.mask[w] &= ~u->held[w] | pass;
		}
	}
}

/* the flag of each row of the batch the statement's FILTERs read, for the rows the layout holds */
static void
fill_flags(UnRun *u, uint32 n)
{
	const uint32 words = (n + 63) / 64;

	for (int k = 0; k < u->ntests; k++)
	{
		if (!u->flag_used[k])
			continue;
		for (uint32 r = 0; r < n; r++)
			u->flag_data[k][r] = (u->yes[k][r >> 6] >> (r & 63)) & 1;
		for (uint32 w = 0; w < words; w++)
			u->flag_valid[k][w] = u->held[w] & (u->x->test[k].nullable ? u->known[k][w] : ~UINT64CONST(0));
		u->flagv[k] = (SiftVec) {.data = u->flag_data[k],.valid = u->flag_valid[k],.len = (uint16) n,.all_valid = false,.dense = true,.type = SVEC_I64};
	}
}

/* the rows of the batch the tests keep: one pair each, whose columns are those of the table and the flags; the rows the index cannot hold are the caller's, from the heap */
static void
emit_tested(UnRun *u, const SiftF2ExtBatch *b, uint64 *fallback)
{
	const uint32 words = (b->n + 63) / 64;
	uint32		m = 0;

	fill_flags(u, b->n);
	for (uint32 w = 0; w < words; w++)
	{
		fallback[w] = u->bs.mask[w] & ~u->held[w];
		for (uint64 bits = u->bs.mask[w] & u->held[w]; bits != 0; bits &= bits - 1)
		{
			u->pidx[m] = w * 64 + (uint32) __builtin_ctzll(bits);
			u->eidx[m++] = SIFT_GATHER_NONE;
			if (m == SIFT_VEC_LEN)
			{
				emit_pairs(u, u->fv, NULL, m);
				m = 0;
			}
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
	if (u->ntests > 0)
		decide_tests(u, b);
	if (sift_v6_vet_mask(&u->vet, b->tid, 0, false, n, bs) == 0)
		return;
	u->stats->rows += bs->n_active;
	MemoryContextReset(u->bcx);
	compute_columns(u, b, bs);
	elem_vecs(u, es, ev);
	if (u->keyed && u->x->elem.group_leaf < 0)
		run_keyed(u, b, NULL, fallback);
	else if (u->ntests > 0)
		emit_tested(u, b, fallback);
	else if (es->layout.n_parents < (uint32) n)
		memcpy(fallback, bs->mask, words * sizeof(uint64));
	else if (u->keyed)
		run_keyed(u, b, &ev[1], fallback);
	else
	{
		uint64	   *keep = NULL;
		SiftUnnestIn in = {.lay = &es->layout,.parent_mask = bs->mask,.poison = es->poison,.doc_valid = es->doc_valid,.left = u->x->elem.left};
		SiftUnnestCursor cur = {0};

		if (u->eev != NULL)
		{
			keep = MemoryContextAllocZero(u->bcx, ((es->n_elems + 63) / 64 + 1) * sizeof(uint64));
			sift_f2_elem_eval(u->eev, ev, 0, es->n_elems, keep);
			in.elem_mask = keep;
		}
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

			if (u->keyed)
				emit_heap_keys(u, u->fv, b->tid[row], row);
			else if (u->ntests > 0)
				emit_test_heap_row(u, b->tid[row], row);
			else
				emit_heap_row(u, u->fv, b->tid[row], row);
		}
}

static void
setup_clauses(UnRun *u, const SiftColRel *r)
{
	const SiftV6Spec *spec = u->spec;
	bool		bound;
	int			k = 0;

	if (u->x->where != NIL)
	{
		u->has_where = true;
		bound = sift_v6_clause_bind(CurrentMemoryContext, u->x->where, spec, &u->where);
		SIFT_INVARIANT(bound, "the WHERE clause of the aggregate scan of an unnest compiled when it was planned");
		u->wvecs = palloc(Max(spec->nfcols, 1) * sizeof(SiftVec *));
		u->wcol = palloc(Max(spec->nfcols, 1) * sizeof(int));
		for (int i = 0; i < spec->nfcols; i++)
			u->wcol[i] = spec->fcol[i].src == SV6SRC_ATTR && !is_flag(&spec->fcol[i]) ? col_index(u, r, &spec->fcol[i], false) : -1;
	}
	if (u->x->pair_where != NIL)
	{
		u->has_pwhere = true;
		bound = sift_v6_unnest_elem_clause(CurrentMemoryContext, u->x->pair_where, &u->pwhere, u->pcol, &u->npvec);
		SIFT_INVARIANT(bound, "the quals over the members of the keys of the aggregate scan compiled when it was planned");
	}
	if (u->x->elem_where != NIL)
		u->eev = sift_f2_elem_eval_open(CurrentMemoryContext, u->x->elem_where);
	if (u->ntests == 0)
		return;
	u->tests = sift_f2_row_tests_open(CurrentMemoryContext, u->x);
	foreach_ptr(List, tq, u->x->test_quals)
	{
		for (int c = 0; c < u->x->test[k].ncmps; c++)
			u->outer_att[k * SIFT_V6_XCMPS + c] = u->index->rd_index->indkey.values[u->x->test[k].cmp[c].outer - 1];
		if (lsecond(tq) != NIL)
			u->heap_test[k] = ExecPrepareExpr(linitial(lsecond(tq)), u->estate);
		u->flag_data[k] = palloc0(SIFT_VEC_LEN * sizeof(int64));
		u->flag_valid[k] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		k++;
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
	u->index = index;
	u->keyed = u->x->elem.fn == SV6E_KEYS;
	u->nel = u->x->elem.ncols;
	u->nchild = u->keyed ? (u->x->elem.group_leaf >= 0 ? 2 : 0) : u->nel;
	u->ntests = u->x->ntests;
	for (int c = 0; c < u->nel; c++)
		u->etype[c] = vec_type(sift_v6_class(u->x->elem.col_type[c]));
	for (int c = 0; c < u->nchild; c++)
		u->ctype[c] = u->keyed ? (c == 0 ? SVEC_STRVIEW : SVEC_F64) : u->etype[c];
	if (!u->keyed)
		u->heap_att = index->rd_index->indkey.values[u->x->elem.attno - 1];
	if (u->x->elem.fn == SV6E_UNNEST)
		get_typlenbyvalalign(u->x->elem.type, &u->typlen, &u->typbyval, &u->typalign);
	else if (!u->keyed && u->x->elem.fn != SV6E_PATH)
		u->rec = sift_v6_recfn_new(CurrentMemoryContext, u->x);
	u->bcx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 unnest heap rows");
	if (u->x->heap_where != NIL || u->x->elem_array != NIL || u->keyed || u->ntests > 0)
	{
		MemoryContext old = CurrentMemoryContext;

		u->estate = CreateExecutorState();
		MemoryContextSwitchTo(u->estate->es_query_cxt);
		u->hqual = u->x->heap_where != NIL ? ExecPrepareQual(u->x->heap_where, u->estate) : NULL;
		u->arr = u->x->elem_array != NIL ? ExecPrepareExpr(linitial(u->x->elem_array), u->estate) : NULL;
		for (int c = 0; u->keyed && c < u->nel; c++)
			u->cexpr[c] = ExecPrepareExpr(list_nth(u->x->col_expr, c), u->estate);
		u->econtext = CreateExprContext(u->estate);
		MemoryContextSwitchTo(old);
	}
	if (u->keyed)
	{
		fmgr_info(F_JSONB_OBJECT_FIELD_TEXT, &u->jtext);
		for (int c = 0; c < u->nel; c++)
		{
			u->kidx[c] = palloc(SIFT_VEC_LEN * sizeof(uint32));
			u->elem_of[c] = palloc(SIFT_VEC_LEN * sizeof(uint32));
		}
	}
	u->pidx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	u->eidx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	u->month_key = palloc0(MONTH_SLOTS * sizeof(int32));
	u->month_val = palloc0(MONTH_SLOTS * sizeof(int32));
	setup_cols(u, sift_col_rel(index));
	setup_clauses(u, sift_col_rel(index));
	for (int c = 0; c < u->nel; c++)
		u->lcol[c] = u->keyed && u->x->elem.col_leaf[c] >= 0 ? source_col(u, sift_col_rel(index), u->x->elem.col_leaf[c]) : -1;
	setup_pipe(u, sift_col_rel(index), g, star);
	for (int i = 0; i < u->ncols; i++)
	{
		if (u->cols[i].month)
		{
			u->mdata[i] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
			u->mvalid[i] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		}
		if (u->cols[i].json)
		{
			u->jdata[i] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
			u->jvalid[i] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		}
	}
	sift_v6_vet_begin(&u->vet, heap, snap, stats);
	ps = pull_spec(u);
	map = sift_v6_map_preagg_begin(spec, g, &u->vet, stats);
	ps.gran_sink = map != NULL ? sift_v6_map_preagg_try_granule : NULL;
	ps.gran_arg = map;
	pull = sift_f2_pull_begin(index, snap, &ps);
	u->pull = pull;
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
