/*
 * col/v1_engine/store_ops_unnest_test.c - the tests of the elements of a row's array (SiftV6Ext.test: EXISTS, jsonpath) over Format 1: the rows of a batch whose array the leaves hold are decided on the
 * element columns (agg/unnest/test.h), each row once with a flag for each test; the others are decided from their heap row, which gives the answers and the errors of PostgreSQL.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "col/v1_engine/store_ops_unnest.h"

void
sift_v1_unnest_setup_tests(UnRun *u)
{
	int			k = 0;

	if (u->ntests == 0)
		return;
	u->tests = sift_v6_row_tests_open(CurrentMemoryContext, u->x);
	foreach_ptr(List, tq, u->x->test_quals)
	{
		for (int c = 0; c < u->x->test[k].ncmps; c++)
		{
			u->outer_att[k * SIFT_V6_XCMPS + c] = un_heap_column(u, u->x->test[k].cmp[c].outer);
			sift_v1_unnest_heap_att(u, u->outer_att[k * SIFT_V6_XCMPS + c]);
		}
		if (lsecond(tq) != NIL)
			u->heap_test[k] = sift_v1_unnest_heap_expr(u, linitial(lsecond(tq)));
		u->flag_data[k] = palloc0(SIFT_VEC_LEN * sizeof(int64));
		u->flag_valid[k] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		k++;
	}
}

static void
elem_vecs(const UnRun *u, const SiftV1ElemBatch *eb, SiftVec *ev)
{
	for (int c = 0; c < u->nel; c++)
		ev[c] = sift_v6_elem_col_vec(&u->x->elem, c, u->etype[c], eb->child_data[c], eb->child_valid[c], eb->layout.n_elems, u->bcx);
}

/* the tests of the rows of the batch the layout holds, and the rows of those that fail one the statement keeps its rows by leave the batch; the others are the heap's */
static void
decide_tests(UnRun *u, const SiftV6Batch *b, const SiftV1ElemBatch *eb)
{
	const uint32 words = (b->n + 63) / 64;
	SiftVec		ev[SIFT_V6_XELEM_COLS];

	for (uint32 w = 0; w < words; w++)
		u->held[w] = u->bs.mask[w] & ~eb->poison[w];
	elem_vecs(u, eb, ev);
	sift_v6_row_tests_eval(u->tests, &(SiftV6StreamIn) {.lay = &eb->layout,.doc_valid = eb->doc_valid,.ev = ev}, b->col, u->outer_slot, u->held, u->yes, u->known);
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

/* the rows of the batch the tests keep: one pair each, whose columns are those of the table and the flags; the rows the leaves cannot hold are the caller's, from the heap */
static void
emit_tested(UnRun *u, uint32 n, uint64 *fallback)
{
	const uint32 words = (n + 63) / 64;
	uint32		m = 0;

	fill_flags(u, n);
	for (uint32 w = 0; w < words; w++)
	{
		fallback[w] = u->bs.mask[w] & ~u->held[w];
		for (uint64 bits = u->bs.mask[w] & u->held[w]; bits != 0; bits &= bits - 1)
		{
			u->pidx[m] = w * 64 + (uint32) __builtin_ctzll(bits);
			u->eidx[m++] = SIFT_GATHER_NONE;
			if (m == SIFT_VEC_LEN)
			{
				sift_v1_unnest_emit_pairs(u, u->fv, NULL, m);
				m = 0;
			}
		}
	}
	if (m > 0)
		sift_v1_unnest_emit_pairs(u, u->fv, NULL, m);
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
	const Datum d = slot_getattr(u->heap_slot, u->outer_att[k * SIFT_V6_XCMPS + c], &isnull);

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

/* every test of the heap row: a jsonpath is the call itself, an EXISTS the test of the elements the function gives; *err: the error of a function the join's conditions alone read */
static void
heap_flags(UnRun *u, bool *yes, bool *known, ErrorData **err)
{
	SiftVec		ev[SIFT_V6_XELEM_COLS] = {0};
	ErrorData **late = NULL;
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
		if (!have && (nel = sift_v1_unnest_heap_elems(u, ev, sift_v6_stream_joined(u->x, 0) ? err : NULL)) > 0 && u->rec != NULL)
		{
			late = MemoryContextAllocZero(u->bcx, nel * sizeof(ErrorData *));
			sift_v6_recfn_values(u->rec, NULL, u->bcx, ev, nel, late);
		}
		have = true;
		if (*err != NULL)
			return;
		for (int c = 0; c < u->x->test[k].ncmps; c++)
			outer[c] = heap_outer(u, k, c, &cell[c], &valid[c]);
		yes[k] = nel > 0 && sift_v6_row_test_one(u->tests, k, ev, nel, outer, late);
	}
}

static bool
need_met(const SiftV6XTest *xt, bool yes, bool known)
{
	return xt->need == 0 || (xt->need == 1 ? yes : xt->nullable ? known && !yes : !yes);
}

/* a row the leaves cannot hold: its tests from the heap row, the pair it gives when the statement keeps it */
static void
tests_from_heap(UnRun *u, uint64 tag, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);
	bool		yes[SIFT_V6_XTESTS],
				known[SIFT_V6_XTESTS];
	bool		kept = sift_v1_unnest_heap_load(u, tag);
	ErrorData  *err = NULL;

	if (kept)
		heap_flags(u, yes, known, &err);
	if (err != NULL)
	{
		u->pidx[0] = row;
		u->eidx[0] = SIFT_GATHER_NONE;
		u->pipe->late_row = err;
		sift_v1_unnest_emit_pairs(u, u->fv, NULL, 1);
		u->pipe->late_row = NULL;
		kept = false;
	}
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
		sift_v1_unnest_emit_pairs(u, u->fv, NULL, 1);
	}
	MemoryContextSwitchTo(old);
}

void
sift_v1_unnest_run_tests(UnRun *u, const SiftV6Batch *b, const SiftV1ElemBatch *eb)
{
	const uint32 words = (b->n + 63) / 64;
	uint64		fallback[SIFT_VEC_WORDS] = {0};

	MemoryContextReset(u->bcx);
	memset(u->held, 0, sizeof(u->held));
	if (eb != NULL)
		decide_tests(u, b, eb);
	emit_tested(u, b->n, fallback);
	for (uint32 w = 0; w < words; w++)
		for (uint64 bits = fallback[w]; bits != 0; bits &= bits - 1)
			tests_from_heap(u, b->tag[w * 64 + (uint32) __builtin_ctzll(bits)], w * 64 + (uint32) __builtin_ctzll(bits));
}
