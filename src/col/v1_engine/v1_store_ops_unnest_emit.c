/*
 * col/v1_engine/v1_store_ops_unnest_emit.c - the pairs of (row, element) of the aggregate scan of a LATERAL unnest over Format 1 (v1_store_ops_unnest.c): the arithmetic and the PostgreSQL expressions over
 * the pairs, their emission to the groups, and the rows the leaves cannot hold, which are read from the heap row through the virtual row of the expressions of the run.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "col/v1_engine/v1_store_ops_unnest.h"

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
void
sift_v1_unnest_run_pairs(UnRun *u, uint32 m)
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

void
sift_v1_unnest_emit_pairs(UnRun *u, const SiftVec *fact, const SiftVec *elems, uint32 m)
{
	for (int i = 0; i < u->ncols; i++)
		sift_vec_gather(u->out[i], u->out_valid[i], u->out_data[i], &fact[i], u->pidx, m);
	for (int c = 0; c < u->nel; c++)
		if (u->eused[c])
			sift_vec_gather(u->out[u->ncols + c], u->out_valid[u->ncols + c], u->out_data[u->ncols + c], &elems[c], u->eidx, m);
	for (int k = 0; k < u->ntests; k++)
		if (u->flag_used[k])
			sift_vec_gather(u->out[flag_slot(u, k)], u->out_valid[flag_slot(u, k)], u->out_data[flag_slot(u, k)], &u->flagv[k], u->pidx, m);
	sift_v1_unnest_run_pairs(u, m);
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

void
sift_v1_unnest_emit_row(UnRun *u, const SiftVec *fact, const SiftVec *ev, const uint64 *keep, uint32 nel, uint32 row)
{
	uint32		m = 0;

	if (nel == 0 && u->x->elem.left)
	{
		u->pidx[0] = row;
		u->eidx[0] = SIFT_GATHER_NONE;
		sift_v1_unnest_emit_pairs(u, fact, ev, 1);
		return;
	}
	for (uint32 k = 0; k < nel; k++)
		if (keep == NULL || ((keep[k >> 6] >> (k & 63)) & 1))
		{
			u->pidx[m] = row;
			u->eidx[m++] = k;
			if (m == SIFT_VEC_LEN)
			{
				sift_v1_unnest_emit_pairs(u, fact, ev, m);
				m = 0;
			}
		}
	if (m > 0)
		sift_v1_unnest_emit_pairs(u, fact, ev, m);
}

/* ---- the row of the heap ---- */

void
sift_v1_unnest_heap_open(UnRun *u, Relation heap)
{
	MemoryContext old;

	u->estate = CreateExecutorState();
	old = MemoryContextSwitchTo(u->estate->es_query_cxt);
	u->econtext = CreateExprContext(u->estate);
	MemoryContextSwitchTo(old);
	u->heap_slot = MakeSingleTupleTableSlot(RelationGetDescr(heap), &TTSOpsVirtual);
}

void
sift_v1_unnest_heap_close(UnRun *u)
{
	ExecDropSingleTupleTableSlot(u->heap_slot);
	FreeExecutorState(u->estate);
}

void
sift_v1_unnest_heap_att(UnRun *u, AttrNumber att)
{
	u->heap_atts = list_append_unique_int(u->heap_atts, att);
}

/* the expression over the columns of the heap row, prepared for the virtual row sift_v1_unnest_heap_load makes */
ExprState *
sift_v1_unnest_heap_expr(UnRun *u, Expr *e)
{
	MemoryContext old = MemoryContextSwitchTo(u->estate->es_query_cxt);
	ExprState  *state = ExecPrepareExpr(e, u->estate);

	MemoryContextSwitchTo(old);
	foreach_ptr(Var, v, pull_var_clause((Node *) e, 0))
		sift_v1_unnest_heap_att(u, v->varattno);
	return state;
}

void
sift_v1_unnest_heap_load(UnRun *u, uint64 tag)
{
	TupleTableSlot *slot = u->heap_slot;
	const int	n = list_length(u->heap_atts);
	bool		found;

	for (int i = 0; i < n; i++)
		u->hatt[i] = (AttrNumber) list_nth_int(u->heap_atts, i);
	found = u->src->fetch(u->src, tag, u->hatt, n, u->hval, u->hnull);
	SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when it is read from the heap");
	u->stats->heap_reread_rows++;
	ExecClearTuple(slot);
	memset(slot->tts_isnull, true, slot->tts_tupleDescriptor->natts * sizeof(bool));
	for (int i = 0; i < n; i++)
	{
		slot->tts_values[u->hatt[i] - 1] = u->hval[i];
		slot->tts_isnull[u->hatt[i] - 1] = u->hnull[i];
	}
	ExecStoreVirtualTuple(slot);
	u->econtext->ecxt_scantuple = slot;
	ResetExprContext(u->econtext);
}

/* the array of the row loaded: its column, or the expression of the columns of the row */
static Datum
heap_array(UnRun *u, bool *isnull)
{
	if (u->arr != NULL)
		return ExecEvalExprSwitchContext(u->arr, u->econtext, isnull);
	return slot_getattr(u->heap_slot, u->heap_att, isnull);
}

uint32
sift_v1_unnest_heap_elems(UnRun *u, SiftVec *ev)
{
	bool		isnull;
	const Datum d = heap_array(u, &isnull);

	if (isnull)
		return 0;
	return u->rec != NULL ? sift_v6_recfn_call(u->rec, d, u->bcx, ev) : array_elems(u, d, ev);
}

/* a row of the batch: the array of its heap row taken apart as the function does, NULL and empty arrays included, errors included */
void
sift_v1_unnest_emit_heap_row(UnRun *u, const SiftV6Batch *b, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	SiftVec		ev[SIFT_V6_XELEM_COLS] = {0};
	uint64	   *keep = NULL;
	uint32		nel;

	MemoryContextReset(u->bcx);
	sift_v1_unnest_heap_load(u, b->tag[row]);
	nel = sift_v1_unnest_heap_elems(u, ev);
	if (u->eev != NULL && nel > 0)
	{
		keep = palloc0(((nel + 63) / 64 + 1) * sizeof(uint64));
		sift_v6_elem_eval(u->eev, ev, 0, nel, keep);
	}
	if (u->rec != NULL)
		sift_v6_recfn_values(u->rec, keep, u->bcx, ev, nel);
	sift_v1_unnest_emit_row(u, u->fv, ev, keep, nel, row);
	MemoryContextSwitchTo(old);
}
