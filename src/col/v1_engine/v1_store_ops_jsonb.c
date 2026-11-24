/*
 * col/v1_engine/v1_store_ops_jsonb.c - SiftJsonbOps of Format 1 (core/store_ops.h).
 *
 * key_leaf: the shredded scalar leaf of a top-level key of a document whose chunks keep the JSON type of each row (a path declared with a type or as text), -1 for any other key.
 * unnest_run: the aggregate scan of a LATERAL unnest of an array column (agg/v6/v6_unnest_run.h). The columns of the table the statement reads come from the batches of the Format 1 source, which
 * hands over only the rows the tombstones and the snapshot leave; the array of each row that passes the quals of the table is read from its heap row and taken apart as PostgreSQL does (NULL and
 * empty arrays, NULL elements, a jsonb document that is no array and the errors it raises included), and the pairs of (row, element) go through the pipe of the engine into the groups.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "access/tableam.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/v6_gather.h"
#include "agg/v6/v6_pipe.h"
#include "agg/v6/v6_plan_rewrite.h"
#include "agg/v6/v6_star_run.h"
#include "agg/v6/v6_unnest_jsonb.h"
#include "agg/v6/v6_vexpr.h"
#include "agg/vexpr_str.h"
#include "col/v1/v1_store_ops.h"
#include "table/v1/v1_tbl_ops.h"
#include "core/errctx.h"
#include "core/memtrack.h"

#define OUT_SLOTS	(SIFT_V6_MAX_COLS + SIFT_V6_XELEM_COLS + SIFT_V6_MAX_DERIVE)
#define MONTH_SLOTS	4096

typedef struct UnCol
{
	int			attno;
	Oid			typid;
	SiftV6Class cls;
	bool		month;			/* the month of the date, which PostgreSQL finds through timestamptz and the zone of the session: each distinct date is asked of it once */
} UnCol;

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
	SiftV6Pipe *pipe;
	SiftV6Source *src;
	SiftBatchSel bs;
	SiftBatchSel obs;
	int			ncols;
	UnCol		cols[SIFT_V6_MAX_COLS];
	UnArith		arith[SIFT_V6_MAX_DERIVE];
	int			nel;
	SiftVecType etype[SIFT_V6_XELEM_COLS];
	bool		eused[SIFT_V6_XELEM_COLS];
	AttrNumber	heap_att;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	SiftV6RecFn *rec;
	SiftClause	where;
	bool		has_where;
	SiftVec   **wvecs;
	int		   *wcol;
	SiftClause	ewhere;
	bool		has_ewhere;
	int			ecol[SIFT_V6_MAX_FCOLS];
	int			nevec;
	void	   *ebuf[SIFT_V6_MAX_FCOLS];
	uint64	   *evalid[SIFT_V6_MAX_FCOLS];
	SiftVec		evin[SIFT_V6_MAX_FCOLS];
	SiftVec	   *evp[SIFT_V6_MAX_FCOLS];
	SiftVec	   *out[OUT_SLOTS];
	uint64	   *out_valid[OUT_SLOTS];
	void	   *out_data[OUT_SLOTS];
	SiftVec		outv[OUT_SLOTS];
	SiftVec		fv[SIFT_V6_MAX_COLS];
	int64	   *mdata[SIFT_V6_MAX_COLS];
	uint64	   *mvalid[SIFT_V6_MAX_COLS];
	int32	   *month_key;
	int32	   *month_val;
	uint32	   *pidx;
	uint32	   *eidx;
	MemoryContext bcx;			/* the array of the row being taken apart */
} UnRun;

static bool
typed_leaf(Oid typid)
{
	return typid == BOOLOID || typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == FLOAT4OID || typid == FLOAT8OID || typid == NUMERICOID || typid == TEXTOID;
}

