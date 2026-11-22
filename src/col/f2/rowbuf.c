/*
 * col/rowbuf.c - rows in memory, columnar (col/rowbuf.h).
 *
 * A column of the rows has a value for each row. A column of elements (SiftPartSpec.parent) has one for each element of the arrays of the rows, in the order of the rows: the column of the
 * lengths of those arrays keeps where the elements of each row start (estart), so a row is its cells and the elements estart[r] .. estart[r + 1] - 1 of its group.
 */
#include "core/memtrack.h"
#include "core/host.h"

#include "col/f2/rowbuf.h"
#include "core/range.h"

/* What one value of a column of this kind holds in the arrays of the buffer: its null flag, tag and the value, or the address and length of the string (its bytes are counted when it is added) */
static uint32
value_slot_bytes(SiftCellKind kind)
{
	return 2 + (kind == SIFT_CELL_INT || kind == SIFT_CELL_F8 ? 8 : 12);
}

/* The columns of elements of each group, in one counting pass: a row is then walked as its groups and the children of each, not as every column for every group */
static void
build_groups(SiftRowBuf *rb)
{
	int			fill = 0;

	rb->groups = MemoryContextAlloc(rb->cx, rb->ncols * sizeof(int16));
	rb->child_off = MemoryContextAllocZero(rb->cx, (rb->ncols + 1) * sizeof(int16));
	rb->children = MemoryContextAlloc(rb->cx, rb->ncols * sizeof(int16));
	for (int g = 0; g < rb->ncols; g++)
		if (sift_rowbuf_is_group(rb, g))
			rb->groups[rb->ngroups++] = g;
	for (int gi = 0; gi < rb->ngroups; gi++)
	{
		rb->child_off[gi] = fill;
		for (int c = 0; c < rb->ncols; c++)
			if (rb->parent[c] == rb->groups[gi])
				rb->children[fill++] = c;
	}
	rb->child_off[rb->ngroups] = fill;
}

SiftRowBuf *
sift_rowbuf_new(const SiftPartSpec *spec)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift row buffer");
	SiftRowBuf *rb = MemoryContextAllocZero(cx, sizeof(SiftRowBuf));
	int			ncols = spec->ncols;

	rb->cx = cx;
	sift_str_arena_init(&rb->strs, cx);
	rb->ncols = ncols;
	rb->kinds = MemoryContextAlloc(cx, ncols * sizeof(SiftCellKind));
	memcpy(rb->kinds, spec->kinds, ncols * sizeof(SiftCellKind));
	rb->i = MemoryContextAllocZero(cx, ncols * sizeof(int64 *));
	rb->f = MemoryContextAllocZero(cx, ncols * sizeof(double *));
	rb->s = MemoryContextAllocZero(cx, ncols * sizeof(char **));
	rb->slen = MemoryContextAllocZero(cx, ncols * sizeof(uint32 *));
	rb->isnull = MemoryContextAllocZero(cx, ncols * sizeof(bool *));
	rb->tag = MemoryContextAllocZero(cx, ncols * sizeof(uint8 *));
	rb->xpay = MemoryContextAllocZero(cx, ncols * sizeof(char **));
	rb->xplen = MemoryContextAllocZero(cx, ncols * sizeof(uint32 *));
	if (spec->parent != NULL)
	{
		rb->parent = MemoryContextAlloc(cx, ncols * sizeof(int16));
		memcpy(rb->parent, spec->parent, ncols * sizeof(int16));
		rb->estart = MemoryContextAllocZero(cx, ncols * sizeof(uint32 *));
		rb->nelem = MemoryContextAllocZero(cx, ncols * sizeof(uint32));
		rb->ecap = MemoryContextAllocZero(cx, ncols * sizeof(uint32));
		for (int c = 0; c < ncols; c++)
			if (rb->parent[c] >= 0)
				rb->estart[rb->parent[c]] = MemoryContextAllocZero(cx, 2 * sizeof(uint32));
		build_groups(rb);
	}
	rb->row_slot_bytes = sizeof(uint64) + 3 * sizeof(uint32) + SIFT_BOUND_BYTES;
	for (int c = 0; c < ncols; c++)
		if (!sift_rowbuf_is_child(rb, c))
			rb->row_slot_bytes += value_slot_bytes(rb->kinds[c]) + (sift_rowbuf_is_group(rb, c) ? sizeof(uint32) : 0);
	return rb;
}

/* Arrays that are allocated when first needed are zero from there on */
static void
grow_zero(SiftRowBuf *rb, void **arr, size_t esz, size_t oldn, size_t newn)
{
	if (*arr == NULL)
		*arr = MemoryContextAllocZero(rb->cx, newn * esz);
	else
	{
		*arr = repalloc(*arr, newn * esz);
		memset((char *) *arr + oldn * esz, 0, (newn - oldn) * esz);
	}
}

