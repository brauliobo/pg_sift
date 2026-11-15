/*
 * col/batch_unit.c - L1 of the column engine: the decoding of a unit into a batch (all its rows, or the selected ones), a leaf at a time.
 */
/* check-memory: allow-allocs every array here follows the rows of one unit (at most SIFT_COL_UNIT_MAX_ROWS, 74,496) or of one chunk, not the rows of the table */
#include "core/host.h"

#include "core/limits.h"
#include "col/enc.h"
#include "col/enc_int.h"

typedef struct UnitLeaf
{
	bool		present;
	SiftCChunk	c;
	int64	   *key;			/* decoded, NULL slots 0 */
	uint32	   *remap;			/* STR: code of the unit's dictionary -> code in the destination, PG_UINT32_MAX = not yet */
	uint32	   *eoff;			/* GROUP: element offset of each row, nrows + 1 */
	uint32	   *voff;			/* VAR and inline STR: start of each string, n + 1 */
	char	   *vb;
	uint8	   *tag;			/* the chunk's tags per slot (overflow types, kinds); NULL without SIFT_COLC_TAGS and SIFT_COLC_KINDS (minor 3: not known) */
	bool		inl;			/* STR stored inline: the strings are interned into the destination's dictionary */
} UnitLeaf;

/* Appends slot `slot` of leaf col of the unit to dst's leaf */
static void
unit_emit(SiftColBatch *dst, SiftCDict *const *dicts, UnitLeaf *lf, int col, uint32 slot)
{
	bool		isnull = !lf->present || sift_col_bit_test(lf->c.nulls, slot);
	bool		ovf = lf->present && sift_col_bit_test(lf->c.ovf, slot);
	uint8		tag = lf->present && lf->tag ? lf->tag[slot] : SIFT_COLTAG_UNKNOWN;
	int64		key = isnull ? 0 : lf->key[slot];

	if (lf->inl || lf->voff != NULL)
	{
		if (!isnull && !leaf_fits(dst, col, lf->vb + lf->voff[slot], (uint32) key))
			isnull = ovf = true, tag = SIFT_COLTAG_UNKNOWN;
		if (lf->inl)
			vals_put(dst->mcxt, &dst->col[col], isnull ? 0 : sift_cdict_intern(dst->dict[col], lf->vb + lf->voff[slot], (uint32) key), isnull, ovf, tag);
		else
			vals_put_var(dst->mcxt, &dst->col[col], &dst->schema->cols[col], lf->vb + lf->voff[slot], (uint32) key, isnull, ovf, tag);
		return;
	}
	if (!isnull && lf->remap != NULL)
	{
		uint32	   *to;

		if (key < 0 || key >= dicts[col]->n)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
							errmsg("invalid column unit: dictionary code %lld is outside the dictionary of %u values", (long long) key, dicts[col]->n)));
		to = &lf->remap[key];
		if (*to == PG_UINT32_MAX)
		{
			const char *str = dicts[col]->bytes + dicts[col]->off[key];
			uint32		len = cdict_len(dicts[col], (uint32) key);

			if (!leaf_fits(dst, col, str, len))
			{
				vals_put(dst->mcxt, &dst->col[col], 0, true, true, SIFT_COLTAG_UNKNOWN);
				return;
			}
			*to = sift_cdict_intern(dst->dict[col], str, len);
		}
		key = *to;
	}
	vals_put(dst->mcxt, &dst->col[col], key, isnull, ovf, tag);
}

/* do the leaves read have no group, and no string that is stored inline or in a dictionary of its own: a leaf of those is decoded a column at a time (unit_emit_columns) */
static bool
unit_columns_plain(const SiftColSchema *s, const UnitLeaf *lf, const bool *want)
{
	for (int col = 0; col < s->ncols; col++)
		if (!(want != NULL && !want[col]) && (s->cols[col].parent >= 0 || s->cols[col].kind == SIFT_COLK_GROUP || lf[col].inl || lf[col].voff != NULL))
			return false;
	return true;
}

