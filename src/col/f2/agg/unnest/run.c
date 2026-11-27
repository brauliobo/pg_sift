/*
 * col/f2/agg/unnest/run.c - the aggregate scan of a LATERAL unnest of an array column (agg/v6_unnest_run.h).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"

#include "agg/qual/qual.h"
#include "col/am/preds.h"
#include "col/f2/agg/unnest/int.h"
#include "col/f2/agg/unnest/run.h"

static SiftVecType
vec_type(SiftV6Class cls)
{
	return cls == SV6C_F64 ? SVEC_F64 : cls == SV6C_TEXT ? SVEC_STRVIEW : SVEC_I64;
}

/* the "<doc>#present" leaf of a shredded document, whose NULL slots are the SQL NULL documents an IS [NOT] NULL of the document tests (agg/plan/qual.c checks it exists) */
static int
present_leaf(const SiftColRel *r, int attno)
{
	int			i = r->first[attno];

	while (i < r->schema->ncols && r->schema->cols[i].attno == attno && !sift_col_def_is_present(&r->schema->cols[i]))
		i++;
	SIFT_INVARIANT(i < r->schema->ncols && r->schema->cols[i].attno == attno, "sift_col: the jsonb column %d of an unnest scan has a presence leaf", attno);
	return i;
}

/* the leaf a column of the scan is: the one of a key of a jsonb document a qual or an expression names, the one jleaf names, the presence of a document read whole, or the first one of the attribute */
static int
leaf_of(const SiftColRel *r, const SiftV6Col *c)
{
	if (sift_v6_vatt_is_json(c->attno))
		return sift_v6_vatt_leaf(c->attno);
	if (c->jleaf > 0)
		return c->jleaf - 1;
	return c->typid == JSONBOID && r->json[c->attno] ? present_leaf(r, c->attno) : r->first[c->attno];
}

static int col_index(UnRun *u, const SiftColRel *r, const SiftV6Col *c, bool month);