#define GROW(arr, oldmax, newmax) \
	((arr) = (arr) == NULL ? MemoryContextAllocZero(rb->cx, (newmax) * sizeof(*(arr))) : repalloc((arr), (newmax) * sizeof(*(arr))))

/* Room for `newn` values in the arrays of column c (they held `oldn`) */
static void
grow_column(SiftRowBuf *rb, int c, size_t oldn, size_t newn)
{
	GROW(rb->isnull[c], oldn, newn);
	GROW(rb->tag[c], oldn, newn);
	if (rb->kinds[c] == SIFT_CELL_INT)
		GROW(rb->i[c], oldn, newn);
	else if (rb->kinds[c] == SIFT_CELL_F8)
		GROW(rb->f[c], oldn, newn);
	else
	{
		GROW(rb->s[c], oldn, newn);
		GROW(rb->slen[c], oldn, newn);
	}
	if (rb->xpay[c] != NULL)
	{
		grow_zero(rb, (void **) &rb->xpay[c], sizeof(char *), oldn, newn);
		grow_zero(rb, (void **) &rb->xplen[c], sizeof(uint32), oldn, newn);
	}
}

static void
reserve(SiftRowBuf *rb, int extra)
{
	int			newmax;

	if (rb->n + extra <= rb->max)
		return;
	newmax = Max(Max(64, rb->max + rb->max / 4), rb->n + extra);
	GROW(rb->tid, rb->max, newmax);
	GROW(rb->xmin, rb->max, newmax);
	GROW(rb->cmin, rb->max, newmax);
	GROW(rb->subxid, rb->max, newmax);
	GROW(rb->keys, rb->max, newmax);
	for (int c = 0; c < rb->ncols; c++)
	{
		if (sift_rowbuf_is_child(rb, c))
			continue;
		grow_column(rb, c, rb->max, newmax);
		if (sift_rowbuf_is_group(rb, c))
			rb->estart[c] = repalloc(rb->estart[c], (newmax + 1) * sizeof(uint32));
	}
	rb->mem_bytes += (uint64) (newmax - rb->max) * rb->row_slot_bytes;
	rb->max = newmax;
}

static void
reserve_elems(SiftRowBuf *rb, int gi, uint32 extra)
{
	int			g = rb->groups[gi];
	uint32		need = rb->nelem[g] + extra,
				newcap;

	if (need <= rb->ecap[g])
		return;
	newcap = Max(Max(64, rb->ecap[g] + rb->ecap[g] / 4), need);
	for (int k = rb->child_off[gi]; k < rb->child_off[gi + 1]; k++)
	{
		int			c = rb->children[k];

		grow_column(rb, c, rb->ecap[g], newcap);
		rb->mem_bytes += (uint64) (newcap - rb->ecap[g]) * value_slot_bytes(rb->kinds[c]);
	}
	rb->ecap[g] = newcap;
}

static uint32
column_cap(const SiftRowBuf *rb, int c)
{
	return sift_rowbuf_is_child(rb, c) ? rb->ecap[rb->parent[c]] : (uint32) rb->max;
}

/* Value idx of column c: a row, or an element of the arrays of a column of elements */
static void
set_cell(SiftRowBuf *rb, int idx, int c, const SiftCell *cell)
{
	rb->isnull[c][idx] = cell->isnull;
	rb->tag[c][idx] = cell->tag;
	if (rb->kinds[c] == SIFT_CELL_INT)
		rb->i[c][idx] = cell->isnull ? 0 : cell->i;
	else if (rb->kinds[c] == SIFT_CELL_F8)
		rb->f[c][idx] = cell->isnull ? 0 : cell->f;
	else
	{
		char	   *copy = NULL;

		if (!cell->isnull)
		{
			copy = sift_str_arena_copy(&rb->strs, cell->s, cell->slen);
			rb->mem_bytes += cell->slen + 1;
		}
		rb->s[c][idx] = copy;
		rb->slen[c][idx] = cell->isnull ? 0 : cell->slen;
	}
	if (cell->xplen > 0)
	{
		char	   *copy = MemoryContextAlloc(rb->cx, cell->xplen);

		if (rb->xpay[c] == NULL)
		{
			grow_zero(rb, (void **) &rb->xpay[c], sizeof(char *), 0, column_cap(rb, c));
			grow_zero(rb, (void **) &rb->xplen[c], sizeof(uint32), 0, column_cap(rb, c));
		}
		memcpy(copy, cell->xp, cell->xplen);
		rb->xpay[c][idx] = copy;
		rb->xplen[c][idx] = cell->xplen;
		rb->mem_bytes += cell->xplen;
	}
	else if (rb->xpay[c] != NULL)
	{
		rb->xpay[c][idx] = NULL;
		rb->xplen[c][idx] = 0;
	}
}

