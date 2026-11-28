/*
 * col/v1_engine/store_ops_unnest.c - the aggregate scan of a LATERAL unnest over Format 1 (agg/v6_unnest_run.h). The columns of the table the statement reads come from the batches of the
 * Format 1 source, which hands over only the rows the tombstones and the snapshot leave. When the index keeps the array shredded (an SCL_ARRAY leaf of the lengths and a child leaf for each column of the
 * element, or an SCL_OBJECT leaf of the keys and values of a map), the elements of the rows that pass the quals of the table are those of the leaves (v1_store_ops_elem.c); the rows the leaves cannot hold
 * (a NULL array or a document that is no array, an array cut by the shredder, an element that holds a value its column cannot or may be no object) have their array read from the heap row and taken apart as
 * PostgreSQL does, with the errors it raises. Without the leaves every array is read from its heap row. The pairs of (row, element) go through the pipe of the engine into the groups.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "nodes/nodeFuncs.h"

#include "col/source_fold.h"
#include "col/v1_engine/store_ops_unnest.h"
#include "core/popcount.h"

/* the vector a column of the table is read as: a numeric as the scaled integers its leaf holds */
static SiftVecType
col_vec_type(const UnCol *c)
{
	return c->typid == NUMERICOID ? SVEC_NUM64 : un_vec_type(c->cls);
}

static bool
key_is_month(const SiftV6Spec *spec, int key)
{
	for (int b = 0; b < spec->nbins; b++)
		if (spec->bin[b].key == key)
			return true;
	return false;
}

/* the column of the table the statement names, when it is one the batches hold */
static bool
reads_batch(const SiftV6Col *c)
{
	return c->src == SV6SRC_ATTR && !sift_v6_col_is_flag(c);
}

/* the column of the batches a column of the statement is read from */
int
sift_v1_unnest_col_index(UnRun *u, const SiftV6Col *col, bool month)
{
	const SiftV6Col *c = col;
	SiftV6Col	json;
	SiftV6Class cls;
	int			attno;

	if (sift_v6_vatt_is_json(c->attno))
	{
		json = sift_v6_vatt_col(c->attno, c->attno, c->typid, c->scale);
		c = &json;
	}
	cls = sift_v6_class(c->typid);
	attno = c->jleaf > 0 ? SIFT_V6_JLEAF_VATT + c->jleaf - 1 : c->attno;
	if (c->jtype || (c->attno >= SIFT_V6_JLEAF_VATT && c->jleaf == 0) || cls == SV6C_DATUM || cls == SV6C_NONE || (c->typid == NUMERICOID && c->scale == SIFT_V6_SCALE_DATUM))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("the aggregate scan of an unnest over Format 1 reads no column of type %u", c->typid),
						errhint("Group by and aggregate integer, numeric, date, timestamp, float8 or text columns of the table that is unnested.")));
	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].attno == attno && u->cols[i].month == month)
			return i;
	if (u->ncols == SIFT_V6_MAX_COLS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("the aggregate scan of an unnest reads at most %d columns of the table", SIFT_V6_MAX_COLS),
						errhint("Aggregate fewer columns of the table that is unnested.")));
	u->cols[u->ncols] = (UnCol) {.attno = attno,.typid = c->typid,.scale = c->scale,.cls = cls,.month = month};
	return u->ncols++;
}

/* the vector of the pipe a column of the statement is read from: a column of the table, the column of the element, or what the pairs compute from them */
static int
slot_of(UnRun *u, const SiftV6Col *c, bool month)
{
	if (sift_v6_col_is_flag(c))
	{
		u->flag_used[c->attno - SIFT_V6_XFLAG_VATT] = true;
		return flag_slot(u, c->attno - SIFT_V6_XFLAG_VATT);
	}
	if (c->derive > 0)
		return u->ncols + u->nel + c->derive - 1;
	if (c->src == SV6SRC_DIM)
		return sift_v6_pipe_dim_slot(u->pipe, c->dim, c->slot);
	if (c->src == SV6SRC_ELEM)
	{
		u->eused[c->slot] = true;
		return u->ncols + c->slot;
	}
	return sift_v1_unnest_col_index(u, c, month);
}

