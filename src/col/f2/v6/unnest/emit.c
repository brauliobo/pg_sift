/*
 * col/f2/v6/unnest/emit.c - the pairs of (row, element) of the aggregate scan of a LATERAL unnest (f2_v6_unnest_run.c): the arithmetic and the PostgreSQL expressions over the pairs, their emission
 * to the groups, and the rows the index cannot hold, which are read from the heap.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "col/f2/v6/unnest/int.h"

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

static void
raise_late(void *err, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	ReThrowError((ErrorData *) err);
}

/* the pair of a member whose cast raised: the error is the pair's, raised when it reaches the groups and gone with it when a join drops it; a qual over the members casts it before the join */
static void
emit_late_pair(UnRun *u, const SiftVec *fact, const SiftVec *ev, uint32 row, uint32 k, ErrorData *err)
{
	if (u->has_pwhere)
		ReThrowError(err);
	u->pidx[0] = row;
	u->eidx[0] = k;
	u->pipe->sink = raise_late;
	u->pipe->sink_arg = err;
	sift_f2_unnest_emit_pairs(u, fact, ev, 1);
	u->pipe->sink = NULL;
	u->pipe->sink_arg = NULL;
}

/* the pairs of one row whose elements are in ev: those the bitmap keeps (NULL: all), and for a left join a NULL element when it has none; late[k]: the error of the cast of member k (NULL: none) */
static void
emit_row(UnRun *u, const SiftVec *fact, const SiftVec *ev, const uint64 *keep, uint32 nel, uint32 row, ErrorData *const *late)
{
	uint32		m = 0;

	if (nel == 0 && u->x->elem.left)
	{
		u->pidx[0] = row;
		u->eidx[0] = SIFT_GATHER_NONE;
		sift_f2_unnest_emit_pairs(u, fact, ev, 1);
		return;
	}
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
}

/* the row of the heap at tid is read, and the quals of the table as the statement has them hold of it */
static bool
heap_row_kept(UnRun *u, uint64 tid)
{
	const bool	found = sift_v6_vet_fetch(&u->vet, tid);

	SIFT_INVARIANT(found, "a row the aggregate scan keeps is there when its array is read");
	u->stats->heap_reread_rows++;
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
void
sift_f2_unnest_emit_heap_row(UnRun *u, const SiftVec *fact, uint64 tid, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	SiftVec		ev[SIFT_V6_XELEM_COLS] = {0};
	uint64	   *keep = NULL;
	ErrorData **late = NULL;
	uint32		nel;

	if (heap_row_kept(u, tid))
	{
		nel = heap_elems(u, ev);
		if (u->eev != NULL && nel > 0)
		{
			keep = palloc0(((nel + 63) / 64 + 1) * sizeof(uint64));
			sift_v6_elem_eval(u->eev, ev, 0, nel, keep);
		}
		if (u->rec != NULL)
		{
			late = palloc0_array(ErrorData *, nel);
			sift_v6_recfn_values(u->rec, keep, u->bcx, ev, nel, late);
		}
		emit_row(u, fact, ev, keep, nel, row, late);
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
		yes[k] = nel > 0 && sift_v6_row_test_one(u->tests, k, ev, nel, outer);
	}
}

static bool
need_met(const SiftV6XTest *xt, bool yes, bool known)
{
	return xt->need == 0 || (xt->need == 1 ? yes : xt->nullable ? known && !yes : !yes);
}

/* a row the leaves cannot hold: its tests from the heap row, the pair it gives when the statement keeps it */
void
sift_f2_unnest_emit_test_heap_row(UnRun *u, uint64 tid, uint32 row)
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
		sift_f2_unnest_emit_pairs(u, u->fv, NULL, 1);
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
void
sift_f2_unnest_run_keyed(UnRun *u, const SiftF2ExtBatch *b, const SiftVec *vals, uint64 *fallback)
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
void
sift_f2_unnest_emit_heap_keys(UnRun *u, const SiftVec *fact, uint64 tid, uint32 row)
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
