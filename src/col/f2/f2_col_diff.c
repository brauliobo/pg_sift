/*
 * col/f2/f2_col_diff.c - what an UPDATE changed of the entries of a sift_col index (col/f2/f2_col_diff.h).
 */
#include "postgres.h"

#include "access/detoast.h"

#include "col/f2/f2_col_diff.h"
#include "table/f2/f2_col_delta.h"

bool
sift_datum_fast_equal(Datum a, Datum b)
{
	const struct varlena *pa = (const struct varlena *) DatumGetPointer(a),
			   *pb = (const struct varlena *) DatumGetPointer(b);

	if (VARATT_IS_EXTERNAL(pa) || VARATT_IS_EXTERNAL(pb))
	{
		struct varatt_external ea,
					eb;

		if (!VARATT_IS_EXTERNAL_ONDISK(pa) || !VARATT_IS_EXTERNAL_ONDISK(pb))
			return false;
		VARATT_EXTERNAL_GET_POINTER(ea, pa);
		VARATT_EXTERNAL_GET_POINTER(eb, pb);
		return memcmp(&ea, &eb, sizeof(ea)) == 0;
	}
	return VARSIZE_ANY(pa) == VARSIZE_ANY(pb) && memcmp(pa, pb, VARSIZE_ANY(pa)) == 0;
}

bool
sift_f2_att_equal(const SiftF2AttDesc *att, Datum oldv, bool oldnull, Datum newv, bool newnull)
{
	if (oldnull || newnull)
		return oldnull && newnull;
	if (att->attbyval)
		return oldv == newv;
	if (att->attlen > 0)
		return memcmp(DatumGetPointer(oldv), DatumGetPointer(newv), att->attlen) == 0;
	if (att->attlen == -1)
		return sift_datum_fast_equal(oldv, newv);
	return false;
}

uint64
sift_f2_att_diff(const SiftF2AttDesc *atts, int natts, const Datum *oldv, const bool *oldnull, const Datum *newv, const bool *newnull)
{
	uint64		mask = 0;

	Assert(natts <= 64);
	for (int a = 0; a < natts; a++)
		if (!sift_f2_att_equal(&atts[a], oldv[a], oldnull[a], newv[a], newnull[a]))
			mask |= UINT64CONST(1) << a;
	return mask;
}

SiftF2PredMove
sift_f2_pred_move(bool old_in, bool new_in)
{
	return old_in ? (new_in ? SIFT_PRED_BOTH : SIFT_PRED_EXITED) : (new_in ? SIFT_PRED_ENTERED : SIFT_PRED_NEITHER);
}

bool
sift_f2_cell_same(SiftCellKind kind, const SiftCell *a, const SiftCell *b)
{
	if (a->isnull != b->isnull || a->tag != b->tag)
		return false;
	if (a->xp != NULL || b->xp != NULL)
		return a->xplen == b->xplen && a->xp != NULL && b->xp != NULL && memcmp(a->xp, b->xp, a->xplen) == 0;
	if (a->isnull)
		return true;
	if (kind == SIFT_CELL_INT)
		return a->i == b->i;
	if (kind == SIFT_CELL_F8)
		return memcmp(&a->f, &b->f, sizeof(double)) == 0;
	return a->slen == b->slen && memcmp(a->s, b->s, a->slen) == 0;
}

static void
leaf_add(SiftF2LeafDiff *out, int c, const SiftCell *cell)
{
	if (out->nchanged == SIFT_F2_DIFF_MAX_LEAVES)
		out->rejected = true;
	else
	{
		out->leaf[out->nchanged] = (int16) c;
		out->cell[out->nchanged++] = *cell;
	}
}

/* The elements of an array leaf: any difference is the whole array changed, which a patch of the row's cells cannot hold */
static bool
elements_same(SiftCellKind kind, const SiftCell *olde, const SiftCell *newe, int64 n)
{
	for (int64 j = 0; j < n; j++)
		if (!sift_f2_cell_same(kind, &olde[j], &newe[j]))
			return false;
	return true;
}