SiftCell
sift_rowbuf_cell(const SiftRowBuf *rb, int c, int idx)
{
	SiftCell	cell = {.isnull = rb->isnull[c][idx],.tag = rb->tag[c][idx]};

	if (rb->kinds[c] == SIFT_CELL_INT)
		cell.i = rb->i[c][idx];
	else if (rb->kinds[c] == SIFT_CELL_F8)
		cell.f = rb->f[c][idx];
	else
	{
		cell.s = rb->s[c][idx];
		cell.slen = rb->slen[c][idx];
	}
	if (rb->xpay[c] != NULL)
	{
		cell.xp = rb->xpay[c][idx];
		cell.xplen = rb->xplen[c][idx];
	}
	return cell;
}

void
sift_rowbuf_add(SiftRowBuf *rb, uint64 tid, uint32 xmin, uint32 cmin, SubTransactionId subxid, const uint8 *key, const SiftCell *cells, const SiftCell *const *elem)
{
	reserve(rb, 1);
	rb->tid[rb->n] = tid;
	rb->xmin[rb->n] = xmin;
	rb->cmin[rb->n] = cmin;
	rb->subxid[rb->n] = subxid;
	if (key != NULL)
		memcpy(rb->keys[rb->n], key, SIFT_BOUND_BYTES);
	else
		memset(rb->keys[rb->n], 0, SIFT_BOUND_BYTES);
	for (int c = 0; c < rb->ncols; c++)
		if (!sift_rowbuf_is_child(rb, c))
			set_cell(rb, rb->n, c, &cells[c]);
	for (int gi = 0; gi < rb->ngroups; gi++)
	{
		int			g = rb->groups[gi];
		uint32		cnt = cells[g].isnull ? 0 : (uint32) cells[g].i;

		rb->estart[g][rb->n] = rb->nelem[g];
		reserve_elems(rb, gi, cnt);
		for (int k = rb->child_off[gi]; k < rb->child_off[gi + 1]; k++)
			for (uint32 j = 0; j < cnt; j++)
				set_cell(rb, rb->nelem[g] + j, rb->children[k], &elem[rb->children[k]][j]);
		rb->nelem[g] += cnt;
		rb->estart[g][rb->n + 1] = rb->nelem[g];
	}
	rb->n++;
}

void
sift_rowbuf_copy_row(SiftRowBuf *dst, const SiftRowBuf *src, int r, uint32 xmin, uint32 cmin, SubTransactionId subxid)
{
	SiftCell   *cells = palloc0(Max(src->ncols, 1) * sizeof(SiftCell));
	SiftCell  **elem = palloc0(Max(src->ncols, 1) * sizeof(SiftCell *));

	for (int c = 0; c < src->ncols; c++)
		if (!sift_rowbuf_is_child(src, c))
			cells[c] = sift_rowbuf_cell(src, c, r);
	for (int c = 0; c < src->ncols; c++)
		if (sift_rowbuf_is_child(src, c))
		{
			int			g = src->parent[c];
			uint32		first = src->estart[g][r],
						cnt = src->estart[g][r + 1] - first;

			elem[c] = palloc(Max(cnt, 1) * sizeof(SiftCell));
			for (uint32 j = 0; j < cnt; j++)
				elem[c][j] = sift_rowbuf_cell(src, c, first + j);
		}
	sift_rowbuf_add(dst, src->tid[r], xmin, cmin, subxid, src->keys[r], cells, (const SiftCell *const *) elem);
	for (int c = 0; c < src->ncols; c++)
		if (elem[c] != NULL)
			pfree(elem[c]);
	pfree(elem);
	pfree(cells);
}

static void
pack_row_key(const SiftPartSpec *spec, const SiftCell *cells, const uint64 *tid, uint8 *key)
{
	SiftBound	b;
	bool		pushed = true;
	int			ncomp = Min(sift_spec_int_keys(spec), SIFT_BOUND_COMPS);

	if (spec->nsort == 0)
	{
		memset(key, 0, SIFT_BOUND_BYTES);
		return;
	}
	sift_bound_clear(&b);
	for (int k = 0; k < ncomp && pushed; k++)
	{
		const SiftCell *c = &cells[spec->sort_cols[k]];

		pushed = c->isnull ? sift_bound_push_key_null(&b, spec->sort_flags[k]) : sift_bound_push_key_i64(&b, c->i, spec->sort_flags[k]);
	}
	pushed = pushed && (tid == NULL || ncomp < spec->nsort || sift_bound_push_tid(&b, *tid));
	SIFT_INVARIANT(pushed, "the bound of a row holds its leading integer key columns, at most SIFT_BOUND_COMPS, and, with no more, its address");
	sift_bound_pack(&b, key);
}

