/*
 * col/batch.c - L1 of the column engine: batches, the decoded rows of one superblock, and the decoding of a unit into one.
 */
#include "core/host.h"

#include "core/limits.h"
#include "col/enc.h"
#include "col/enc_int.h"

/* ---------------------------------------------------------------------------
 * 3. Batches
 * ------------------------------------------------------------------------- */

SiftColBatch *
sift_cbatch_create(MemoryContext mcxt, const SiftColSchema *schema)
{
	SiftColBatch *b = MemoryContextAllocZero(mcxt, sizeof(SiftColBatch));

	b->mcxt = mcxt;
	b->schema = schema;
	b->rowcap = 256;
	b->keys = MemoryContextAlloc(mcxt, b->rowcap * sizeof(SiftKey));
	b->col = MemoryContextAllocZero(mcxt, schema->ncols * sizeof(SiftCVals));
	b->dict = MemoryContextAllocZero(mcxt, schema->ncols * sizeof(SiftCDict *));
	for (int i = 0; i < schema->ncols; i++)
	{
		if (leaf_is_str(&schema->cols[i]))
			b->dict[i] = sift_cdict_create(mcxt);
		if (schema->cols[i].kind == SIFT_COLK_GROUP)
			b->col[i].eoff = MemoryContextAllocZero(mcxt, (b->rowcap + 1) * sizeof(uint32));
	}
	return b;
}

void
sift_cbatch_reset(SiftColBatch *b, uint32 sb)
{
	b->sb = sb;
	b->nrows = 0;
	b->keyed = false;
	b->hasver = false;
	for (int i = 0; i < b->schema->ncols; i++)
	{
		SiftCVals  *v = &b->col[i];

		if (v->nulls)
			memset(v->nulls, 0, words_of(v->n) * sizeof(uint64));
		if (v->ovf)
			memset(v->ovf, 0, words_of(v->n) * sizeof(uint64));
		if (v->otag)
			memset(v->otag, 0, v->n);
		v->n = 0;
		v->noted = false;
		if (v->eoff)
			v->eoff[0] = 0;
		if (v->voff)
			v->voff[0] = 0;
		if (b->dict[i])
			cdict_reset(b->dict[i]);
	}
}

static void
bitmap_set(MemoryContext mcxt, uint64 **bits, uint32 cap, uint32 i)
{
	if (*bits == NULL)
		*bits = MemoryContextAllocZero(mcxt, words_of(cap) * sizeof(uint64));
	(*bits)[i >> 6] |= UINT64CONST(1) << (i & 63);
}

/* Appends one slot of a leaf: the key (0 for a NULL), whether it is NULL, whether it overflowed (then also NULL) and its tag: the JSON type of an overflow value (0: not known), else its kind (0: the leaf's own) */
static void
vals_put(MemoryContext mcxt, SiftCVals *v, int64 key, bool isnull, bool ovf, uint8 tag)
{
	if (v->n == v->cap)
	{
		uint32		cap = Max(64, v->cap * 2);

		v->key = v->key ? repalloc(v->key, cap * sizeof(int64)) : MemoryContextAlloc(mcxt, cap * sizeof(int64));
		if (v->nulls)
		{
			v->nulls = repalloc(v->nulls, words_of(cap) * sizeof(uint64));
			memset(v->nulls + words_of(v->cap), 0, (words_of(cap) - words_of(v->cap)) * sizeof(uint64));
		}
		if (v->ovf)
		{
			v->ovf = repalloc(v->ovf, words_of(cap) * sizeof(uint64));
			memset(v->ovf + words_of(v->cap), 0, (words_of(cap) - words_of(v->cap)) * sizeof(uint64));
		}
		if (v->otag)
		{
			v->otag = repalloc(v->otag, cap);
			memset(v->otag + v->cap, 0, cap - v->cap);
		}
		v->cap = cap;
	}
	v->key[v->n] = isnull ? 0 : key;
	if (isnull)
		bitmap_set(mcxt, &v->nulls, v->cap, v->n);
	if (ovf)
		bitmap_set(mcxt, &v->ovf, v->cap, v->n);
	if (tag != SIFT_COLTAG_UNKNOWN)
	{
		if (v->otag == NULL)
			v->otag = MemoryContextAllocZero(mcxt, v->cap);
		v->otag[v->n] = tag;
	}
	v->n++;
}