/* The m rows (ascending) of a unit appended to dst a leaf at a time: what the loop over the rows of unit_decode does, with the per-value work of vals_put left to the leaves that have a NULL, an overflow or a tag */
static void
unit_emit_columns(SiftColBatch *dst, SiftCDict *const *dicts, UnitLeaf *lf, const SiftKey *keys, const uint32 *rows, uint32 m, const bool *want)
{
	const SiftColSchema *s = dst->schema;

	batch_reserve_rows(dst, m);
	for (uint32 i = 0; i < m; i++)
		dst->keys[dst->nrows + i] = keys[rows[i]];
	batch_check_keys(dst, m, true);
	for (int col = 0; col < s->ncols; col++)
	{
		UnitLeaf   *l = &lf[col];
		SiftCVals  *v = &dst->col[col];

		if (want != NULL && !want[col])
			continue;
		if (l->present && l->remap == NULL)
		{
			vals_reserve(dst->mcxt, v, m);
			for (uint32 i = 0; i < m; i++)
				v->key[v->n + i] = l->key[rows[i]];
			for (uint32 i = 0; l->c.nulls != NULL && i < m; i++)
				if (sift_col_bit_test(l->c.nulls, rows[i]))
					bitmap_set(dst->mcxt, &v->nulls, v->cap, v->n + i);
			for (uint32 i = 0; l->c.ovf != NULL && i < m; i++)
				if (sift_col_bit_test(l->c.ovf, rows[i]))
					bitmap_set(dst->mcxt, &v->ovf, v->cap, v->n + i);
			for (uint32 i = 0; l->tag != NULL && i < m; i++)
				if (l->tag[rows[i]] != SIFT_COLTAG_UNKNOWN)
				{
					if (v->otag == NULL)
						v->otag = MemoryContextAllocZero(dst->mcxt, v->cap);
					v->otag[v->n + i] = l->tag[rows[i]];
				}
			v->n += m;
		}
		else
			for (uint32 i = 0; i < m; i++)
				unit_emit(dst, dicts, l, col, rows[i]);
	}
	dst->nrows += m;
}

/*
 * rows (NULL: every row that live keeps): the m rows (ascending) to decode, of which only the values of the plain leaves that are read at those positions are decoded: the others are decoded
 * whole, as is the address of every row of a unit cut by key
 */
