/*
 * col/v1_engine/store_ops_unnest_emit.c - the pairs of (row, element) of the aggregate scan of a LATERAL unnest over Format 1 (v1_store_ops_unnest.c): the arithmetic and the PostgreSQL expressions over
 * the pairs, their emission to the groups, and the rows the leaves cannot hold, which are read from the heap row through the virtual row of the expressions of the run.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "agg/pg_exec.h"
#include "col/v1_engine/store_ops_unnest.h"

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
		bool		ok = (u->obs.mask[r >> 6] >> (r & 63)) & 1;

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

typedef struct LatePair
{
	UnRun	   *u;
	uint32		member;
	ErrorData  *err;
} LatePair;

/* the pair reached the groups: the error is the statement's when a key or an aggregate whose FILTER passes reads the member; otherwise the pair goes to the groups without its value */
static void
reach_late(void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	const LatePair *lp = arg;

	if (bs->n_active == 0)
		return;
	sift_v6_late_probe_check(lp->u->probe, lp->u->econtext, lp->member, lp->err);
	sift_v6_group_update(lp->u->pipe->group, bs, keyvec, argvec);
}

/* the pair of a member whose cast raised: decided when it reaches the groups, gone with it when a join drops it */
static void
emit_late_pair(UnRun *u, const SiftVec *fact, const SiftVec *ev, uint32 row, uint32 k, ErrorData *err)
{
	LatePair	lp = {u, k, err};

	u->pidx[0] = row;
	u->eidx[0] = k;
	u->pipe->sink = reach_late;
	u->pipe->sink_arg = &lp;
	sift_v1_unnest_emit_pairs(u, fact, ev, 1);
	u->pipe->sink = NULL;
	u->pipe->sink_arg = NULL;
}

/* late[k]: the error of the cast of member k (NULL: none) */
void
sift_v1_unnest_emit_row(UnRun *u, const SiftVec *fact, const SiftVec *ev, const uint64 *keep, uint32 nel, uint32 row, ErrorData *const *late)
{
	uint32		m = 0;

	if (u->x->elem.left && sift_unnest_count_bits(keep, 0, nel) == 0)
	{
		u->pidx[0] = row;
		u->eidx[0] = SIFT_GATHER_NONE;
		sift_v1_unnest_emit_pairs(u, fact, ev, 1);
		return;
	}
	for (uint32 k = 0; k < nel; k++)
		if (late != NULL && late[k] != NULL)
		{
			if (m > 0)
				sift_v1_unnest_emit_pairs(u, fact, ev, m);
			m = 0;
			emit_late_pair(u, fact, ev, row, k, late[k]);
		}
		else if (keep == NULL || ((keep[k >> 6] >> (k & 63)) & 1))
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
	u->econtext = sift_v6_exec_context(u->spec);
	MemoryContextSwitchTo(old);
	u->heap_slot = MakeSingleTupleTableSlot(RelationGetDescr(heap), &TTSOpsVirtual);
	u->heap_slot->tts_tableOid = RelationGetRelid(heap);
}

void
sift_v1_unnest_heap_close(UnRun *u)
{
	ExecDropSingleTupleTableSlot(u->heap_slot);
	FreeExprContext(u->econtext, true);
	FreeExecutorState(u->estate);
}

/* a column of the table the virtual row holds; a system column (ctid, tableoid) is the address of the row and its table, which the virtual row has without a read */
void
sift_v1_unnest_heap_att(UnRun *u, AttrNumber att)
{
	if (att > 0)
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

/* the clauses of the WHERE the node prepared (SiftV6Exec.heap_qual), a qual of the virtual row sift_v1_unnest_heap_load makes of the columns they read */
ExprState *
sift_v1_unnest_heap_qual(UnRun *u)
{
	foreach_ptr(Var, v, pull_var_clause((Node *) u->x->heap_where, 0))
		sift_v1_unnest_heap_att(u, v->varattno);
	return u->spec->exec->heap_qual;
}

bool
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
	ItemPointerSet(&slot->tts_tid, (BlockNumber) (tag >> 16), (OffsetNumber) (tag & 0xFFFF));
	u->econtext->ecxt_scantuple = slot;
	ResetExprContext(u->econtext);
	return u->hqual == NULL || ExecQual(u->hqual, u->econtext);
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
sift_v1_unnest_heap_elems(UnRun *u, SiftVec *ev, ErrorData **err)
{
	bool		isnull;
	const Datum d = heap_array(u, &isnull);

	if (isnull)
		return 0;
	return u->rec != NULL ? sift_v6_recfn_call(u->rec, d, u->bcx, ev, err) : array_elems(u, d, ev);
}

/* the row of an EXISTS whose function raised: one pair with no element, which raises the error when it reaches the test (agg/pipe.h) */
static void
emit_late_row(UnRun *u, const SiftVec *ev, uint32 row, ErrorData *err)
{
	u->pidx[0] = row;
	u->eidx[0] = SIFT_GATHER_NONE;
	u->pipe->late_row = err;
	sift_v1_unnest_emit_pairs(u, u->fv, ev, 1);
	u->pipe->late_row = NULL;
}

/* a row of the batch: the array of its heap row taken apart as the function does, NULL and empty arrays included, errors included */
void
sift_v1_unnest_emit_heap_row(UnRun *u, const SiftV6Batch *b, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	SiftVec		ev[SIFT_V6_XELEM_COLS] = {0};
	const uint64 *keep;
	ErrorData **late;
	ErrorData  *err = NULL;
	uint32		nel;

	for (int c = 0; c < u->nel; c++)
		ev[c].type = u->etype[c];
	MemoryContextReset(u->bcx);
	if (!sift_v1_unnest_heap_load(u, b->tag[row]))
	{
		MemoryContextSwitchTo(old);
		return;
	}
	nel = sift_v1_unnest_heap_elems(u, ev, u->x->elem.semi ? &err : NULL);
	if (err != NULL)
	{
		emit_late_row(u, ev, row, err);
		MemoryContextSwitchTo(old);
		return;
	}
	keep = sift_v6_late_keep(u->probe, u->eev, u->rec, u->econtext, u->bcx, ev, nel, &late);
	sift_v1_unnest_emit_row(u, u->fv, ev, keep, nel, row, late);
	MemoryContextSwitchTo(old);
}