/* Appends one slot of a VAR leaf: the bytes of the value (none for a NULL) */
static void
vals_put_var(MemoryContext mcxt, SiftCVals *v, const char *s, uint32 len, bool isnull, bool ovf, uint8 tag)
{
	uint32		start = v->n == 0 ? 0 : v->voff[v->n];

	if (isnull)
		len = 0;
	if ((uint64) start + len > (uint64) PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift_col: the variable-length values of a unit exceed 2 GB"),
				 errhint("Leave the column out of the index, or index left(column, N) or a hash of it.")));
	if (v->voff == NULL)
	{
		v->voff = MemoryContextAlloc(mcxt, sizeof(uint32));
		v->voff[0] = 0;
	}
	if (v->n == v->cap)
		v->voff = repalloc(v->voff, ((Size) Max(64, v->cap * 2) + 1) * sizeof(uint32));
	vals_put(mcxt, v, len, isnull, ovf, tag);
	if (v->vb == NULL || start + len > v->vcap)
	{
		v->vcap = Max(Max(1024, v->vcap * 2), (Size) start + len);
		v->vb = v->vb ? repalloc(v->vb, v->vcap) : MemoryContextAlloc(mcxt, v->vcap);
	}
	if (len > 0)
		memcpy(v->vb + start, s, len);
	v->voff[v->n] = start + len;
}

/* Validates the key against the batch and makes room for the row; the row is counted by batch_row_end */
static void
batch_row_begin(SiftColBatch *b, SiftKey key)
{
	SIFT_INVARIANT(b->keyed || sift_block_sb(sift_key_block(key)) == b->sb, "sift_col: row %u/%u is outside superblock %u", sift_key_block(key),
				   (uint32) (key & 0xFFFF), b->sb);
	SIFT_INVARIANT((key & 0xFFFF) >= 1 && (key & 0xFFFF) <= MaxHeapTuplesPerPage, "sift_col: row offset %u is not a heap offset",
				   (uint32) (key & 0xFFFF));
	SIFT_INVARIANT(b->nrows == 0 || key > b->keys[b->nrows - 1], "sift_col: rows must be added in ascending TID order");
	if (b->nrows == b->rowcap)
	{
		b->rowcap *= 2;
		b->keys = repalloc(b->keys, b->rowcap * sizeof(SiftKey));
		if (b->xmin)
		{
			b->xmin = repalloc(b->xmin, b->rowcap * sizeof(uint32));
			b->cmin = repalloc(b->cmin, b->rowcap * sizeof(uint32));
		}
		for (int i = 0; i < b->schema->ncols; i++)
			if (b->col[i].eoff)
				b->col[i].eoff = repalloc(b->col[i].eoff, ((Size) b->rowcap + 1) * sizeof(uint32));
	}
	b->keys[b->nrows] = key;
}

void
sift_cbatch_set_ver(SiftColBatch *b, uint32 r, uint32 xmin, uint32 cmin)
{
	if (!b->hasver)
	{
		if (xmin == 0)
			return;
		if (b->xmin == NULL)
		{
			b->xmin = MemoryContextAlloc(b->mcxt, b->rowcap * sizeof(uint32));
			b->cmin = MemoryContextAlloc(b->mcxt, b->rowcap * sizeof(uint32));
		}
		memset(b->xmin, 0, b->rowcap * sizeof(uint32));
		memset(b->cmin, 0, b->rowcap * sizeof(uint32));
		b->hasver = true;
	}
	b->xmin[r] = xmin;
	b->cmin[r] = xmin ? cmin : 0;
}