void
sift_row_key(const SiftPartSpec *spec, const SiftCell *cells, uint64 tid, uint8 *key)
{
	pack_row_key(spec, cells, &tid, key);
}

void
sift_row_key_prefix(const SiftPartSpec *spec, const SiftCell *cells, uint8 *key)
{
	pack_row_key(spec, cells, NULL, key);
}

void
sift_rowbuf_add_granule(SiftRowBuf *rb, const SiftGranuleView *v, const SiftRowVer *vers, const SiftPartSpec *spec, const SiftCodeMap *code_map)
{
	sift_rowbuf_add_granule_mapped(rb, v, vers, spec, code_map, NULL);
}

/* The column c of n rows that a part written before it was added has: the cell every row gets */
static void
fill_missing(const SiftRowBuf *rb, int c, const SiftCell *mc, int n, bool *nv, uint8 *tv, int64 **iv, double **fv, SiftStrCell **sv)
{
	memset(tv, 0, Max(n, 1));
	for (int r = 0; r < n; r++)
		nv[r] = mc->isnull;
	if (rb->kinds[c] == SIFT_CELL_INT)
	{
		*iv = palloc(Max(n, 1) * sizeof(int64));
		for (int r = 0; r < n; r++)
			(*iv)[r] = mc->isnull ? 0 : mc->i;
	}
	else if (rb->kinds[c] == SIFT_CELL_F8)
	{
		*fv = palloc(Max(n, 1) * sizeof(double));
		for (int r = 0; r < n; r++)
			(*fv)[r] = mc->isnull ? 0 : mc->f;
	}
	else
	{
		*sv = palloc(Max(n, 1) * sizeof(SiftStrCell));
		for (int r = 0; r < n; r++)
			(*sv)[r] = (SiftStrCell) {.isnull = mc->isnull,.p = mc->s,.len = mc->slen};
	}
}