/* the column of the batch that is leaf `leaf`, which the statement may not read itself: the source of the value of a member that is a column of the index, or the presence of a document */
static int
source_col(UnRun *u, const SiftColRel *r, int leaf, Oid typid)
{
	const SiftV6Col c = {.attno = 1,.typid = typid,.jleaf = leaf + 1};
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
	const SiftF2Rel *f = sift_f2_rel(u->index);

	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].leaf == leaf && u->cols[i].month == month)
			return i;
	if (u->ncols == SIFT_F2_PULL_MAX_COLS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the aggregate scan of an unnest reads at most %d columns of the table", SIFT_F2_PULL_MAX_COLS),
						errhint("Aggregate fewer columns of the table that is unnested.")));
	u->cols[u->ncols] = (UnCol) {.leaf = leaf,.cls = c->typid == BPCHAROID ? SV6C_TEXT : sift_v6_class(c->typid),.month = month,.json = c->jleaf > 0 || sift_v6_vatt_is_json(c->attno),.read = true};
	if (f->key_leaf[leaf])
	{
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

/* the vector of the pipe a column of the statement is read from: a column of the table, the column of the element, or what the pairs compute from them */
static int
slot_of(UnRun *u, const SiftColRel *r, const SiftV6Col *c, bool month)
{
	if (sift_v6_col_is_flag(c))
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
	return u->attr_slot != NULL ? u->attr_slot(u->attr_arg, c) : col_index(u, r, c, month);
}

void
sift_v6_unnest_attr_cols(const SiftV6Spec *spec, SiftV6UnnestColFn fn, void *arg)
{
	const SiftV6Ext *x = spec->ext;

	for (int i = 0; i < spec->nkeys; i++)
		if (spec->key[i].src == SV6SRC_ATTR && spec->key[i].derive == 0)
			fn(arg, &spec->key[i], key_is_month(spec, i));
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
		if (sift_v6_vec_col(spec, v) != NULL && sift_v6_vec_col(spec, v)->src == SV6SRC_ATTR && sift_v6_vec_col(spec, v)->derive == 0 && !sift_v6_col_is_flag(sift_v6_vec_col(spec, v)))
			fn(arg, sift_v6_vec_col(spec, v), false);
	for (int d = 0; d < x->ndims; d++)
		for (int k = 0; k < x->dim[d].nkeys; k++)
			if (sift_v6_xdim_key_is_col(&x->dim[d], k) && x->dim[d].fsrc[k] == SV6SRC_ATTR)
			{
				const SiftV6Col key = sift_v6_xdim_key_col(&x->dim[d], k);

				fn(arg, &key, false);
			}
	for (int i = 0; i < x->nrcol; i++)
		if (x->rcol[i].src == SV6SRC_ATTR)
			fn(arg, &x->rcol[i], false);
	for (int k = 0; k < spec->nderive; k++)
	{
		const SiftV6Derive *d = &spec->derive[k];

		for (int c = 0; c <= d->ncond; c++)
			if ((c == 0 ? d->src : d->cond[c - 1]).src == SV6SRC_ATTR)
				fn(arg, c == 0 ? &d->src : &d->cond[c - 1], false);
		if (d->kind == SVD_PG_EXPR)
		{
			SiftV6PgItem it;

			sift_v6_pgexpr_item(spec, d, &it);
			for (int c = 0; c < it.nin; c++)
				if (it.col[c].src == SV6SRC_ATTR)
					fn(arg, &it.col[c], false);
		}
	}
	for (int i = 0; x->where != NIL && i < spec->nfcols; i++)
		if (spec->fcol[i].src == SV6SRC_ATTR && !sift_v6_col_is_flag(&spec->fcol[i]))
			fn(arg, &spec->fcol[i], false);
}

typedef struct ColsOf
{
	UnRun	   *u;
	const SiftColRel *r;
} ColsOf;

static void
add_col(void *arg, const SiftV6Col *c, bool month)
{
	const ColsOf *co = arg;

	col_index(co->u, co->r, c, month);
}

static void
setup_cols(UnRun *u, const SiftColRel *r)
{
	sift_v6_unnest_attr_cols(u->spec, add_col, &(ColsOf) {u, r});
	for (int k = 0; k < u->ntests; k++)
	{
		for (int c = 0; c < u->x->test[k].ncmps; c++)
		{
			const SiftV6XCmp *cmp = &u->x->test[k].cmp[c];
			const SiftV6Col outer = {.attno = cmp->outer,.typid = cmp->outer_type};

			u->outer_slot[k * SIFT_V6_XCMPS + c] = col_index(u, r, &outer, false);
		}
		u->doc_slot[k] = u->x->test[k].doc_leaf >= 0 ? source_col(u, r, u->x->test[k].doc_leaf, BOOLOID) : -1;
	}
}

/* the leaves the pull reads: the columns, and of each stream the lengths of its array and its element columns (the keys and the values of the map when keyed) */
static SiftF2PullSpec
pull_spec(const UnRun *u)
{
	SiftF2PullSpec ps = {.ncols = u->ncols,.nmasks = 0,.ngroups = u->x->elem.group_leaf >= 0 ? u->nst : 0,.poison_rows = u->keyed};

	for (int i = 0; i < u->ncols; i++)
		ps.col[i] = (SiftF2PullCol) {.leaf = u->cols[i].leaf,.type = vec_type(u->cols[i].cls)};
	for (int s = 0; s < ps.ngroups; s++)
	{
		const UnStream *st = &u->st[s];

		ps.group[s] = (SiftF2PullGroup) {.leaf = st->el->group_leaf,.nchild = st->nchild};
		for (int c = 0; c < st->nchild; c++)
			ps.group[s].child[c] = (SiftF2PullCol) {.leaf = u->keyed ? (c == 0 ? st->el->key_leaf : st->el->val_leaf) : st->el->col_leaf[c],.type = st->ctype[c],
													.kind = !u->keyed && st->el->col_kind[c]};
	}
	return ps;
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
	u->pg[k] = sift_v6_pgexpr_open(CurrentMemoryContext, NULL, it.expr, it.nin, pin);
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
		type[i] = u->attr_slot != NULL ? u->ftype[i] : vec_type(u->cols[i].cls);
	for (int c = 0; c < u->nel; c++)
		type[u->ncols + c] = u->st[0].etype[c];
	for (int k = 0; k < spec->nderive; k++)
		type[u->ncols + u->nel + k] = spec->derive[k].kind == SVD_PG_EXPR ? sift_v6_pgexpr_vec_type(spec, &spec->derive[k]) : SVEC_F64;
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

/* the month of a date the way PostgreSQL finds it: date_trunc('month', date)::date, asked in the batch context and kept in the slot of the date (a zeroed slot holds day 0, which
 * is its own month) */
static int32
month_of(UnRun *u, int32 day)
{
	const uint32 at = ((uint32) day * 2654435761u) >> (32 - MONTH_BITS);

	if (u->month_key[at] != day)
	{
		MemoryContext old = MemoryContextSwitchTo(u->bcx);
		const Datum ts = DirectFunctionCall1(date_timestamptz, DateADTGetDatum(day));

		u->month_val[at] = DatumGetDateADT(DirectFunctionCall1(timestamptz_date, DirectFunctionCall2(timestamptz_trunc, u->month_unit, ts)));
		u->month_key[at] = day;
		MemoryContextSwitchTo(old);
	}
	return u->month_val[at];
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
		if (u->cols[i].json && u->cols[i].read)
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

static void
restrict_where(UnRun *u, const SiftF2ExtBatch *b)
{
	SiftBatchSel *bs = &u->bs;
	uint64		keep[SIFT_VEC_WORDS];

	for (int i = 0; i < u->spec->nfcols; i++)
		if (u->wcol[i] >= 0)
			u->wvecs[i] = (SiftVec *) &b->col[u->wcol[i]];
	sift_clause_eval(&u->where, u->wvecs, b->n, keep);
	for (int w = 0; w < (b->n + 63) / 64; w++)
		bs->mask[w] &= keep[w] | b->elem->poison[w];
}

/* the element columns of stream s of the batch */
static void
elem_vecs(const UnRun *u, int s, const SiftF2ElemStream *es, SiftVec *ev)
{
	for (int c = 0; c < u->st[s].nchild; c++)
		ev[c] = (SiftVec) {.type = u->st[s].ctype[c],.data = (void *) es->child_data[c],.valid = (uint64 *) es->child_valid[c],.all_valid = false};
}

/* the tests of the rows of the batch the layouts hold, and the rows of those that fail one the statement keeps its rows by leave the batch; the others are the heap's */
static void
decide_tests(UnRun *u, const SiftF2ExtBatch *b)
{
	const uint32 words = (b->n + 63) / 64;
	SiftVec		ev[SIFT_V6_XSTREAMS][SIFT_V6_XELEM_COLS];
	SiftV6StreamIn in[SIFT_V6_XSTREAMS];

	memset(u->held, 0, sizeof(u->held));
	for (int s = 0; s < u->nst; s++)
	{
		if (b->elem[s].layout.n_parents < (uint32) b->n)
			return;
		elem_vecs(u, s, &b->elem[s], ev[s]);
		in[s] = (SiftV6StreamIn) {.lay = &b->elem[s].layout,.doc_valid = b->elem[s].doc_valid,.ev = ev[s]};
	}
	for (uint32 w = 0; w < words; w++)
		u->held[w] = u->bs.mask[w] & ~b->elem->poison[w];
	sift_v6_row_tests_eval(u->tests, in, b->col, u->outer_slot, u->held, u->yes, u->known);
	for (int k = 0; k < u->ntests; k++)
	{
		const SiftV6XTest *xt = &u->x->test[k];
		const SiftVec *doc = u->doc_slot[k] >= 0 ? &b->col[u->doc_slot[k]] : NULL;

		for (uint32 w = 0; doc != NULL && w < words; w++)
			u->known[k][w] = u->held[w] & (doc->all_valid ? ~UINT64CONST(0) : doc->valid[w]);
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

	for (uint32 w = 0; w < words; w++)
	{
		fallback[w] = u->bs.mask[w] & ~u->held[w];
		for (uint64 bits = u->bs.mask[w] & u->held[w]; bits != 0; bits &= bits - 1)
		{
			u->pidx[m] = w * 64 + (uint32) __builtin_ctzll(bits);
			u->eidx[m++] = SIFT_GATHER_NONE;
			if (m == SIFT_VEC_LEN)
			{
				sift_f2_unnest_emit_pairs(u, u->fv, NULL, m);
				m = 0;
			}
		}
	}
	if (m > 0)
		sift_f2_unnest_emit_pairs(u, u->fv, NULL, m);
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
	if (u->ntests > 0)
	{
		decide_tests(u, b);
		fill_flags(u, b->n);
	}
	if (u->has_where)
		restrict_where(u, b);
	if (sift_v6_vet_mask(&u->vet, b->tid, 0, false, n, bs) == 0)
		return;
	u->stats->rows += bs->n_active;
	MemoryContextReset(u->bcx);
	compute_columns(u, b, bs);
	elem_vecs(u, 0, es, ev);
	if (u->keyed && u->x->elem.group_leaf < 0)
		sift_f2_unnest_run_keyed(u, b, NULL, fallback);
	else if (u->x->row_tests)
		emit_tested(u, b, fallback);
	else if (es->layout.n_parents < (uint32) n)
		memcpy(fallback, bs->mask, words * sizeof(uint64));
	else if (u->keyed)
		sift_f2_unnest_run_keyed(u, b, &ev[1], fallback);
	else
	{
		uint64	   *keep = NULL;
		SiftUnnestIn in = {.lay = &es->layout,.parent_mask = bs->mask,.poison = es->poison,.doc_valid = es->doc_valid,.left = u->x->elem.left};
		SiftUnnestCursor cur = {0};

		if (u->eev != NULL)
		{
			keep = MemoryContextAllocZero(u->bcx, ((es->n_elems + 63) / 64 + 1) * sizeof(uint64));
			sift_v6_elem_eval(u->eev, ev, 0, es->n_elems, &in, keep);
			in.elem_mask = keep;
		}
		while (!sift_unnest_done(&in, &cur))
		{
			const uint32 m = sift_unnest_expand(&in, &cur, u->pidx, u->eidx, SIFT_VEC_LEN, fallback);

			if (m > 0)
				sift_f2_unnest_emit_pairs(u, u->fv, ev, m);
		}
	}
	for (int w = 0; w < words; w++)
		for (uint64 bits = fallback[w] & bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 row = w * 64 + __builtin_ctzll(bits);

			if (u->keyed)
				sift_f2_unnest_emit_heap_keys(u, u->fv, b->tid[row], row);
			else if (u->ntests > 0)
				sift_f2_unnest_emit_test_heap_row(u, b->tid[row], row);
			else
				sift_f2_unnest_emit_heap_row(u, u->fv, b->tid[row], row);
		}
}

/* a statement of plain columns of the table and of the element, the pairs of which the compute threads can fold as the serial run does (no qual of the element PostgreSQL evaluates) */
static bool
par_plain(const UnRun *u)
{
	const SiftV6ElemFn fn = u->x->elem.fn;

	if ((fn != SV6E_UNNEST && fn != SV6E_RECORDSET) || u->ntests > 0 || u->row_slot >= 0 || u->spec->nderive > 0 || u->x->ndims > 0 || u->attr_slot != NULL ||
		(u->eev != NULL && sift_v6_elem_eval_pg(u->eev)))
		return false;
	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].month || u->cols[i].json)
			return false;
	return true;
}

/* The rows of the batch that take part: the deletion vector, the WHERE clause, the visibility of the heap row; false when none is left */
static bool
par_select_rows(UnRun *u, const SiftF2ExtBatch *b, uint64 *mask)
{
	SiftBatchSel *bs = &u->bs;
	const int	words = (b->n + 63) / 64;

	memset(bs->mask, 0, sizeof(bs->mask));
	memcpy(bs->mask, b->live, words * sizeof(uint64));
	if (b->n & 63)
		bs->mask[words - 1] &= (UINT64CONST(1) << (b->n & 63)) - 1;
	if (u->has_where)
		restrict_where(u, b);
	if (sift_v6_vet_mask(&u->vet, b->tid, 0, false, b->n, bs) == 0)
		return false;
	memcpy(mask, bs->mask, words * sizeof(uint64));
	return true;
}

/* The rows whose array the index cannot hold leave mask and are folded from the heap (not again by a rerun) */
static void
par_fold_heap_rows(UnRun *u, const SiftF2ExtBatch *b, uint64 *mask)
{
	const SiftF2ElemStream *es = b->elem;
	const int	words = (b->n + 63) / 64;

	MemoryContextReset(u->bcx);
	for (int w = 0; w < words; w++)
	{
		const uint64 heap = es->layout.n_parents < (uint32) b->n ? mask[w] : mask[w] & es->poison[w];

		mask[w] &= ~heap;
		for (uint64 bits = heap; u->pulled > u->seen && bits != 0; bits &= bits - 1)
			sift_f2_unnest_emit_heap_row(u, b->col, b->tid[w * 64 + __builtin_ctzll(bits)], w * 64 + __builtin_ctzll(bits));
	}
}

static bool
par_next_batch(void *arg, const SiftF2ExtBatch **bp, uint64 *mask)
{
	UnRun	   *u = arg;

	while ((*bp = sift_f2_pull_next(u->pull)) != NULL)
	{
		uint64		any = 0;

		u->pulled++;
		u->stats->batches += u->pulled > u->seen;
		if (!par_select_rows(u, *bp, mask))
			continue;
		u->stats->rows += u->pulled > u->seen ? u->bs.n_active : 0;
		par_fold_heap_rows(u, *bp, mask);
		for (int w = 0; w < SIFT_VEC_WORDS; w++)
			any |= mask[w];
		if (any != 0)
			return true;
	}
	return false;
}

/* The quals of the element on the elements of the batch, bit e of the elements of its layout (es->n_elems of them) */
static const uint64 *
batch_keep(UnRun *u, const SiftF2ExtBatch *b, const SiftVec *ev)
{
	const SiftF2ElemStream *es = b->elem;
	uint64	   *keep;

	if (u->eev == NULL)
		return NULL;
	keep = MemoryContextAllocZero(u->bcx, ((es->n_elems + 63) / 64 + 1) * sizeof(uint64));
	sift_v6_elem_eval(u->eev, ev, es->layout.offsets[0], es->layout.offsets[b->n], NULL, keep);
	return keep;
}

/* The same for the elements of the slot of the batch: bit i for the element offsets[0] + i, none set past the last */
static const uint64 *
par_elem_keep(void *arg, const SiftF2ExtBatch *b)
{
	UnRun	   *u = arg;
	const SiftF2ElemStream *es = b->elem;
	const uint32 first = es->layout.offsets[0],
				nelems = es->layout.offsets[b->n] - first,
				shift = first & 63;
	const uint64 *keep;
	uint64	   *out;
	SiftVec		ev[SIFT_V6_XELEM_COLS];

	elem_vecs(u, 0, es, ev);
	if ((keep = batch_keep(u, b, ev)) == NULL)
		return NULL;
	out = MemoryContextAllocZero(u->bcx, ((nelems + 63) / 64 + 1) * sizeof(uint64));
	for (uint32 w = 0; w < (nelems + 63) / 64; w++)
		out[w] = keep[first / 64 + w] >> shift | (shift == 0 ? 0 : keep[first / 64 + w + 1] << (64 - shift));
	if (nelems & 63)
		out[nelems / 64] &= (UINT64CONST(1) << (nelems & 63)) - 1;
	return out;
}

/* The pairs of the rows of mask, read from the columns of the index */
static void
par_fold_pairs(void *arg, const SiftF2ExtBatch *b, const uint64 *mask)
{
	UnRun	   *u = arg;
	const SiftF2ElemStream *es = b->elem;
	SiftUnnestIn in = {.lay = &es->layout,.parent_mask = mask,.doc_valid = es->doc_valid,.left = u->x->elem.left};
	SiftUnnestCursor cur = {0};
	SiftVec		ev[SIFT_V6_XELEM_COLS];
	uint64		none[SIFT_VEC_WORDS];

	elem_vecs(u, 0, es, ev);
	in.elem_mask = batch_keep(u, b, ev);
	while (!sift_unnest_done(&in, &cur))
	{
		const uint32 m = sift_unnest_expand(&in, &cur, sift_un_pairs_pidx(u->pairs), sift_un_pairs_eidx(u->pairs), SIFT_VEC_LEN, none);

		if (m > 0)
			sift_un_pairs_emit(u->pairs, b->col, ev, m);
	}
}

static void
par_redo(void *arg)
{
	UnRun	   *u = arg;

	sift_f2_pull_end(u->pull);
	u->pull = sift_f2_pull_begin(u->index, u->snap, &u->ps);
	u->seen = u->pulled;
	u->pulled = 0;
}

/* true: every row is in the groups of g */
static bool
unnest_par(UnRun *u, SiftV6Group *g)
{
	SiftV6UnnestSrc src;
	uint32		eused = 0;

	if (!par_plain(u))
		return false;
	for (int c = 0; c < u->nel; c++)
		eused |= (uint32) u->eused[c] << c;
	u->pairs = sift_un_pairs_new(CurrentMemoryContext, u->pipe, u->ncols, u->nel, eused);
	src = (SiftV6UnnestSrc) {.next = par_next_batch,.fold = par_fold_pairs,.redo = par_redo,.elem_keep = par_elem_keep,.pairs = u->pairs,.pipe = u->pipe,.nel = u->nel,.eused = eused,
		.left = u->x->elem.left,.arg = u};
	return sift_v6_unnest_par(u->spec, g, u->index, &src);
}

/* the attribute of the heap a column of the host is: of the index, or the table's own */
static AttrNumber
heap_attno(const UnRun *u, int att)
{
	return u->index != NULL ? u->index->rd_index->indkey.values[att - 1] : att;
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
		u->wvecs = palloc0(Max(spec->nfcols, 1) * sizeof(SiftVec *));
		u->wcol = palloc(Max(spec->nfcols, 1) * sizeof(int));
		for (int i = 0; i < spec->nfcols; i++)
		{
			const bool	flag = sift_v6_col_is_flag(&spec->fcol[i]);

			u->wcol[i] = spec->fcol[i].src == SV6SRC_ATTR && !flag ? col_index(u, r, &spec->fcol[i], false) : -1;
			if (flag)
			{
				u->flag_used[spec->fcol[i].attno - SIFT_V6_XFLAG_VATT] = true;
				u->wvecs[i] = &u->flagv[spec->fcol[i].attno - SIFT_V6_XFLAG_VATT];
			}
		}
	}
	if (u->x->pair_where != NIL)
	{
		u->has_pwhere = true;
		bound = sift_v6_unnest_elem_clause(CurrentMemoryContext, u->x->pair_where, &u->pwhere, u->pcol, &u->npvec);
		SIFT_INVARIANT(bound, "the quals over the members of the keys of the aggregate scan compiled when it was planned");
	}
	if (u->x->elem_where != NIL)
		u->eev = sift_v6_elem_eval_open(CurrentMemoryContext, u->x->elem_where);
	if (u->ntests == 0)
		return;
	u->tests = sift_v6_row_tests_open(CurrentMemoryContext, u->x);
	foreach_ptr(List, tq, u->x->test_quals)
	{
		for (int c = 0; c < u->x->test[k].ncmps; c++)
			u->outer_att[k * SIFT_V6_XCMPS + c] = heap_attno(u, u->x->test[k].cmp[c].outer);
		if (lsecond(tq) != NIL)
			u->heap_test[k] = ExecPrepareExpr(linitial(lsecond(tq)), u->estate);
		u->flag_data[k] = palloc0(SIFT_VEC_LEN * sizeof(int64));
		u->flag_valid[k] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		k++;
	}
}

/* stream s: the columns of its elements, and how a row read from the heap gives them */
static void
stream_init(UnRun *u, int s, AttrNumber heap_att)
{
	UnStream   *st = &u->st[s];
	const SiftV6XElem *el = sift_v6_ext_stream(u->x, s);
	const bool	keyed = el->fn == SV6E_KEYS;

	st->el = el;
	st->nchild = keyed ? (el->group_leaf >= 0 ? 2 : 0) : el->ncols;
	for (int c = 0; c < el->ncols; c++)
		st->etype[c] = vec_type(sift_v6_class(el->col_type[c]));
	for (int c = 0; c < st->nchild; c++)
		st->ctype[c] = keyed ? (c == 0 ? SVEC_STRVIEW : SVEC_F64) : st->etype[c];
	st->heap_att = keyed ? 0 : heap_att;
	if (el->fn == SV6E_UNNEST)
		get_typlenbyvalalign(el->type, &st->typlen, &st->typbyval, &st->typalign);
	else if (!keyed && el->fn != SV6E_PATH)
		st->rec = sift_v6_recfn_new(CurrentMemoryContext, el, sift_v6_ext_stream_names(u->x, s));
}

static void
unrun_init(UnRun *u, const SiftV6Spec *spec, Relation index, SiftV6ScanStats *stats, AttrNumber heap_att)
{
	u->spec = spec;
	u->row_slot = -1;
	u->x = spec->ext;
	u->stats = stats;
	u->index = index;
	u->keyed = u->x->elem.fn == SV6E_KEYS;
	u->nel = u->x->elem.ncols;
	u->ntests = u->x->ntests;
	u->nst = 1 + u->x->nstreams;
	for (int s = 0; s < u->nst; s++)
		stream_init(u, s, s == 0 ? heap_att : 0);
	u->bcx = sift_mem_allocset_create(CurrentMemoryContext, "sift agg v6 unnest batch");
	sift_mem_init(&u->rmt, "agg_unnest_heap_row", CurrentMemoryContext, true, SIFT_BUDGET_WORK_MEM, 1, 1);
	u->ccx = sift_mem_allocset_create(u->rmt.root_cx, "sift agg v6 unnest heap chunk");
	if (u->x->heap_where != NIL || u->x->elem_array != NIL || u->keyed || u->ntests > 0 || u->st[0].rec != NULL)
	{
		MemoryContext old = CurrentMemoryContext;

		u->estate = CreateExecutorState();
		MemoryContextSwitchTo(u->estate->es_query_cxt);
		u->hqual = u->x->heap_where != NIL ? ExecPrepareQual(u->x->heap_where, u->estate) : NULL;
		for (int s = 0; s < u->nst; s++)
			u->st[s].arr = sift_v6_ext_stream_array(u->x, s) != NULL ? ExecPrepareExpr(unconstify(Expr *, sift_v6_ext_stream_array(u->x, s)), u->estate) : NULL;
		u->probe = u->st[0].rec != NULL ? sift_v6_late_probe_open(u->x->late_probe, u->estate, u->st[0].rec) : NULL;
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
	u->month_key = palloc0((1 << MONTH_BITS) * sizeof(int32));
	u->month_val = palloc0((1 << MONTH_BITS) * sizeof(int32));
	u->month_unit = CStringGetTextDatum("month");
}

static void
unrun_buffers(UnRun *u)
{
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
}

static void
unrun_end(UnRun *u)
{
	sift_v6_pipe_finish(u->pipe);
	sift_v6_vet_end(&u->vet);
	if (u->estate != NULL)
		FreeExecutorState(u->estate);
	for (int s = 0; s < u->nst; s++)
		if (u->st[s].rec != NULL)
			sift_v6_recfn_free(u->st[s].rec);
	MemoryContextDelete(u->bcx);
	sift_mem_end(&u->rmt);
}

/* a qual of the table on a column of the index (a comparison, an IN list or a null test with constants) as the key an index scan has of it; false for a virtual column and an OR */
static bool
where_key(Relation index, const Node *q, ScanKeyData *k)
{
	const Node *arg;
	AttrNumber	att;
	const Oid	opno = IsA(q, OpExpr) ? ((const OpExpr *) q)->opno : IsA(q, ScalarArrayOpExpr) ? ((const ScalarArrayOpExpr *) q)->opno : InvalidOid;
	const Const *c;
	int			strategy;
	Oid			lefttype,
				righttype;

	if (!IsA(q, NullTest) && !IsA(q, OpExpr) && !IsA(q, ScalarArrayOpExpr))
		return false;
	arg = IsA(q, NullTest) ? (const Node *) ((const NullTest *) q)->arg : linitial(IsA(q, OpExpr) ? ((const OpExpr *) q)->args : ((const ScalarArrayOpExpr *) q)->args);
	att = ((const Var *) arg)->varattno;
	if (att < 1 || att > IndexRelationGetNumberOfKeyAttributes(index))
		return false;
	*k = (ScanKeyData) {.sk_attno = att};
	if (IsA(q, NullTest))
	{
		k->sk_flags = SK_ISNULL | (((const NullTest *) q)->nulltesttype == IS_NULL ? SK_SEARCHNULL : SK_SEARCHNOTNULL);
		return true;
	}
	c = lsecond(IsA(q, OpExpr) ? ((const OpExpr *) q)->args : ((const ScalarArrayOpExpr *) q)->args);
	if (get_op_opfamily_strategy(opno, index->rd_opfamily[att - 1]) == 0)
		strategy = sift_v6_qual_private_strategy(opno, index->rd_opfamily[att - 1]);
	else
		get_op_opfamily_properties(opno, index->rd_opfamily[att - 1], false, &strategy, &lefttype, &righttype);
	k->sk_strategy = (StrategyNumber) strategy;
	k->sk_subtype = c->consttype;
	k->sk_collation = exprInputCollation((const Node *) q);
	k->sk_argument = c->constvalue;
	k->sk_flags = IsA(q, ScalarArrayOpExpr) ? SK_SEARCHARRAY : c->constisnull ? SK_ISNULL : 0;
	return strategy != 0;
}

/* The quals of the table on columns of the index as predicates of their leaves: the pull reads no granule whose zones they leave out (the batches are filtered by the quals again) */
static void
where_preds(UnRun *u, const SiftColRel *r, Relation index, SiftF2PullSpec *ps)
{
	ColKeys		ck = {.defs = r->schema->cols,.ndefs = r->schema->ncols,.first = r->first,.idesc = RelationGetDescr(index)};
	ScanKeyData *keys = palloc(Max(list_length(u->x->where), 1) * sizeof(ScanKeyData));
	SiftColPred *preds = palloc(Max(list_length(u->x->where), 1) * sizeof(SiftColPred));
	int			n = 0;

	foreach_ptr(Node, q, u->x->where)
		n += where_key(index, q, &keys[n]);
	if (n == 0 || !sift_col_key_preds(keys, n, &ck, preds))
		return;
	ps->npred = sift_col_preds_keep(preds, n);
	ps->pred = preds;
}

void
sift_v6_unnest_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, Snapshot snap, SiftV6ScanStats *stats, const SiftV6StarRun *star)
{
	UnRun		run = {0};
	UnRun	   *u = &run;
	SiftF2PullSpec ps;
	SiftV6MapPreagg *map;
	const SiftF2ExtBatch *b;
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);

	unrun_init(u, spec, index, stats, spec->ext->elem.fn == SV6E_KEYS ? 0 : index->rd_index->indkey.values[spec->ext->elem.attno - 1]);
	setup_cols(u, r);
	setup_clauses(u, r);
	for (int c = 0; c < u->nel; c++)
		u->lcol[c] = u->keyed && u->x->elem.col_leaf[c] >= 0 ? source_col(u, r, u->x->elem.col_leaf[c], FLOAT8OID) : -1;
	setup_pipe(u, r, g, star);
	unrun_buffers(u);
	sift_v6_vet_begin(&u->vet, heap, snap, stats);
	ps = pull_spec(u);
	where_preds(u, r, index, &ps);
	map = sift_v6_map_preagg_begin(spec, g, &u->vet, stats);
	ps.gran_sink = map != NULL ? sift_v6_map_preagg_try_granule : NULL;
	ps.gran_arg = map;
	u->ps = ps;
	u->snap = snap;
	u->pull = sift_f2_pull_begin(index, snap, &u->ps);
	if (map != NULL || !unnest_par(u, g))
		while ((b = sift_f2_pull_next(u->pull)) != NULL)
			run_batch(u, b);
	sift_f2_pull_end(u->pull);
	if (map != NULL)
		sift_v6_map_preagg_end(map);
	unrun_end(u);
}