static int
jsonb_key_leaf(Oid indexoid, int att, const char *key, int len)
{
	SiftV1Rel	r;
	const SiftSrcLayout *lay;
	int			found = -1;

	if (get_rel_relkind(indexoid) != RELKIND_INDEX)
		return -1;
	r = sift_v1_rel_open(indexoid, false);
	lay = sift_v1_idx_reader.layout(r.rel);
	for (int i = 0; i < lay->nleaves && found < 0; i++)
	{
		const SiftSrcLeaf *l = &lay->leaf[i];

		if (l->attno == att && l->parent < 0 && l->kind == SCL_SCALAR && !l->lossy && l->name[0] != '#' && typed_leaf(l->typid) && list_length(l->path) == 1 &&
			strncmp(strVal(linitial(l->path)), key, len) == 0 && strVal(linitial(l->path))[len] == '\0')
			found = i;
	}
	sift_v1_rel_close(&r);
	return found;
}

static SiftVecType
vec_type(SiftV6Class cls)
{
	return cls == SV6C_F64 ? SVEC_F64 : cls == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
}

static bool
key_is_month(const SiftV6Spec *spec, int key)
{
	for (int b = 0; b < spec->nbins; b++)
		if (spec->bin[b].key == key)
			return true;
	return false;
}

/* the column of the batches a column of the statement is read from */
static int
col_index(UnRun *u, const SiftV6Col *c, bool month)
{
	const SiftV6Class cls = sift_v6_class(c->typid);

	if (c->attno >= SIFT_V6_JLEAF_VATT || c->jleaf > 0 || c->typid == NUMERICOID || cls == SV6C_DATUM || cls == SV6C_NONE)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("the aggregate scan of an unnest over Format 1 reads no column of type %u or key of a document", c->typid),
						errhint("Group by and aggregate integer, date, timestamp, float8 or text columns of the table that is unnested.")));
	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].attno == c->attno && u->cols[i].month == month)
			return i;
	if (u->ncols == SIFT_V6_MAX_COLS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the aggregate scan of an unnest reads at most %d columns of the table", SIFT_V6_MAX_COLS),
						errhint("Aggregate fewer columns of the table that is unnested.")));
	u->cols[u->ncols] = (UnCol) {.attno = c->attno,.typid = c->typid,.cls = cls,.month = month};
	return u->ncols++;
}

/* the vector of the pipe a column of the statement is read from: a column of the table, the column of the element, or what the pairs compute from them */
static int
slot_of(UnRun *u, const SiftV6Col *c, bool month)
{
	if (c->derive > 0)
		return u->ncols + u->nel + c->derive - 1;
	if (c->src == SV6SRC_DIM)
		return sift_v6_pipe_dim_slot(u->pipe, c->dim, c->slot);
	if (c->src == SV6SRC_ELEM)
	{
		u->eused[c->slot] = true;
		return u->ncols + c->slot;
	}
	return col_index(u, c, month);
}

static void
setup_cols(UnRun *u)
{
	const SiftV6Spec *spec = u->spec;

	for (int i = 0; i < spec->nkeys; i++)
		if (spec->key[i].src == SV6SRC_ATTR)
			col_index(u, &spec->key[i], key_is_month(spec, i));
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
		if (sift_v6_vec_col(spec, v) != NULL && sift_v6_vec_col(spec, v)->src == SV6SRC_ATTR && sift_v6_vec_col(spec, v)->derive == 0)
			col_index(u, sift_v6_vec_col(spec, v), false);
	for (int d = 0; d < u->x->ndims; d++)
		for (int k = 0; k < u->x->dim[d].nkeys; k++)
			if (u->x->dim[d].fsrc[k] == SV6SRC_ATTR)
			{
				const SiftV6Col key = sift_v6_xdim_key_col(&u->x->dim[d], k);

				col_index(u, &key, false);
			}
	for (int i = 0; i < u->x->nrcol; i++)
		if (u->x->rcol[i].src == SV6SRC_ATTR)
			col_index(u, &u->x->rcol[i], false);
	for (int k = 0; k < spec->nderive; k++)
	{
		const SiftV6Derive *d = &spec->derive[k];

		for (int c = 0; c <= d->ncond; c++)
			if ((c == 0 ? d->src : d->cond[c - 1]).src == SV6SRC_ATTR)
				col_index(u, c == 0 ? &d->src : &d->cond[c - 1], false);
	}
	for (int i = 0; u->x->where != NIL && i < spec->nfcols; i++)
		col_index(u, &spec->fcol[i], false);
}