static void
setup_cols(UnRun *u)
{
	const SiftV6Spec *spec = u->spec;

	for (int i = 0; i < spec->nkeys; i++)
		if (reads_batch(&spec->key[i]) && spec->key[i].derive == 0)
			sift_v1_unnest_col_index(u, &spec->key[i], key_is_month(spec, i));
	for (int v = 0; v < sift_v6_spec_nvec(spec); v++)
		if (sift_v6_vec_col(spec, v) != NULL && reads_batch(sift_v6_vec_col(spec, v)) && sift_v6_vec_col(spec, v)->derive == 0)
			sift_v1_unnest_col_index(u, sift_v6_vec_col(spec, v), false);
	for (int d = 0; d < u->x->ndims; d++)
		for (int k = 0; k < u->x->dim[d].nkeys; k++)
			if (sift_v6_xdim_key_is_col(&u->x->dim[d], k) && u->x->dim[d].fsrc[k] == SV6SRC_ATTR)
			{
				const SiftV6Col key = sift_v6_xdim_key_col(&u->x->dim[d], k);

				sift_v1_unnest_col_index(u, &key, false);
			}
	for (int i = 0; i < u->x->nrcol; i++)
		if (reads_batch(&u->x->rcol[i]))
			sift_v1_unnest_col_index(u, &u->x->rcol[i], false);
	for (int k = 0; k < spec->nderive; k++)
	{
		const SiftV6Derive *d = &spec->derive[k];

		for (int c = 0; c <= d->ncond; c++)
			if ((c == 0 ? d->src : d->cond[c - 1]).src == SV6SRC_ATTR)
				sift_v1_unnest_col_index(u, c == 0 ? &d->src : &d->cond[c - 1], false);
		if (d->kind == SVD_PG_EXPR)
		{
			SiftV6PgItem it;

			sift_v6_pgexpr_item(spec, d, &it);
			for (int c = 0; c < it.nin; c++)
				if (reads_batch(&it.col[c]))
					sift_v1_unnest_col_index(u, &it.col[c], false);
		}
	}
	for (int i = 0; u->x->where != NIL && i < spec->nfcols; i++)
		if (reads_batch(&spec->fcol[i]))
			sift_v1_unnest_col_index(u, &spec->fcol[i], false);
	for (int k = 0; k < u->ntests; k++)
		for (int c = 0; c < u->x->test[k].ncmps; c++)
		{
			const SiftV6XCmp *cmp = &u->x->test[k].cmp[c];
			const SiftV6Col outer = {.attno = cmp->outer,.typid = cmp->outer_type};

			u->outer_slot[k * SIFT_V6_XCMPS + c] = sift_v1_unnest_col_index(u, &outer, false);
		}
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
bind_pg(UnRun *u, int k)
{
	SiftV6PgItem it;
	SiftV6PgIn	pin[SIFT_V6_MAX_COLS];

	sift_v6_pgexpr_item(u->spec, &u->spec->derive[k], &it);
	for (int c = 0; c < it.nin; c++)
	{
		u->pgslot[k][c] = slot_of(u, &it.col[c], false);
		pin[c] = (SiftV6PgIn) {.typid = it.col[c].typid,.scale = it.col[c].scale};
	}
	u->pg[k] = sift_v6_pgexpr_of(CurrentMemoryContext, u->spec, &u->spec->derive[k], &it, pin);
	sift_vx_out_init(&u->pgout[k], CurrentMemoryContext);
}

static void
setup_pipe(UnRun *u, SiftV6Group *g, const SiftV6StarRun *star)
{
	const SiftV6Spec *spec = u->spec;
	SiftVecType type[OUT_SLOTS];
	const int	nslots = u->ncols + u->nel + spec->nderive + u->ntests + (u->x->elem.semi ? 1 : 0);

	for (int i = 0; i < u->ncols; i++)
		type[i] = col_vec_type(&u->cols[i]);
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

		if (d->kind == SVD_PG_EXPR)
		{
			bind_pg(u, k);
			continue;
		}
		ar->nin = d->ncond + 1;
		for (int c = 0; c < ar->nin; c++)
		{
			const SiftV6Col *col = c == 0 ? &d->src : &d->cond[c - 1];

			ar->in[c] = slot_of(u, col, false);
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

static void
restrict_where(UnRun *u, const SiftV6Batch *b)
{
	SiftBatchSel *bs = &u->bs;
	uint64		keep[SIFT_VEC_WORDS];

	for (int i = 0; i < u->spec->nfcols; i++)
		u->wvecs[i] = u->wcol[i] < 0 ? NULL : (SiftVec *) &b->col[u->wcol[i]];
	sift_clause_eval(&u->where, u->wvecs, b->n, keep);
	for (int w = 0; w < (b->n + 63) / 64; w++)
		bs->mask[w] &= keep[w];
}

/* the rows of the batch the quals no scan key says keep, decided on the columns as the statement reads them; a row whose array the leaves cannot hold is the heap's, which tests it */
static void
restrict_quals(UnRun *u, const SiftV6Batch *b, const SiftV1ElemBatch *eb)
{
	SiftBatchSel *bs = &u->bs;
	const int	words = (b->n + 63) / 64;
	uint64		heap[SIFT_VEC_WORDS];

	bs->n_raw = b->n;
	bs->n_active = 0;
	for (int w = 0; w < words; w++)
	{
		heap[w] = bs->mask[w] & eb->poison[w];
		bs->mask[w] &= ~heap[w];
		bs->n_active += sift_pc64(bs->mask[w]);
	}
	bs->all_active = bs->n_active == bs->n_raw;
	bs->has_sel = false;
	for (int i = 0; i < u->nbq; i++)
		u->bqv[i] = u->fv[u->bqcol[i]];
	sift_v6_bq_filter(u->bq, bs);
	for (int w = 0; w < words; w++)
		bs->mask[w] |= heap[w];
}

/* the pairs of the rows of the batch whose elements the leaves hold; the rows they cannot hold (poison) are read from their heap row */
static void
emit_elems(UnRun *u, const SiftV6Batch *b, const SiftV1ElemBatch *eb)
{
	SiftVec		ev[SIFT_V6_XELEM_COLS];
	SiftUnnestIn in = {.lay = &eb->layout,.parent_mask = u->bs.mask,.poison = eb->poison,.doc_valid = eb->doc_valid,.left = u->x->elem.left};
	SiftUnnestCursor cur = {0};
	uint64		fallback[SIFT_VEC_WORDS] = {0};

	MemoryContextReset(u->bcx);
	for (int c = 0; c < u->nel; c++)
		ev[c] = sift_v6_elem_col_vec(&u->x->elem, c, u->etype[c], eb->child_data[c], eb->child_valid[c], eb->layout.n_elems, u->bcx);
	if (u->eev != NULL)
	{
		uint64	   *keep = MemoryContextAllocZero(u->bcx, ((eb->layout.n_elems + 63) / 64 + 1) * sizeof(uint64));

		sift_v6_elem_eval(u->eev, ev, 0, eb->layout.n_elems, &in, keep);
		in.elem_mask = keep;
	}
	while (!sift_unnest_done(&in, &cur))
	{
		const uint32 m = sift_unnest_expand(&in, &cur, u->pidx, u->eidx, SIFT_VEC_LEN, fallback);

		if (m > 0)
			sift_v1_unnest_emit_pairs(u, u->fv, ev, m);
	}
	for (int w = 0; w < (b->n + 63) / 64; w++)
		for (uint64 bits = fallback[w]; bits != 0; bits &= bits - 1)
			sift_v1_unnest_emit_heap_row(u, b, w * 64 + (uint32) __builtin_ctzll(bits));
}

void
sift_v1_unnest_batch(UnRun *u, const SiftV6Batch *b, const SiftV1ElemBatch *eb)
{
	SiftBatchSel *bs = &u->bs;
	const int	words = (b->n + 63) / 64;

	u->stats->batches++;
	if (u->row_slot >= 0)
		sift_v6_pipe_semi_reset(u->pipe);
	memset(bs->mask, 0, sizeof(bs->mask));
	for (int w = 0; w < words; w++)
		bs->mask[w] = b->mask != NULL ? b->mask[w] : ~UINT64CONST(0);
	if (b->n & 63)
		bs->mask[words - 1] &= (UINT64CONST(1) << (b->n & 63)) - 1;
	if (u->has_where)
		restrict_where(u, b);
	compute_columns(u, b, bs);
	if (u->bq != NULL && eb != NULL)
		restrict_quals(u, b, eb);
	for (int w = 0; w < words; w++)
		u->stats->rows += sift_pc64(bs->mask[w]);
	if (u->ntests > 0)
		sift_v1_unnest_run_tests(u, b, eb);
	else if (eb != NULL)
		emit_elems(u, b, eb);
	else
		for (int w = 0; w < words; w++)
			for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
				sift_v1_unnest_emit_heap_row(u, b, w * 64 + (uint32) __builtin_ctzll(bits));
}

static const SiftVec *
bq_col(void *arg, const SiftV6InCol *c)
{
	UnRun	   *u = arg;
	const SiftV6Col col = sift_v6_vatt_is_json(c->attno) ? sift_v6_spec_vatt_col(u->spec, c->attno, c->typid, c->scale) : (SiftV6Col) {.attno = c->attno,.typid = c->typid,.scale = c->scale};

	u->bqcol[u->nbq] = sift_v1_unnest_col_index(u, &col, false);
	return &u->bqv[u->nbq++];
}

/* the quals of the table no scan key says, over the columns of the batch the source decodes */
static void
setup_bq(UnRun *u, Relation host)
{
	SiftV6Input in;

	if (u->spec->vqual == NIL)
		return;
	if (u->spec->is_table)
		sift_v6_input_init_table(&in, host);
	else
		sift_v6_input_init_rel(&in, host);
	u->bq = sift_v6_bq_open(CurrentMemoryContext, u->spec->vqual, &in, bq_col, u, u->spec->exec);
}

static void
setup_clauses(UnRun *u, Relation host)
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
			u->wcol[i] = reads_batch(&spec->fcol[i]) ? sift_v1_unnest_col_index(u, &spec->fcol[i], false) : -1;
	}
	if (u->x->elem_where != NIL)
		u->eev = sift_v6_elem_eval_open(CurrentMemoryContext, u->x->elem_where);
	setup_bq(u, host);
	sift_v1_unnest_setup_tests(u);
}

/* the expression reads a virtual attribute of the scan (a leaf of a key of a document), which no column of the heap row is */
static bool
reads_virtual(Node *node, void *host)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR && ((Var *) node)->varattno > RelationGetNumberOfAttributes((Relation) host))
		return true;
	return expression_tree_walker(node, reads_virtual, host);
}