SiftV6UnnestRows *
sift_v6_unnest_rows_begin(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Snapshot snap, SiftV6ScanStats *stats, int nfact, const SiftVecType *fact_type,
						  int (*attr_slot) (void *arg, const SiftV6Col *c), void *attr_arg)
{
	UnRun	   *u = sift_palloc0_aligned(UnRun, 1);

	if (nfact > SIFT_F2_PULL_MAX_COLS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the aggregate scan of an unnest reads at most %d columns of the table", SIFT_F2_PULL_MAX_COLS),
						errhint("Aggregate fewer columns of the table that is unnested.")));
	unrun_init(u, spec, NULL, stats, spec->ext->elem.attno);
	u->ncols = nfact;
	memcpy(u->ftype, fact_type, nfact * sizeof(SiftVecType));
	u->attr_slot = attr_slot;
	u->attr_arg = attr_arg;
	setup_clauses(u, NULL);
	setup_pipe(u, NULL, g, NULL);
	sift_v6_vet_begin(&u->vet, heap, snap, stats);
	return u;
}

/* the flags of the tests of a batch whose rows all come from the heap: none known until the row is read */
static void
clear_flags(UnRun *u, uint32 n)
{
	for (int k = 0; k < u->ntests; k++)
	{
		memset(u->flag_valid[k], 0, SIFT_VEC_WORDS * sizeof(uint64));
		u->flagv[k] = (SiftVec) {.data = u->flag_data[k],.valid = u->flag_valid[k],.len = (uint16) n,.all_valid = false,.dense = true,.type = SVEC_I64};
	}
}

void
sift_v6_unnest_rows_run(SiftV6UnnestRows *u, SiftVec *const *fact, const SiftBatchSel *bs, const uint64 *tid)
{
	for (int i = 0; i < u->ncols; i++)
		u->fv[i] = *fact[i];
	MemoryContextReset(u->bcx);
	clear_flags(u, bs->n_raw);
	for (int w = 0; w < (bs->n_raw + 63) / 64; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 row = w * 64 + __builtin_ctzll(bits);

			if (u->ntests > 0)
				sift_f2_unnest_emit_test_heap_row(u, tid[row], row);
			else
				sift_f2_unnest_emit_heap_row(u, u->fv, tid[row], row);
		}
}

void
sift_v6_unnest_rows_end(SiftV6UnnestRows *u)
{
	unrun_end(u);
}