static void
setup_pipe(UnRun *u, SiftV6Group *g, const SiftV6StarRun *star)
{
	const SiftV6Spec *spec = u->spec;
	SiftVecType type[OUT_SLOTS];

	for (int i = 0; i < u->ncols; i++)
		type[i] = vec_type(u->cols[i].cls);
	for (int c = 0; c < u->nel; c++)
		type[u->ncols + c] = u->etype[c];
	for (int k = 0; k < spec->nderive; k++)
		type[u->ncols + u->nel + k] = SVEC_F64;
	u->pipe = sift_v6_pipe_new(CurrentMemoryContext, spec, g, u->ncols + u->nel + spec->nderive, type);
	if (star != NULL)
		sift_v6_star_attach(star, u->pipe);
	for (int d = 0; d < u->x->ndims; d++)
		for (int k = 0; k < u->x->dim[d].nkeys; k++)
		{
			const SiftV6Col key = sift_v6_xdim_key_col(&u->x->dim[d], k);

			u->pipe->fslot[d][k] = slot_of(u, &key, false);
		}
	for (int i = 0; i < u->x->nrcol; i++)
		u->pipe->rslot[i] = slot_of(u, &u->x->rcol[i], false);
	for (int i = 0; i < spec->nkeys; i++)
		u->pipe->key_slot[i] = slot_of(u, &spec->key[i], key_is_month(spec, i));
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
	{
		const SiftV6Col *c = sift_v6_vec_col(spec, v);

		u->pipe->arg_slot[v] = c == NULL ? -1 : slot_of(u, c, false);
	}
	for (int k = 0; k < spec->nderive; k++)
	{
		const SiftV6Derive *d = &spec->derive[k];
		UnArith    *ar = &u->arith[k];

		ar->nin = d->ncond + 1;
		for (int c = 0; c < ar->nin; c++)
		{
			const SiftV6Col *col = c == 0 ? &d->src : &d->cond[c - 1];

			ar->in[c] = slot_of(u, col, false);
			ar->f64[c] = col->typid == FLOAT8OID;
		}
	}
	for (int s = 0; s < u->ncols + u->nel + spec->nderive; s++)
	{
		u->out_data[s] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sift_gather_width(type[s]) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		u->out_valid[s] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		u->out[s] = &u->outv[s];
	}
}

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
compute_columns(UnRun *u, const SiftV6Batch *b, const SiftBatchSel *bs)
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
	for (int k = 0; k < u->spec->nderive; k++)
		arith_pairs(u, k, m);
	memset(bs->mask, 0, sizeof(bs->mask));
	for (uint32 i = 0; i < m; i++)
		bs->mask[i >> 6] |= UINT64CONST(1) << (i & 63);
	bs->n_raw = bs->n_active = (uint16) m;
	bs->all_active = true;
	bs->has_sel = false;
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

/* a row of the batch: the array of its heap row taken apart as the function does, NULL and empty arrays included, errors included */
static void
emit_heap_row(UnRun *u, const SiftV6Batch *b, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	SiftVec		ev[SIFT_V6_XELEM_COLS] = {0};
	uint64	   *keep = NULL;
	uint32		nel = 0;
	bool		isnull,
				found;
	Datum		d;

	MemoryContextReset(u->bcx);
	found = u->src->fetch(u->src, b->tag[row], &u->heap_att, 1, &d, &isnull);
	SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its array is read");
	if (!isnull)
		nel = u->rec != NULL ? sift_v6_recfn_call(u->rec, d, u->bcx, ev) : array_elems(u, d, ev);
	if (u->has_ewhere && nel > 0)
	{
		keep = palloc0(((nel + 63) / 64 + 1) * sizeof(uint64));
		eval_elems(u, ev, nel, keep);
	}
	emit_row(u, u->fv, ev, keep, nel, row);
	MemoryContextSwitchTo(old);
}

