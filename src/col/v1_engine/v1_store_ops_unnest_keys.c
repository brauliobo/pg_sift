/*
 * col/v1_engine/v1_store_ops_unnest_keys.c - the members of a map a statement reads by key (SV6E_KEYS, agg/v6/v6_ext.h) over Format 1: a row of the table is one pair, whose columns are the members its
 * keys name in the keys and values of the SCL_OBJECT leaf of the map (or the typed leaf of a key of the document); a row whose map the leaves cannot hold, or whose member the column cannot take
 * as the statement casts it, is read from the heap row, where the expression of each column gives the value as PostgreSQL does, errors included.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "col/v1_engine/v1_store_ops_unnest.h"

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
	sift_v1_unnest_run_pairs(u, m);
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

/* a row whose map the leaves cannot hold: the row of the heap is the pair, each column read from it as the statement reads it, errors included */
static void
keys_from_heap(UnRun *u, uint64 tag, uint32 row)
{
	MemoryContext old = MemoryContextSwitchTo(u->bcx);

	sift_v1_unnest_heap_load(u, tag);
	u->pidx[0] = row;
	for (int i = 0; i < u->ncols; i++)
		sift_vec_gather(u->out[i], u->out_valid[i], u->out_data[i], &u->fv[i], u->pidx, 1);
	for (int c = 0; c < u->nel; c++)
		heap_member_cell(u, c);
	sift_v1_unnest_run_pairs(u, 1);
	MemoryContextSwitchTo(old);
}

void
sift_v1_unnest_run_keyed(UnRun *u, const SiftV6Batch *b, const SiftV1ElemBatch *eb)
{
	const int	words = (b->n + 63) / 64;
	const bool	grouped = u->x->elem.group_leaf >= 0;
	uint64		fallback[SIFT_VEC_WORDS] = {0};
	uint64		rows[SIFT_VEC_WORDS] = {0};
	SiftVec		vals = {0};
	uint32		m = 0;

	MemoryContextReset(u->bcx);
	for (int w = 0; w < words; w++)
	{
		fallback[w] = grouped && eb == NULL ? u->bs.mask[w] : grouped ? u->bs.mask[w] & eb->poison[w] : 0;
		rows[w] = u->bs.mask[w] & ~fallback[w];
	}
	if (grouped && eb != NULL)
	{
		vals = (SiftVec) {.type = SVEC_F64,.data = (void *) eb->child_data[1],.valid = (uint64 *) eb->child_valid[1],.all_valid = false};
		for (int c = 0; c < u->nel; c++)
		{
			const char *key = strVal(list_nth(u->x->elem_names, c));

			if (u->lcol[c] < 0)
				sift_map_lookup(&eb->layout, eb->child_data[0], key, (uint32) strlen(key), rows, u->elem_of[c]);
		}
	}
	for (int w = 0; w < words; w++)
		for (uint64 bits = rows[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 row = w * 64 + (uint32) __builtin_ctzll(bits);
			const bool	has_doc = grouped && ((eb->doc_valid[row >> 6] >> (row & 63)) & 1);

			if (!members_fit(u, &vals, row, has_doc))
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
				emit_keyed(u, u->fv, &vals, m);
				m = 0;
			}
		}
	if (m > 0)
		emit_keyed(u, u->fv, &vals, m);
	for (int w = 0; w < words; w++)
		for (uint64 bits = fallback[w]; bits != 0; bits &= bits - 1)
			keys_from_heap(u, b->tag[w * 64 + (uint32) __builtin_ctzll(bits)], w * 64 + (uint32) __builtin_ctzll(bits));
}