/*
 * the quals of the table over its stored columns, and the keys of the dimensions of a star join on columns of the index (keys), as the source decides them, so that it reads no unit their bounds
 * leave out (the batches are filtered by all the quals, and the pairs by the join, again)
 */
static SiftV6Vexpr *
source_quals(const UnRun *u, Relation host, ScanKey keys, int nkeys, SiftV6Input *in)
{
	SiftV6Input probe = *in;
	SiftV6Vexpr *vx = palloc0(sizeof(SiftV6Vexpr));
	List	   *stored = NIL;

	foreach_ptr(Node, q, u->x->where)
		if (!reads_virtual(q, host))
			stored = lappend(stored, q);
	stored = list_concat(sift_srcfold_heap_quals(host, stored), sift_srcfold_scankey_quals(host, keys, nkeys));
	if (stored == NIL || !sift_v6_vexpr_quals(CurrentMemoryContext, stored, 1, NULL, &probe, vx))
		return NULL;
	*in = probe;
	return vx;
}

/* the columns the source of the run gives (u->in) and the elements it reads with them (u->espec when u->has_elems) */
static void
source_input(UnRun *u, Relation heap, Relation index)
{
	const SiftSrcLayout *lay = sift_v1_reader_of(u->spec->is_table)->layout(u->spec->is_table ? heap : index);
	SiftVecType type[SIFT_V6_XELEM_COLS];
	int			leaf[SIFT_V6_XELEM_COLS];

	if (u->spec->is_table)
		sift_v6_input_init_table(&u->in, heap);
	else
		sift_v6_input_init_rel(&u->in, index);
	for (int i = 0; i < u->ncols; i++)
		u->in.col[i] = (SiftV6InCol) {.attno = u->cols[i].attno,.typid = u->cols[i].typid,.scale = u->cols[i].scale,.type = col_vec_type(&u->cols[i])};
	u->in.ncols = u->ncols;
	for (int c = 0; c < u->nel; c++)
		type[c] = un_vec_type(sift_v6_class(sift_v6_elem_read_type(&u->x->elem, c)));
	memcpy(leaf, u->x->elem.col_leaf, sizeof(leaf));
	u->has_elems = sift_v1_elem_spec(lay, u->x->elem.group_leaf, u->nel, leaf, type, u->x->elem.col_kind, &u->espec);
	u->espec.objects = u->x->elem.fn == SV6E_RECORDSET || u->x->elem.fn == SV6E_PATH;
}