static void
batch_group_end(SiftColBatch *b, int group, uint32 count)
{
	b->col[group].eoff[b->nrows + 1] = b->col[group].eoff[b->nrows] + count;
}

/*
 * Whether a string of len bytes still fits the byte budget of leaf id in this unit (a value the leaf's dictionary holds costs nothing).
 * Past it the value is stored as an overflow, as a value the leaf cannot store is, and the first one is announced.
 */
static bool
leaf_fits(SiftColBatch *b, int id, const char *s, uint32 len)
{
	SiftCVals  *v = &b->col[id];
	int64		budget = sift_limit(unit_bytes, SIFT_COL_UNIT_MAX_BYTES);
	uint64		used = b->dict[id] ? b->dict[id]->nbytes : v->n == 0 ? 0 : v->voff[v->n];

	if (used + len <= (uint64) budget || (b->dict[id] && sift_cdict_find(b->dict[id], s, len) >= 0))
		return true;
	if (!v->noted)
	{
		const SiftColDef *d = &b->schema->cols[id];

		v->noted = true;
		ereport(NOTICE,
				(errmsg("sift_col: the strings of leaf %d (attribute %d%s%s) in superblock %u pass %lld bytes: the values after them are stored as overflow and read from the heap",
						id, d->attno, d->path[0] ? " path " : "", d->path, b->sb, (long long) budget),
				 errhint("Index an expression that keeps the part of the value that is used, such as left(column, 255).")));
	}
	return false;
}

static void
put_value(SiftColBatch *b, int id, const SiftColValue *v)
{
	bool		isnull = v->isnull || v->overflow;
	int64		key = 0;
	SiftColValue over;

	if ((b->schema->cols[id].store == SIFT_COLS_VAR || b->dict[id]) && !isnull && !leaf_fits(b, id, v->v.s, v->len))
	{
		over = (SiftColValue) {.isnull = true, .overflow = true, .tag = SIFT_COLTAG_UNKNOWN};
		v = &over;
		isnull = true;
	}
	if (b->schema->cols[id].store == SIFT_COLS_VAR)
	{
		vals_put_var(b->mcxt, &b->col[id], v->v.s, v->len, isnull, v->overflow, v->tag);
		return;
	}
	if (!isnull)
		switch (b->schema->cols[id].store)
		{
			case SIFT_COLS_INT:
				key = v->v.i;
				break;
			case SIFT_COLS_STR:
				key = sift_cdict_intern(b->dict[id], v->v.s, v->len);
				break;
			default:
				key = sift_col_f8_key(v->v.f);
		}
	vals_put(b->mcxt, &b->col[id], key, isnull, v->overflow, v->tag);
}

bool
sift_cbatch_kinds_exact(const SiftColBatch *b)
{
	for (int c = 0; c < b->schema->ncols; c++)
		for (uint32 i = 0; b->col[c].otag != NULL && i < b->col[c].n; i++)
			if (b->col[c].otag[i] == SIFT_COLTAG_UNSURE)
				return false;
	return true;
}

void
sift_cbatch_add(SiftColBatch *b, const SiftColRow *row)
{
	sift_cbatch_add_ver(b, row, 0, 0);
}

void
sift_cbatch_add_ver(SiftColBatch *b, const SiftColRow *row, uint32 xmin, uint32 cmin)
{
	const SiftColSchema *s = b->schema;

	batch_row_begin(b, sift_key_from_tid((ItemPointer) &row->tid));
	for (int id = 0; id < s->ncols; id++)
	{
		const SiftColValue *v = &row->val[id];
		uint32		count = 0;

		if (s->cols[id].parent >= 0)
			continue;
		put_value(b, id, v);
		if (s->cols[id].kind != SIFT_COLK_GROUP)
			continue;
		if (!v->isnull && !v->overflow)
		{
			SIFT_INVARIANT(v->v.i >= 0 && v->v.i <= PG_INT32_MAX, "sift_col: array of %lld elements", (long long) v->v.i);
			count = (uint32) v->v.i;
		}
		batch_group_end(b, id, count);
		for (int c = id + 1; c < s->ncols; c++)
			if (s->cols[c].parent == id)
				for (uint32 j = 0; j < count; j++)
					put_value(b, c, &row->elem[c][j]);
	}
	if (b->hasver || xmin)
		sift_cbatch_set_ver(b, b->nrows, xmin, cmin);
	b->nrows++;
}