void
sift_rowbuf_add_granule_mapped(SiftRowBuf *rb, const SiftGranuleView *v, const SiftRowVer *vers, const SiftPartSpec *spec, const SiftCodeMap *code_map, const SiftColRemap *remap)
{
	MemoryContext tmp = sift_mem_allocset_create(CurrentMemoryContext, "sift granule rows");
	MemoryContext old = MemoryContextSwitchTo(tmp);
	int			n = v->hdr->nrows;
	uint64	   *tids = palloc(n * sizeof(uint64));
	SiftCell   *cells = palloc0(rb->ncols * sizeof(SiftCell));
	SiftCell  **elem = palloc0(rb->ncols * sizeof(SiftCell *));
	int64	  **iv = palloc0(rb->ncols * sizeof(int64 *));
	double	  **fv = palloc0(rb->ncols * sizeof(double *));
	SiftStrCell **sv = palloc0(rb->ncols * sizeof(SiftStrCell *));
	bool	  **nv = palloc(rb->ncols * sizeof(bool *));
	uint8	  **tv = palloc(rb->ncols * sizeof(uint8 *));
	uint32	  **first = palloc0(rb->ncols * sizeof(uint32 *));
	uint8		key[SIFT_BOUND_BYTES];

	sift_granule_tids(v, tids);
	SIFT_INVARIANT(remap == NULL || (rb->parent == NULL && remap->ncols == rb->ncols), "a part is remapped only for a table, whose columns are all of the rows");
	for (int c = 0; c < rb->ncols; c++)
	{
		int			pc = remap != NULL ? remap->phys[c] : c;
		int			len = pc < 0 ? n : sift_granule_chunk_len(v, pc);

		nv[c] = palloc(Max(len, 1) * sizeof(bool));
		tv[c] = palloc(Max(len, 1));
		if (pc < 0)
		{
			fill_missing(rb, c, &remap->missing[c], n, nv[c], tv[c], &iv[c], &fv[c], &sv[c]);
			continue;
		}
		sift_granule_tags(v, pc, tv[c]);
		if (rb->kinds[c] == SIFT_CELL_INT)
		{
			iv[c] = palloc(Max(len, 1) * sizeof(int64));
			if (code_map != NULL && code_map[c].to != NULL)
				sift_granule_dict_codes(v, pc, code_map[c].to, code_map[c].n, iv[c], nv[c]);
			else
				sift_granule_ints(v, pc, iv[c], nv[c]);
		}
		else if (rb->kinds[c] == SIFT_CELL_F8)
		{
			fv[c] = palloc(Max(len, 1) * sizeof(double));
			sift_granule_f8(v, pc, fv[c], nv[c]);
		}
		else
		{
			sv[c] = palloc(Max(len, 1) * sizeof(SiftStrCell));
			sift_granule_strs(v, pc, sv[c]);
		}
	}
	for (int g = 0; g < rb->ncols; g++)
		if (sift_rowbuf_is_group(rb, g))
		{
			first[g] = palloc((n + 1) * sizeof(uint32));
			first[g][0] = 0;
			for (int r = 0; r < n; r++)
				first[g][r + 1] = first[g][r] + (nv[g][r] ? 0 : (uint32) iv[g][r]);
		}
	for (int r = 0; r < n; r++)
	{
		for (int c = 0; c < rb->ncols; c++)
		{
			if (sift_rowbuf_is_child(rb, c))
			{
				uint32		from = first[rb->parent[c]][r],
							cnt = first[rb->parent[c]][r + 1] - from;

				elem[c] = palloc(Max(cnt, 1) * sizeof(SiftCell));
				for (uint32 j = 0; j < cnt; j++)
					elem[c][j] = (SiftCell) {0};
				for (uint32 j = 0; j < cnt; j++)
				{
					SiftCell   *e = &elem[c][j];
					uint32		at = from + j;

					e->tag = tv[c][at];
					if (rb->kinds[c] == SIFT_CELL_STR)
					{
						e->isnull = sv[c][at].isnull;
						e->s = sv[c][at].p;
						e->slen = sv[c][at].len;
					}
					else
					{
						e->isnull = nv[c][at];
						e->i = iv[c] ? iv[c][at] : 0;
						e->f = fv[c] ? fv[c][at] : 0;
					}
					if (e->tag != 0)
						e->xp = sift_granule_exc_payload(v, c, at, &e->xplen);
				}
				continue;
			}
			cells[c] = (SiftCell) {.tag = tv[c][r]};
			if (rb->kinds[c] == SIFT_CELL_STR)
			{
				cells[c].isnull = sv[c][r].isnull;
				cells[c].s = sv[c][r].p;
				cells[c].slen = sv[c][r].len;
			}
			else
			{
				cells[c].isnull = nv[c][r];
				cells[c].i = iv[c] ? iv[c][r] : 0;
				cells[c].f = fv[c] ? fv[c][r] : 0;
			}
			if (cells[c].tag != 0)
				cells[c].xp = sift_granule_exc_payload(v, remap != NULL ? remap->phys[c] : c, r, &cells[c].xplen);
		}
		sift_row_key(spec, cells, tids[r], key);
		MemoryContextSwitchTo(old);
		sift_rowbuf_add(rb, tids[r], vers ? vers[r].xmin : FrozenTransactionId, vers ? vers[r].cmin : 0, 0, key, cells, (const SiftCell *const *) elem);
		MemoryContextSwitchTo(tmp);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);
}

/* The arrays of a column of elements are made when its group first holds an element */
static void
release(void *p)
{
	if (p != NULL)
		pfree(p);
}

/* The values of column c (a row each, or an element) at positions src_start[k] .. + cnt[k] for k < n, one block after the other */
static void
gather_column(SiftRowBuf *rb, int c, const uint32 *src_start, const uint32 *cnt, int n, uint32 total)
{
	uint32		cap = Max(total, 1);
	bool	   *isnull = MemoryContextAllocZero(rb->cx, cap * sizeof(bool));
	uint8	   *tag = MemoryContextAllocZero(rb->cx, cap);
	void	   *vals = NULL;
	uint32	   *slen = NULL;
	const char **xpay = rb->xpay[c] ? MemoryContextAllocZero(rb->cx, cap * sizeof(char *)) : NULL;
	uint32	   *xplen = rb->xplen[c] ? MemoryContextAllocZero(rb->cx, cap * sizeof(uint32)) : NULL;
	uint32		at = 0;

	if (rb->kinds[c] == SIFT_CELL_INT)
		vals = MemoryContextAllocZero(rb->cx, cap * sizeof(int64));
	else if (rb->kinds[c] == SIFT_CELL_F8)
		vals = MemoryContextAllocZero(rb->cx, cap * sizeof(double));
	else
	{
		vals = MemoryContextAllocZero(rb->cx, cap * sizeof(char *));
		slen = MemoryContextAllocZero(rb->cx, cap * sizeof(uint32));
	}
	for (int k = 0; k < n; k++)
		for (uint32 j = 0; j < cnt[k]; j++, at++)
		{
			uint32		from = src_start[k] + j;

			isnull[at] = rb->isnull[c][from];
			tag[at] = rb->tag[c][from];
			if (rb->kinds[c] == SIFT_CELL_INT)
				((int64 *) vals)[at] = rb->i[c][from];
			else if (rb->kinds[c] == SIFT_CELL_F8)
				((double *) vals)[at] = rb->f[c][from];
			else
			{
				((const char **) vals)[at] = rb->s[c][from];
				slen[at] = rb->slen[c][from];
			}
			if (xpay != NULL)
			{
				xpay[at] = rb->xpay[c][from];
				xplen[at] = rb->xplen[c][from];
			}
		}
	release(rb->isnull[c]);
	release(rb->tag[c]);
	rb->isnull[c] = isnull;
	rb->tag[c] = tag;
	if (rb->kinds[c] == SIFT_CELL_INT)
	{
		release(rb->i[c]);
		rb->i[c] = vals;
	}
	else if (rb->kinds[c] == SIFT_CELL_F8)
	{
		release(rb->f[c]);
		rb->f[c] = vals;
	}
	else
	{
		release(rb->s[c]);
		release(rb->slen[c]);
		rb->s[c] = vals;
		rb->slen[c] = slen;
	}
	if (xpay != NULL)
	{
		release(rb->xpay[c]);
		release(rb->xplen[c]);
		rb->xpay[c] = xpay;
		rb->xplen[c] = xplen;
	}
}