void
sift_f2_leaf_diff(const SiftCellKind *kinds, const int16 *parent, const bool *want, int ncols, const SiftCell *oldc, SiftCell *const *olde, const SiftCell *newc, SiftCell *const *newe,
				  SiftF2LeafDiff *out)
{
	bool	   *is_len = palloc0(Max(ncols, 1));

	*out = (SiftF2LeafDiff) {0};
	for (int c = 0; c < ncols; c++)
		if (parent[c] >= 0)
			is_len[parent[c]] = true;
	for (int c = 0; c < ncols && !out->rejected; c++)
	{
		if (!want[c])
			continue;
		if (parent[c] >= 0)
		{
			int			g = parent[c];
			int64		n = newc[g].isnull ? 0 : newc[g].i;

			if (!sift_f2_cell_same(kinds[g], &oldc[g], &newc[g]) || (n > 0 && !elements_same(kinds[c], olde[c], newe[c], n)))
				out->rejected = true;
			continue;
		}
		if (sift_f2_cell_same(kinds[c], &oldc[c], &newc[c]))
			continue;
		if (is_len[c] || newc[c].xp != NULL || oldc[c].xp != NULL)
			out->rejected = true;
		else
			leaf_add(out, c, &newc[c]);
	}
	pfree(is_len);
}

SiftF2Admit
sift_f2_col_delta_admit_index(const SiftF2AdmitIn *in)
{
	int			n = in->nchanged_leaves;

	if (!in->tracked)
		return SIFT_F2A_NOT_TRACKED;
	if (in->pred != SIFT_PRED_BOTH)
		return SIFT_F2A_PREDICATE;
	if (in->key_changed)
		return SIFT_F2A_KEY_CHANGED;
	if (in->rejected)
		return SIFT_F2A_REJECTED;
	if (in->chain_pages + in->staged_pages >= SIFT_COL_DELTA_MAX_PAGES)
		return SIFT_F2A_CHAIN_FULL;
	if (n == 0)
		return SIFT_F2A_OK;
	if (n > SIFT_COL_DELTA_MAX_COLS_PER_REC)
		return SIFT_F2A_TOO_MANY_LEAVES;
	if (!sift_col_delta_cols_fit(n, in->total_leaves))
		return SIFT_F2A_TOO_WIDE;
	if (in->payload_bytes > SIFT_COL_DELTA_MAX_PAYLOAD)
		return SIFT_F2A_PAYLOAD;
	return SIFT_F2A_OK;
}

#ifndef SIFT_S49_STANDALONE
#include "access/htup_details.h"
#include "utils/rel.h"

/* The values of the entry the version of the row has in the index; held: the version is in the index (a partial index's predicate) */
static void
entry_values(const SiftF2DiffEnv *env, HeapTuple t, Datum *v, bool *isnull, bool *held)
{
	const FormData_pg_index *x = env->index->rd_index;
	ExprContext *ec;

	*held = true;
	if (env->direct)
	{
		for (int a = 0; a < x->indnatts; a++)
			v[a] = heap_getattr(t, x->indkey.values[a], RelationGetDescr(env->heap), &isnull[a]);
		return;
	}
	ec = GetPerTupleExprContext(env->estate);
	ec->ecxt_scantuple = ExecStoreHeapTuple(t, env->slot, false);
	*held = env->pred == NULL || ExecQual(env->pred, ec);
	if (*held)
		FormIndexDatum(env->info, env->slot, env->estate, v, isnull);
}

static bool
key_attribute_changed(const SiftF2DiffEnv *env, uint64 changed)
{
	const SiftColRel *r = sift_col_rel(env->index);	/* check-memory: allow nothing below reads the catalog */

	for (int k = 0; k < env->f->nkey; k++)
		if ((changed >> (r->schema->cols[env->f->key_cols[k]].attno - 1)) & 1)
			return true;
	return false;
}

static void
keep_string(MemoryContext cx, SiftCell *c)
{
	if (c->s != NULL && !c->isnull && c->xp == NULL)
		c->s = memcpy(MemoryContextAlloc(cx, Max(c->slen, 1)), c->s, c->slen);
}

/* The strings of a path leaf are the shredder's, which it reuses for the next row: the cells of a version keep their own */
static void
keep_strings(const SiftF2Rel *f, MemoryContext cx, SiftCell *cells, SiftCell **elem)
{
	for (int c = 0; c < f->ncols; c++)
	{
		const int	g = f->parent[c];
		const int64 n = g < 0 || cells[g].isnull ? 0 : cells[g].i;

		if (f->kinds[c] != SIFT_CELL_STR)
			continue;
		if (g < 0)
			keep_string(cx, &cells[c]);
		for (int64 j = 0; j < n; j++)
			keep_string(cx, &elem[c][j]);
	}
}