void
sift_cbatch_value(const SiftColBatch *b, int col, uint32 slot, SiftColValue *v)
{
	const SiftCVals *vals = &b->col[col];
	int64		key = vals->key[slot];

	memset(v, 0, sizeof(*v));
	v->isnull = sift_col_bit_test(vals->nulls, slot);
	v->overflow = sift_col_bit_test(vals->ovf, slot);
	v->tag = vals->otag ? vals->otag[slot] : SIFT_COLTAG_UNKNOWN;
	if (v->isnull)
		return;
	switch (b->schema->cols[col].store)
	{
		case SIFT_COLS_INT:
			v->v.i = key;
			break;
		case SIFT_COLS_STR:
			v->v.s = b->dict[col]->bytes + b->dict[col]->off[key];
			v->len = cdict_len(b->dict[col], (uint32) key);
			break;
		case SIFT_COLS_VAR:
			v->v.s = vals->vb + vals->voff[slot];
			v->len = (uint32) key;
			break;
		default:
			v->v.f = sift_col_f8_from_key(key);
	}
}

/* The elements of a row's arrays live in one block; elem[ncols] points at its values, the header holds the capacity */
typedef struct ElemBlock
{
	Size		cap;
	SiftColValue v[FLEXIBLE_ARRAY_MEMBER];
} ElemBlock;

#define ELEM_BLOCK(p)	((ElemBlock *) ((char *) (p) - offsetof(ElemBlock, v)))

void
sift_cbatch_row(const SiftColBatch *b, uint32 r, SiftColRow *row)
{
	const SiftColSchema *s = b->schema;
	Size		total = 0;
	SiftColValue *next;

	if (row->val == NULL)
	{
		row->val = MemoryContextAllocZero(b->mcxt, s->ncols * sizeof(SiftColValue));
		row->elem = MemoryContextAllocZero(b->mcxt, (s->ncols + 1) * sizeof(SiftColValue *));
	}
	row->ncols = s->ncols;
	sift_key_to_tid(b->keys[r], &row->tid);
	for (int id = 0; id < s->ncols; id++)
		if (s->cols[id].kind == SIFT_COLK_GROUP)
			for (int c = id + 1; c < s->ncols; c++)
				if (s->cols[c].parent == id)
					total += b->col[id].eoff[r + 1] - b->col[id].eoff[r];

	if (total > 0 && (row->elem[s->ncols] == NULL || ELEM_BLOCK(row->elem[s->ncols])->cap < total))
	{
		ElemBlock  *blk;

		if (row->elem[s->ncols])
			pfree(ELEM_BLOCK(row->elem[s->ncols]));
		blk = MemoryContextAlloc(b->mcxt, offsetof(ElemBlock, v) + Max(total, 16) * sizeof(SiftColValue));
		blk->cap = Max(total, 16);
		row->elem[s->ncols] = blk->v;
	}
	next = row->elem[s->ncols];
	memset(row->elem, 0, s->ncols * sizeof(SiftColValue *));
	for (int id = 0; id < s->ncols; id++)
	{
		const SiftColDef *d = &s->cols[id];

		if (d->parent >= 0)
		{
			memset(&row->val[id], 0, sizeof(SiftColValue));
			row->val[id].isnull = true;
			continue;
		}
		sift_cbatch_value(b, id, r, &row->val[id]);
		if (d->kind != SIFT_COLK_GROUP)
			continue;
		for (int c = id + 1; c < s->ncols; c++)
			if (s->cols[c].parent == id)
			{
				uint32		first = b->col[id].eoff[r],
							count = b->col[id].eoff[r + 1] - first;

				row->elem[c] = next;
				for (uint32 j = 0; j < count; j++)
					sift_cbatch_value(b, c, first + j, next++);
			}
	}
}