/* The rows are in the order perm[0..n): the elements of each group follow them (before the rows' own arrays are permuted, which the starts are read from) */
static void
reorder_elements(SiftRowBuf *rb, const int *perm)
{
	for (int gi = 0; gi < rb->ngroups; gi++)
	{
		int			g = rb->groups[gi];
		uint32	   *src = palloc(Max(rb->n, 1) * sizeof(uint32)),
				   *cnt = palloc(Max(rb->n, 1) * sizeof(uint32)),
				   *start = rb->estart[g];
		uint32		total = 0;

		for (int k = 0; k < rb->n; k++)
		{
			src[k] = start[perm[k]];
			cnt[k] = start[perm[k] + 1] - start[perm[k]];
		}
		for (int k = rb->child_off[gi]; k < rb->child_off[gi + 1]; k++)
			gather_column(rb, rb->children[k], src, cnt, rb->n, rb->nelem[g]);
		for (int k = 0; k < rb->n; k++)
		{
			start[k] = total;
			total += cnt[k];
		}
		start[rb->n] = total;
		rb->ecap[g] = Max(rb->nelem[g], 1);
		pfree(src);
		pfree(cnt);
	}
}

typedef struct SubMove
{
	SubTransactionId sub;
	SubTransactionId to;
} SubMove;

/* An aborted subtransaction's rows go; those of a released one are its parent's from now on */
static bool
sub_drops(SiftRowBuf *rb, int r, void *arg)
{
	const SubMove *m = arg;

	if (rb->subxid[r] != m->sub)
		return false;
	if (m->to == 0)
		return true;
	rb->subxid[r] = m->to;
	return false;
}

int
sift_rowbuf_drop_sub(SiftRowBuf *rb, SubTransactionId sub, SubTransactionId to)
{
	SubMove		m = {sub, to};

	return sift_rowbuf_drop_if(rb, sub_drops, &m);
}

/* Row src of the arrays of the rows put in the place of row dst (the values of the elements stay where they are) */
static void
move_row(SiftRowBuf *rb, int dst, int src)
{
	rb->tid[dst] = rb->tid[src];
	rb->xmin[dst] = rb->xmin[src];
	rb->cmin[dst] = rb->cmin[src];
	rb->subxid[dst] = rb->subxid[src];
	memcpy(rb->keys[dst], rb->keys[src], SIFT_BOUND_BYTES);
	for (int c = 0; c < rb->ncols; c++)
	{
		if (sift_rowbuf_is_child(rb, c))
			continue;
		rb->isnull[c][dst] = rb->isnull[c][src];
		rb->tag[c][dst] = rb->tag[c][src];
		if (rb->kinds[c] == SIFT_CELL_INT)
			rb->i[c][dst] = rb->i[c][src];
		else if (rb->kinds[c] == SIFT_CELL_F8)
			rb->f[c][dst] = rb->f[c][src];
		else
		{
			rb->s[c][dst] = rb->s[c][src];
			rb->slen[c][dst] = rb->slen[c][src];
		}
		if (rb->xpay[c] != NULL)
		{
			rb->xpay[c][dst] = rb->xpay[c][src];
			rb->xplen[c][dst] = rb->xplen[c][src];
		}
	}
}