static void
unit_decode(const SiftCUnitView *v, uint32 sb, SiftCDict *const *dicts, const uint64 *live, const uint32 *rows, uint32 m, const bool *want, SiftColBatch *dst)
{
	const SiftColSchema *s = v->schema;
	SiftKey    *keys = palloc(Max(v->nrows, 1) * sizeof(SiftKey));
	UnitLeaf   *lf = palloc0(s->ncols * sizeof(UnitLeaf));

	SIFT_INVARIANT(dst->schema->ncols == s->ncols && dst->sb == sb, "sift_col: a unit is decoded into a batch of its own schema and superblock");
	if (rows != NULL)
	{
		SiftKey    *some = palloc((m + 1) * sizeof(SiftKey));

		sift_cunit_row_keys(v, sb, rows, m, some);
		for (uint32 j = 0; j < m; j++)
			keys[rows[j]] = some[j];
		pfree(some);
	}
	else
		sift_cunit_keys(v, sb, keys);
	for (int col = 0; col < s->ncols; col++)
	{
		UnitLeaf   *l = &lf[col];

		if (want != NULL && !want[col])
			continue;
		l->present = sift_cunit_chunk(v, col, &l->c);
		if (!l->present)
			continue;
		l->key = palloc((l->c.n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
		l->inl = l->c.enc == SIFT_COLENC_ISTR;
		if (rows != NULL && s->cols[col].kind == SIFT_COLK_SCALAR && s->cols[col].parent < 0 && s->cols[col].store != SIFT_COLS_VAR && !l->inl)
		{
			int64	   *some = palloc((m + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));

			sift_cchunk_decode_sel(&l->c, rows, m, some);
			for (uint32 j = 0; j < m; j++)
				l->key[rows[j]] = some[j];
			pfree(some);
		}
		else
			sift_cchunk_decode(&l->c, l->key);
		l->tag = palloc(Max(l->c.n, 1));
		if (!sift_cchunk_tags(&l->c, l->tag))
		{
			pfree(l->tag);
			l->tag = NULL;
		}
		if (s->cols[col].store == SIFT_COLS_VAR || l->inl)
			sift_cchunk_strs(&l->c, CurrentMemoryContext, &l->voff, &l->vb);
		if (leaf_is_str(&s->cols[col]) && !l->inl)
		{
			l->remap = palloc(Max(dicts[col]->n, 1) * sizeof(uint32));
			memset(l->remap, 0xFF, Max(dicts[col]->n, 1) * sizeof(uint32));
		}
		if (s->cols[col].kind == SIFT_COLK_GROUP)
		{
			l->eoff = palloc((v->nrows + 1) * sizeof(uint32));
			l->eoff[0] = 0;
			for (uint32 r = 0; r < v->nrows; r++)
			{
				if ((uint64) l->key[r] > PG_UINT32_MAX || (uint64) l->eoff[r] + (uint64) l->key[r] > PG_UINT32_MAX)
					ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
									errmsg("invalid column unit: the group counts of leaf %d add up to more than %u elements", col, PG_UINT32_MAX)));
				l->eoff[r + 1] = l->eoff[r] + (uint32) l->key[r];
			}
		}
	}
	for (int col = 0; col < s->ncols; col++)
		if (s->cols[col].parent >= 0 && lf[col].present && lf[col].c.n != (lf[s->cols[col].parent].eoff ? lf[s->cols[col].parent].eoff[v->nrows] : 0))
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
							errmsg("invalid column unit: group leaf %d has %u values for %u elements", col, lf[col].c.n,
								   lf[s->cols[col].parent].eoff ? lf[s->cols[col].parent].eoff[v->nrows] : 0)));

	if (rows != NULL && unit_columns_plain(s, lf, want))
	{
		unit_emit_columns(dst, dicts, lf, keys, rows, m, want);
		goto done;
	}
	for (uint32 i = 0; i < (rows != NULL ? m : v->nrows); i++)
	{
		uint32		r = rows != NULL ? rows[i] : i;

		if (rows == NULL && live != NULL && !sift_col_bit_test(live, r))
			continue;
		batch_row_begin(dst, keys[r]);
		for (int col = 0; col < s->ncols; col++)
		{
			if (s->cols[col].parent >= 0 || (want != NULL && !want[col]))
				continue;
			unit_emit(dst, dicts, &lf[col], col, r);
			if (s->cols[col].kind != SIFT_COLK_GROUP)
				continue;
			batch_group_end(dst, col, lf[col].present ? lf[col].eoff[r + 1] - lf[col].eoff[r] : 0);
			for (int ch = col + 1; ch < s->ncols; ch++)
				if (s->cols[ch].parent == col && lf[col].present)
					for (uint32 j = lf[col].eoff[r]; j < lf[col].eoff[r + 1]; j++)
						unit_emit(dst, dicts, &lf[ch], ch, j);
		}
		dst->nrows++;
	}
done:
	for (int col = 0; col < s->ncols; col++)
	{
		if (lf[col].key)
			pfree(lf[col].key);
		if (lf[col].remap)
			pfree(lf[col].remap);
		if (lf[col].eoff)
			pfree(lf[col].eoff);
		if (lf[col].tag)
			pfree(lf[col].tag);
		if (lf[col].voff)
		{
			pfree(lf[col].voff);
			pfree(lf[col].vb);
		}
	}
	pfree(lf);
	pfree(keys);
}

void
sift_cunit_decode(const SiftCUnitView *v, uint32 sb, SiftCDict *const *dicts, const uint64 *live, const bool *want, SiftColBatch *dst)
{
	unit_decode(v, sb, dicts, live, NULL, 0, want, dst);
}

void
sift_cunit_decode_rows(const SiftCUnitView *v, uint32 sb, SiftCDict *const *dicts, const uint32 *rows, uint32 m, const bool *want, SiftColBatch *dst)
{
	unit_decode(v, sb, dicts, NULL, rows, m, want, dst);
}