/* The cells of the version with only the attributes of changed shredded: the others are left NULL, never read */
static void
shred_changed(const SiftF2DiffEnv *env, MemoryContext rowcxt, HeapTuple t, const Datum *v, const bool *isnull, uint64 changed, SiftCell *cells, SiftCell **elem)
{
	int			natts = env->index->rd_index->indnatts;
	bool		masked[INDEX_MAX_KEYS];
	uint8		key[SIFT_BOUND_BYTES];

	for (int a = 0; a < natts; a++)
		masked[a] = isnull[a] || !((changed >> a) & 1);
	sift_f2_row(env->index, env->f, rowcxt, &t->t_self, v, masked, cells, elem, key);
	keep_strings(env->f, rowcxt, cells, elem);
}

static void
own_strings(const SiftF2DiffEnv *env, SiftF2LeafDiff *d)
{
	for (int i = 0; i < d->nchanged; i++)
	{
		SiftCell   *c = &d->cell[i];

		if (!c->isnull && env->f->kinds[d->leaf[i]] == SIFT_CELL_STR)
			c->s = memcpy(MemoryContextAlloc(env->cx, Max(c->slen, 1)), c->s, c->slen);
	}
}

static void
diff_update(const SiftF2DiffEnv *env, HeapTuple oldt, HeapTuple newt, SiftF2DiffResult *res)
{
	Relation	index = env->index;
	int			natts = index->rd_index->indnatts;
	TupleDesc	idesc = RelationGetDescr(index);
	SiftF2AttDesc atts[INDEX_MAX_KEYS];
	Datum		ov[INDEX_MAX_KEYS],
				nv[INDEX_MAX_KEYS];
	bool		on[INDEX_MAX_KEYS],
				nn[INDEX_MAX_KEYS],
				old_in,
				new_in;
	uint64		changed;
	MemoryContext rowcxt = sift_col_rel_rowcxt(sift_col_rel(index));
	SiftCell   *oc,
			   *nc,
			  **oe,
			  **ne;
	bool	   *want;
	const SiftColSchema *schema;

	*res = (SiftF2DiffResult) {0};
	entry_values(env, oldt, ov, on, &old_in);
	entry_values(env, newt, nv, nn, &new_in);
	res->pred = sift_f2_pred_move(old_in, new_in);
	if (res->pred != SIFT_PRED_BOTH)
		return;
	for (int a = 0; a < natts; a++)
		atts[a] = (SiftF2AttDesc) {TupleDescAttr(idesc, a)->attlen, TupleDescAttr(idesc, a)->attbyval};
	changed = sift_f2_att_diff(atts, natts, ov, on, nv, nn);
	res->nchanged_atts = pg_popcount64(changed);
	res->key_changed = key_attribute_changed(env, changed);
	if (res->key_changed || changed == 0)
		return;
	oc = MemoryContextAllocZero(rowcxt, env->f->ncols * sizeof(SiftCell));
	nc = MemoryContextAllocZero(rowcxt, env->f->ncols * sizeof(SiftCell));
	oe = MemoryContextAllocZero(rowcxt, env->f->ncols * sizeof(SiftCell *));
	ne = MemoryContextAllocZero(rowcxt, env->f->ncols * sizeof(SiftCell *));
	want = MemoryContextAllocZero(rowcxt, env->f->ncols * sizeof(bool));
	shred_changed(env, rowcxt, oldt, ov, on, changed, oc, oe);
	shred_changed(env, rowcxt, newt, nv, nn, changed, nc, ne);
	schema = sift_col_rel(index)->schema;
	for (int c = 0; c < env->f->ncols; c++)
		want[c] = (changed >> (schema->cols[c].attno - 1)) & 1;
	sift_f2_leaf_diff(env->f->kinds, env->f->parent, want, env->f->ncols, oc, oe, nc, ne, &res->leaves);
	own_strings(env, &res->leaves);
	MemoryContextReset(rowcxt);
}

void
sift_f2_diff_update(const SiftF2DiffEnv *env, HeapTuple oldt, HeapTuple newt, SiftF2DiffResult *res)
{
	diff_update(env, oldt, newt, res);
	if (!env->direct)
		ResetExprContext(GetPerTupleExprContext(env->estate));
}
#endif