int
sift_rowbuf_drop_if(SiftRowBuf *rb, SiftRowDrop drop, void *arg)
{
	int			keep = 0,
				dropped = 0;
	uint32	   *total = rb->estart ? palloc0(rb->ncols * sizeof(uint32)) : NULL;

	for (int r = 0; r < rb->n; r++)
	{
		if (drop(rb, r, arg))
		{
			dropped++;
			continue;
		}
		for (int gi = 0; gi < rb->ngroups; gi++)
		{
			int			g = rb->groups[gi];
			uint32		from = rb->estart[g][r],
						cnt = rb->estart[g][r + 1] - from;

			for (int k = rb->child_off[gi]; k < rb->child_off[gi + 1] && from != total[g]; k++)
			{
				int			c = rb->children[k];

				memmove(&rb->isnull[c][total[g]], &rb->isnull[c][from], cnt * sizeof(bool));
				memmove(&rb->tag[c][total[g]], &rb->tag[c][from], cnt);
				if (rb->kinds[c] == SIFT_CELL_INT)
					memmove(&rb->i[c][total[g]], &rb->i[c][from], cnt * sizeof(int64));
				else if (rb->kinds[c] == SIFT_CELL_F8)
					memmove(&rb->f[c][total[g]], &rb->f[c][from], cnt * sizeof(double));
				else
				{
					memmove(&rb->s[c][total[g]], &rb->s[c][from], cnt * sizeof(char *));
					memmove(&rb->slen[c][total[g]], &rb->slen[c][from], cnt * sizeof(uint32));
				}
				if (rb->xpay[c] != NULL)
				{
					memmove(&rb->xpay[c][total[g]], &rb->xpay[c][from], cnt * sizeof(char *));
					memmove(&rb->xplen[c][total[g]], &rb->xplen[c][from], cnt * sizeof(uint32));
				}
			}
			rb->estart[g][keep] = total[g];
			total[g] += cnt;
		}
		if (keep != r)
			move_row(rb, keep, r);
		keep++;
	}
	rb->n = keep;
	for (int gi = 0; gi < rb->ngroups; gi++)
	{
		int			g = rb->groups[gi];

		rb->estart[g][keep] = total[g];
		rb->nelem[g] = total[g];
	}
	return dropped;
}

static bool
is_row(SiftRowBuf *rb, int r, void *arg)
{
	return r == *(const int *) arg;
}

void
sift_rowbuf_drop_at(SiftRowBuf *rb, int row)
{
	if (rb->estart != NULL)
	{
		(void) sift_rowbuf_drop_if(rb, is_row, &row);
		return;
	}
	if (row != rb->n - 1)
		move_row(rb, row, rb->n - 1);
	rb->n--;
}

typedef struct SortCtx
{
	const SiftRowBuf *rb;
	const uint16 *cols;
	const uint8 *flags;
	int			ncols;
} SortCtx;

int
sift_rowbuf_cmp_val(const SiftRowBuf *rb, int c, int r1, int r2)
{
	if (rb->kinds[c] == SIFT_CELL_INT)
		return rb->i[c][r1] < rb->i[c][r2] ? -1 : rb->i[c][r1] > rb->i[c][r2];
	if (rb->kinds[c] == SIFT_CELL_F8)
		return rb->f[c][r1] < rb->f[c][r2] ? -1 : rb->f[c][r1] > rb->f[c][r2];
	{
		int			m = memcmp(rb->s[c][r1], rb->s[c][r2], Min(rb->slen[c][r1], rb->slen[c][r2]));

		return m != 0 ? (m < 0 ? -1 : 1) : (rb->slen[c][r1] < rb->slen[c][r2] ? -1 : rb->slen[c][r1] > rb->slen[c][r2]);
	}
}

static int
row_cmp(const void *a, const void *b, void *arg)
{
	const SortCtx *x = arg;
	const SiftRowBuf *rb = x->rb;
	int			r1 = *(const int *) a,
				r2 = *(const int *) b;

	for (int k = 0; k < x->ncols; k++)
	{
		int			c = x->cols[k];
		bool		n1 = rb->isnull[c][r1],
					n2 = rb->isnull[c][r2];
		int			cmp = 0;

		if (n1 || n2)
			cmp = n1 == n2 ? 0 : ((n1 ? 1 : -1) * ((x->flags[k] & SIFT_KEY_NULLS_FIRST) ? -1 : 1));
		else
			cmp = sift_rowbuf_cmp_val(rb, c, r1, r2) * ((x->flags[k] & SIFT_KEY_DESC) ? -1 : 1);
		if (cmp != 0)
			return cmp;
	}
	return rb->tid[r1] < rb->tid[r2] ? -1 : rb->tid[r1] > rb->tid[r2];
}