void
sift_v1_unnest_open(UnRun *u, const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, SiftV6ScanStats *stats, const SiftV6StarRun *star)
{
	*u = (UnRun) {.spec = spec};
	u->row_slot = -1;
	u->x = spec->ext;
	u->index = index;
	u->stats = stats;
	u->nel = u->x->elem.ncols;
	u->ntests = u->x->ntests;
	for (int c = 0; c < u->nel; c++)
		u->etype[c] = un_vec_type(sift_v6_class(u->x->elem.col_type[c]));
	u->heap_att = un_heap_column(u, u->x->elem.attno);
	if (u->x->elem.fn == SV6E_UNNEST)
		get_typlenbyvalalign(u->x->elem.type, &u->typlen, &u->typbyval, &u->typalign);
	else if (u->x->elem.fn != SV6E_PATH)
		u->rec = sift_v6_recfn_new(CurrentMemoryContext, &u->x->elem, u->x->elem_names);
	u->bcx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 unnest heap rows");
	u->pidx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	u->eidx = palloc(SIFT_VEC_LEN * sizeof(uint32));
	u->month_key = palloc0((1 << MONTH_BITS) * sizeof(int32));
	u->month_val = palloc0((1 << MONTH_BITS) * sizeof(int32));
	u->month_unit = CStringGetTextDatum("month");
	sift_v1_unnest_heap_open(u, heap);
	u->hqual = sift_v1_unnest_heap_qual(u);
	if (u->x->elem_array != NIL)
		u->arr = sift_v1_unnest_heap_expr(u, linitial(u->x->elem_array));
	else
		sift_v1_unnest_heap_att(u, u->heap_att);
	u->probe = u->rec != NULL ? sift_v6_late_probe_open(u->x->late_probe, u->x->late_where, u->estate, u->rec) : NULL;
	foreach_ptr(Var, v, pull_var_clause((Node *) u->x->late_probe, 0))
		if (v->varno != INNER_VAR)
			sift_v1_unnest_heap_att(u, v->varattno);
	setup_cols(u);
	setup_clauses(u, spec->is_table ? heap : index);
	setup_pipe(u, g, star);
	for (int i = 0; i < u->ncols; i++)
		if (u->cols[i].month)
		{
			u->mdata[i] = MemoryContextAllocAligned(CurrentMemoryContext, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
			u->mvalid[i] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		}
	source_input(u, heap, index);
}

void
sift_v1_unnest_read(UnRun *u, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, const SiftV1Range *range)
{
	SiftV6Batch b;
	SiftV1Scan	scan = {.range = range,.elem = u->has_elems ? &u->espec : NULL};

	u->src = sift_v1_source_open(CurrentMemoryContext, snap, &u->in, source_quals(u, u->spec->is_table ? heap : index, keys, nkeys, &u->in), &scan);
	while (u->src->next(u->src, &b))
		sift_v1_unnest_batch(u, &b, u->has_elems ? sift_v1_source_elems(u->src) : NULL);
	u->src->close(u->src);
	u->src = NULL;
}

void
sift_v1_unnest_close(UnRun *u)
{
	sift_v6_pipe_finish(u->pipe);
	sift_v1_unnest_heap_close(u);
	MemoryContextDelete(u->bcx);
}

void
sift_v1_unnest_run(const SiftV6Spec *spec, SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, SiftV6ScanStats *stats, const SiftV6StarRun *star)
{
	UnRun		run;

	sift_v1_unnest_open(&run, spec, g, heap, index, stats, star);
	sift_v1_unnest_read(&run, heap, index, keys, nkeys, snap, NULL);
	sift_v1_unnest_close(&run);
}