static void
restrict_where(UnRun *u, const SiftV6Batch *b)
{
	SiftBatchSel *bs = &u->bs;
	uint64		keep[SIFT_VEC_WORDS];

	for (int i = 0; i < u->spec->nfcols; i++)
		u->wvecs[i] = (SiftVec *) &b->col[u->wcol[i]];
	sift_clause_eval(&u->where, u->wvecs, b->n, keep);
	for (int w = 0; w < (b->n + 63) / 64; w++)
		bs->mask[w] &= keep[w];
}

static void
run_batch(UnRun *u, const SiftV6Batch *b)
{
	SiftBatchSel *bs = &u->bs;
	const int	words = (b->n + 63) / 64;

	u->stats->batches++;
	memset(bs->mask, 0, sizeof(bs->mask));
	for (int w = 0; w < words; w++)
		bs->mask[w] = b->mask != NULL ? b->mask[w] : ~UINT64CONST(0);
	if (b->n & 63)
		bs->mask[words - 1] &= (UINT64CONST(1) << (b->n & 63)) - 1;
	if (u->has_where)
		restrict_where(u, b);
	compute_columns(u, b, bs);
	for (int w = 0; w < words; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			u->stats->rows++;
			emit_heap_row(u, b, w * 64 + (uint32) __builtin_ctzll(bits));
		}
}

static void
setup_clauses(UnRun *u)
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
			u->wcol[i] = col_index(u, &spec->fcol[i], false);
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

static void
open_source(UnRun *u, Relation heap, Relation index, Snapshot snap)
{
	SiftV6Input in;

	if (u->spec->is_table)
		sift_v6_input_init_table(&in, heap);
	else
		sift_v6_input_init_rel(&in, index);
	for (int i = 0; i < u->ncols; i++)
		in.col[i] = (SiftV6InCol) {.attno = u->cols[i].attno,.typid = u->cols[i].typid,.type = vec_type(u->cols[i].cls)};
	in.ncols = u->ncols;
	u->src = sift_v1_source_open(CurrentMemoryContext, snap, &in, NULL, NULL);
}

static void
v1_unnest_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, Snapshot snap, SiftV6ScanStats *stats, const SiftV6StarRun *star)
{
	UnRun		run = {0};
	UnRun	   *u = &run;
	SiftV6Batch b;

	u->spec = spec;
	u->x = spec->ext;
	u->stats = stats;
	u->nel = u->x->elem.ncols;
	for (int c = 0; c < u->nel; c++)
		u->etype[c] = vec_type(sift_v6_class(u->x->elem.col_type[c]));
	u->heap_att = spec->is_table ? u->x->elem.attno : index->rd_index->indkey.values[u->x->elem.attno - 1];
	if (u->x->elem.fn == SV6E_UNNEST)
		get_typlenbyvalalign(u->x->elem.type, &u->typlen, &u->typbyval, &u->typalign);
	else
		u->rec = sift_v6_recfn_new(CurrentMemoryContext, u->x);
	u->bcx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 unnest heap rows");
	u->pidx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	u->eidx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	u->month_key = palloc0(MONTH_SLOTS * sizeof(int32));
	u->month_val = palloc0(MONTH_SLOTS * sizeof(int32));
	setup_cols(u);
	setup_clauses(u);
	setup_pipe(u, g, star);
	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].month)
		{
			u->mdata[i] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
			u->mvalid[i] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		}
	open_source(u, heap, index, snap);
	while (u->src->next(u->src, &b))
		run_batch(u, &b);
	u->src->close(u->src);
	MemoryContextDelete(u->bcx);
}

const SiftJsonbOps v1_jsonb_ops = {
	.key_leaf = jsonb_key_leaf,
	.unnest_run = v1_unnest_run
};