#define PERMUTE(arr, T) \
	do { \
		T *tmp = palloc(rb->n * sizeof(T)); \
		for (int r = 0; r < rb->n; r++) tmp[r] = (arr)[perm[r]]; \
		memcpy((arr), tmp, rb->n * sizeof(T)); \
		pfree(tmp); \
	} while (0)

void
sift_rowbuf_permute(SiftRowBuf *rb, const int *perm)
{
	if (rb->estart != NULL)
		reorder_elements(rb, perm);
	PERMUTE(rb->tid, uint64);
	PERMUTE(rb->xmin, uint32);
	PERMUTE(rb->cmin, uint32);
	PERMUTE(rb->subxid, SubTransactionId);
	{
		uint8	  (*tmp)[SIFT_BOUND_BYTES] = palloc((Size) rb->n * SIFT_BOUND_BYTES);

		for (int r = 0; r < rb->n; r++)
			memcpy(tmp[r], rb->keys[perm[r]], SIFT_BOUND_BYTES);
		memcpy(rb->keys, tmp, rb->n * SIFT_BOUND_BYTES);
		pfree(tmp);
	}
	for (int c = 0; c < rb->ncols; c++)
	{
		if (sift_rowbuf_is_child(rb, c))
			continue;
		PERMUTE(rb->isnull[c], bool);
		PERMUTE(rb->tag[c], uint8);
		if (rb->kinds[c] == SIFT_CELL_INT)
			PERMUTE(rb->i[c], int64);
		else if (rb->kinds[c] == SIFT_CELL_F8)
			PERMUTE(rb->f[c], double);
		else
		{
			PERMUTE(rb->s[c], const char *);
			PERMUTE(rb->slen[c], uint32);
		}
		if (rb->xpay[c] != NULL)
		{
			PERMUTE(rb->xpay[c], const char *);
			PERMUTE(rb->xplen[c], uint32);
		}
	}
}

void
sift_rowbuf_sort(SiftRowBuf *rb, const SiftPartSpec *spec)
{
	SortCtx		ctx = {rb, spec->sort_cols, spec->sort_flags, spec->nsort};
	int		   *perm = palloc(Max(rb->n, 1) * sizeof(int));

	for (int r = 0; r < rb->n; r++)
		perm[r] = r;
	qsort_arg(perm, rb->n, sizeof(int), row_cmp, &ctx);
	sift_rowbuf_permute(rb, perm);
	pfree(perm);
}

SiftRows
sift_rowbuf_rows(SiftRowBuf *rb, SiftColIn *cols, const SiftPartSpec *spec)
{
	for (int c = 0; c < rb->ncols; c++)
	{
		cols[c] = (SiftColIn) {.kind = rb->kinds[c],.i = rb->i[c],.f = rb->f[c],.s = (const char *const *) rb->s[c],.slen = rb->slen[c],.isnull = rb->isnull[c],.tag = rb->tag[c],
		.hint = spec->hints ? spec->hints[c] & ~SIFT_CHINT_ASTEXT : SIFT_CHINT_AUTO,.astext = spec->hints && (spec->hints[c] & SIFT_CHINT_ASTEXT),.key_leaf = spec->key_leaf && spec->key_leaf[c],.scale = spec->scales ? spec->scales[c] : 0,.xpay = rb->xpay[c],.xplen = rb->xplen[c]};
		if (sift_rowbuf_is_group(rb, c))
		{
			cols[c].hint = SIFT_CHINT_LIST;
			cols[c].eoff = rb->estart[c];
		}
		if (sift_rowbuf_is_child(rb, c))
			cols[c].group1 = rb->parent[c] + 1;
	}
	SiftRows	rows = {.nrows = rb->n,.ncols = rb->ncols,.tid = rb->tid,.xmin = rb->xmin,.cmin = rb->cmin,.cols = cols,
	.keys = (const uint8 (*)[SIFT_BOUND_BYTES]) rb->keys,.sort_cols = spec->sort_cols,.sort_flags = spec->sort_flags,.nsort = spec->nsort,.key_lo = NULL,.key_hi = NULL,
	.preagg = spec->preagg,.preagg_dim = spec->preagg_dim,.preagg_measure = spec->preagg_measure,.time_col = spec->time_col,.compress_alg = spec->compress_alg,.kll = spec->kll,
	.col_ids = spec->col_ids,.ncol_ids = spec->ncol_ids,.defaults = spec->defaults,.defaults_len = spec->defaults_len,.defaults_stamp = spec->defaults_stamp};

	memcpy(rows.time_cols, spec->time_cols, sizeof(rows.time_cols));
	return rows;
}

void
sift_rowbuf_free(SiftRowBuf *rb)
{
	MemoryContextDelete(rb->cx);
}
