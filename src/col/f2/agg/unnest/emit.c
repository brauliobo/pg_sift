/*
 * col/f2/agg/unnest/emit.c - the pairs of (row, element) of the aggregate scan of a LATERAL unnest (f2_v6_unnest_run.c): the arithmetic and the PostgreSQL expressions over the pairs, their emission
 * to the groups, and the rows the index cannot hold, which are read from the heap.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "col/f2/agg/unnest/int.h"

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
	if (bs->n_active > 0)
		sift_v6_pipe_run(u->pipe, u->out, bs);
}

/* the pairs of (row, element) into the vectors of the pairs, and on to the groups: pidx[i] names the row of fact and eidx[i] the element of elems of pair i (SIFT_GATHER_NONE: a NULL one) */
void
sift_f2_unnest_emit_pairs(UnRun *u, const SiftVec *fact, const SiftVec *elems, uint32 m)
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

/* the next elements of the array the iterator reads, at most SIFT_VEC_LEN, as columns in cx: unnest(array) of a NULL or empty array has none */
static uint32
array_elems(const UnStream *st, ArrayIterator it, SiftVec *ev, MemoryContext cx)
{
	const size_t w = sift_gather_width(st->etype[0]);
	uint32		k = 0;
	Datum		d;
	bool		isnull;

	ev[0] = (SiftVec) {.type = st->etype[0],.all_valid = false};
	ev[0].data = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * w + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	ev[0].valid = MemoryContextAllocZero(cx, (SIFT_VEC_WORDS + 1) * sizeof(uint64));
	for (; k < SIFT_VEC_LEN && array_iterate(it, &d, &isnull); k++)
	{
		if (isnull)
			continue;
		ev[0].valid[k >> 6] |= UINT64CONST(1) << (k & 63);
		if (st->etype[0] == SVEC_STRVIEW)
		{
			struct varlena *t = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

			((SiftStrView *) ev[0].data)[k] = sift_vstr_make(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
		}
		else
			((int64 *) ev[0].data)[k] = st->el->type == INT2OID ? DatumGetInt16(d) : st->el->type == INT4OID ? DatumGetInt32(d) : DatumGetInt64(d);
	}
	return k;
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
	sift_f2_unnest_emit_pairs(u, fact, ev, 1);
	u->pipe->sink = NULL;
	u->pipe->sink_arg = NULL;
}

/* the pairs of the elements of a chunk of a row in ev: those the bitmap keeps (NULL: all); late[k]: the error of the cast of member k (NULL: none); returns how many it keeps */
static uint32
emit_chunk(UnRun *u, const SiftVec *fact, const SiftVec *ev, const uint64 *keep, uint32 nel, uint32 row, ErrorData *const *late)
{
	uint32		m = 0;

	for (uint32 k = 0; k < nel; k++)
		if (late != NULL && late[k] != NULL)
		{
			if (m > 0)
				sift_f2_unnest_emit_pairs(u, fact, ev, m);
			m = 0;
			emit_late_pair(u, fact, ev, row, k, late[k]);
		}
		else if (keep == NULL || ((keep[k >> 6] >> (k & 63)) & 1))
		{
			u->pidx[m] = row;
			u->eidx[m++] = k;
			if (m == SIFT_VEC_LEN)
			{
				sift_f2_unnest_emit_pairs(u, fact, ev, m);
				m = 0;
			}
		}
	if (m > 0)
		sift_f2_unnest_emit_pairs(u, fact, ev, m);
	return sift_unnest_count_bits(keep, 0, nel);
}

/*
 * the row of the heap at tid is read, and the quals of the table as the statement has them hold of it: the node prepared them for its virtual scan slot, so they read the row
 * deformed; then its members (row r of the batch) are read from it
 */
static bool
heap_row_kept(UnRun *u, uint64 tid, uint32 row)
{
	const bool	found = sift_v6_vet_fetch(&u->vet, tid);

	SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its array is read");
	u->stats->heap_reread_rows++;
	if (u->econtext != NULL)
	{
		slot_getallattrs(u->vet.slot);
		u->econtext->ecxt_scantuple = u->vet.slot;
		ResetExprContext(u->econtext);
	}
	if (u->hqual != NULL && !ExecQual(u->hqual, u->econtext))
		return false;
	sift_f2_unnest_member_heap_row(u, row);
	return true;
}

/* the elements of an array of the row read from the heap as the function gives them, a chunk at a time (heap_next), NULL and empty arrays included, errors included */
typedef struct HeapElems
{
	const UnStream *st;			/* the stream of the array */
	ArrayIterator it;			/* an array; NULL: the rows of the function of st->rec */
	bool		done;
} HeapElems;

static void
heap_open(UnRun *u, int s, HeapElems *h, ErrorData **err)
{
	const UnStream *st = &u->st[s];
	bool		isnull;
	const Datum d = st->arr != NULL ? ExecEvalExprSwitchContext(st->arr, u->econtext, &isnull) : slot_getattr(u->vet.slot, st->heap_att, &isnull);
	ArrayMetaState meta = {.element_type = st->el->type,.typlen = st->typlen,.typbyval = st->typbyval,.typalign = st->typalign};

	*h = (HeapElems) {.st = st,.done = isnull};
	if (isnull)
		return;
	if (st->rec != NULL)
		sift_v6_recfn_open(st->rec, d, u->rmt.root_cx, err);
	else
		h->it = array_create_iterator(DatumGetArrayTypeP(d), 0, &meta);
}

/* the next chunk of the elements, in ev in u->ccx, which the chunk before is reset from: how many, 0 at the end */
static uint32
heap_next(UnRun *u, HeapElems *h, SiftVec *ev)
{
	uint32		n;

	sift_mem_used(&u->rmt);
	MemoryContextReset(u->ccx);
	CHECK_FOR_INTERRUPTS();
	if (h->done)
		return 0;
	n = h->st->rec != NULL ? sift_v6_recfn_next(h->st->rec, u->ccx, ev, SIFT_VEC_LEN) : array_elems(h->st, h->it, ev, u->ccx);
	h->done = n < SIFT_VEC_LEN;
	return n;
}

/* the row of an EXISTS whose function raised: one pair with no element, which raises the error when it reaches the test (agg/pipe.h) */
static void
emit_late_row(UnRun *u, const SiftVec *fact, const SiftVec *ev, uint32 row, ErrorData *err)
{
	u->pidx[0] = row;
	u->eidx[0] = SIFT_GATHER_NONE;
	u->pipe->late_row = err;
	sift_f2_unnest_emit_pairs(u, fact, ev, 1);
	u->pipe->late_row = NULL;
}

/* the pairs of the heap row the quals of the table keep: the array of the pairs taken apart, a chunk at a time */
static void
heap_pairs(UnRun *u, const SiftVec *fact, uint32 row)
{
	SiftVec		ev[SIFT_V6_XELEM_COLS] = {0};
	HeapElems	h;
	uint32		kept = 0;
	uint32		nel;

	ErrorData  *err = NULL;

	for (int c = 0; c < u->nel; c++)
		ev[c].type = u->st[0].etype[c];
	heap_open(u, 0, &h, u->x->elem.semi ? &err : NULL);
	if (err != NULL)
	{
		emit_late_row(u, fact, ev, row, err);
		return;
	}
	MemoryContextSwitchTo(u->ccx);
	while ((nel = heap_next(u, &h, ev)) > 0)
	{
		ErrorData **late;
		const uint64 *keep = sift_v6_late_keep(u->probe, u->eev, u->st[0].rec, u->econtext, u->ccx, ev, nel, &late);

		kept += emit_chunk(u, fact, ev, keep, nel, row, late);
	}
	if (u->x->elem.left && kept == 0)
	{
		u->pidx[0] = row;
		u->eidx[0] = SIFT_GATHER_NONE;
		sift_f2_unnest_emit_pairs(u, fact, ev, 1);
	}
}

/* a row whose array the index cannot hold: the array of its heap row taken apart */
void
sift_f2_unnest_emit_heap_row(UnRun *u, const SiftVec *fact, uint64 tid, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->rmt.root_cx);

	if (heap_row_kept(u, tid, row))
		heap_pairs(u, fact, row);
	MemoryContextSwitchTo(old);
	MemoryContextResetOnly(u->rmt.root_cx);
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

/* the tests of stream s of the heap row that its elements decide (an EXISTS): the elements the function gives, a chunk at a time; the error of a function the join's conditions alone read is *err */
static void
heap_stream_flags(UnRun *u, int s, const bool *elems, SiftVec (*outer)[SIFT_V6_XCMPS], bool *yes, ErrorData **err)
{
	SiftVec		ev[SIFT_V6_XELEM_COLS] = {0};
	HeapElems	h;
	uint32		nel;

	heap_open(u, s, &h, sift_v6_stream_joined(u->x, s) ? err : NULL);
	while (*err == NULL && (nel = heap_next(u, &h, ev)) > 0)
		for (int k = 0; k < u->ntests; k++)
			if (elems[k] && u->x->test[k].stream == s && !yes[k])
				yes[k] = sift_v6_row_test_one(u->tests, k, ev, nel, outer[k]);
}

/* every test of the heap row: a jsonpath is the call itself, an EXISTS the test of the elements of its stream; *err as heap_stream_flags gives it */
static void
heap_flags(UnRun *u, bool *yes, bool *known, ErrorData **err)
{
	SiftVec		outer[SIFT_V6_XTESTS][SIFT_V6_XCMPS];
	HeapCell	cell[SIFT_V6_XTESTS][SIFT_V6_XCMPS];
	uint64		valid[SIFT_V6_XTESTS][SIFT_V6_XCMPS];
	bool		elems[SIFT_V6_XTESTS] = {0};
	bool		streams[SIFT_V6_XSTREAMS] = {0};

	for (int k = 0; k < u->ntests; k++)
	{
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
		elems[k] = streams[u->x->test[k].stream] = true;
		for (int c = 0; c < u->x->test[k].ncmps; c++)
			outer[k][c] = heap_outer(u, k, c, &cell[k][c], &valid[k][c]);
	}
	for (int s = 0; *err == NULL && s < u->nst; s++)
		if (streams[s])
			heap_stream_flags(u, s, elems, outer, yes, err);
}

static bool
need_met(const SiftV6XTest *xt, bool yes, bool known)
{
	return xt->need == 0 || (xt->need == 1 ? yes : xt->nullable ? known && !yes : !yes);
}

/* a row the leaves cannot hold: its tests from the heap row, and when the statement keeps it the pair it is (row_tests) or the pairs of its elements, with its flags */
void
sift_f2_unnest_emit_test_heap_row(UnRun *u, uint64 tid, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->rmt.root_cx);
	bool		yes[SIFT_V6_XTESTS],
				known[SIFT_V6_XTESTS];
	bool		kept = heap_row_kept(u, tid, row);
	ErrorData  *err = NULL;
	SiftVec		none[SIFT_V6_XELEM_COLS] = {0};

	if (kept)
		heap_flags(u, yes, known, &err);
	if (err != NULL)
	{
		for (int c = 0; c < u->nel; c++)
			none[c].type = u->st[0].etype[c];
		emit_late_row(u, u->fv, none, row, err);
		kept = false;
	}
	for (int k = 0; kept && k < u->ntests; k++)
		kept = need_met(&u->x->test[k], yes[k], known[k]);
	for (int k = 0; kept && k < u->ntests; k++)
	{
		u->flag_data[k][row] = yes[k];
		u->flag_valid[k][row >> 6] |= (uint64) (!u->x->test[k].nullable || known[k]) << (row & 63);
	}
	if (kept && u->x->row_tests)
	{
		u->pidx[0] = row;
		u->eidx[0] = SIFT_GATHER_NONE;
		sift_f2_unnest_emit_pairs(u, u->fv, NULL, 1);
	}
	else if (kept)
		heap_pairs(u, u->fv, row);
	MemoryContextSwitchTo(old);
	MemoryContextResetOnly(u->rmt.root_cx);
}