void
sift_cbatch_row_free(SiftColRow *row)
{
	if (row->val == NULL)
		return;
	if (row->elem[row->ncols])
		pfree(ELEM_BLOCK(row->elem[row->ncols]));
	pfree(row->elem);
	pfree(row->val);
	memset(row, 0, sizeof(*row));
}

static void
batch_copy_row(SiftColBatch *dst, const SiftColBatch *src, uint32 r, SiftColRow *scratch)
{
	sift_cbatch_row(src, r, scratch);
	sift_cbatch_add_ver(dst, scratch, src->hasver ? src->xmin[r] : 0, src->hasver ? src->cmin[r] : 0);
}

void
sift_cbatch_add_from(SiftColBatch *dst, const SiftColBatch *src, uint32 r)
{
	SiftColRow	row = {0};

	batch_copy_row(dst, src, r, &row);
	sift_cbatch_row_free(&row);
}

void
sift_cbatch_merge(SiftColBatch *dst, SiftColBatch *const *src, int nsrc)
{
	uint32	   *pos = palloc0(nsrc * sizeof(uint32));
	SiftColRow	row = {0};

	for (;;)
	{
		int			first = -1;
		SiftKey		min = 0;

		for (int i = 0; i < nsrc; i++)
			if (pos[i] < src[i]->nrows && (first < 0 || src[i]->keys[pos[i]] < min))
			{
				first = i;
				min = src[i]->keys[pos[i]];
			}
		if (first < 0)
			break;
		batch_copy_row(dst, src[first], pos[first], &row);
		for (int i = 0; i < nsrc; i++)
			if (pos[i] < src[i]->nrows && src[i]->keys[pos[i]] == min)
				pos[i]++;
	}
	sift_cbatch_row_free(&row);
	pfree(pos);
}

/* ---- decoding a unit into a batch ---- */

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
			vals_put_var(dst->mcxt, &dst->col[col], lf->vb + lf->voff[slot], (uint32) key, isnull, ovf, tag);
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

void
sift_cunit_decode(const SiftCUnitView *v, uint32 sb, SiftCDict *const *dicts, const uint64 *live, SiftColBatch *dst)
{
	const SiftColSchema *s = v->schema;
	SiftKey    *keys = palloc(Max(v->nrows, 1) * sizeof(SiftKey));
	UnitLeaf   *lf = palloc0(s->ncols * sizeof(UnitLeaf));

	SIFT_INVARIANT(dst->schema->ncols == s->ncols && dst->sb == sb, "sift_col: a unit is decoded into a batch of its own schema and superblock");
	sift_cunit_keys(v, sb, keys);
	for (int col = 0; col < s->ncols; col++)
	{
		UnitLeaf   *l = &lf[col];

		l->present = sift_cunit_chunk(v, col, &l->c);
		if (!l->present)
			continue;
		l->key = palloc((l->c.n + SIFT_COL_PAD / sizeof(int64)) * sizeof(int64));
		sift_cchunk_decode(&l->c, l->key);
		l->inl = l->c.enc == SIFT_COLENC_ISTR;
		l->tag = palloc(Max(l->c.n, 1));
		if (!sift_cchunk_tags(&l->c, l->tag))
			l->tag = NULL;
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

	for (uint32 r = 0; r < v->nrows; r++)
	{
		if (live != NULL && !sift_col_bit_test(live, r))
			continue;
		batch_row_begin(dst, keys[r]);
		for (int col = 0; col < s->ncols; col++)
		{
			if (s->cols[col].parent >= 0)
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
