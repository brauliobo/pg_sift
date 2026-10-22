/*
 * col/scan_read.c - the reads of the column source: values of a leaf at the positions of a selection, lanes, groups and TIDs (col/col.h).
 */

#include "postgres.h"

#include "common/pg_prng.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "utils/rls.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "core/popcount.h"
#include "col/gpu.h"
#include "col/pagg.h"
#include "bm25/estimate.h"
#include "col/seg.h"
#include "core/visibility.h"
#include "col/scan_int.h"

/* The bits of src at idx[0..m) as a bit vector of m bits; NULL when none is set */
static uint64 *
gather_bits(MemoryContext cx, const uint64 *src, const uint32 *idx, uint32 m)
{
	uint64	   *out = MemoryContextAllocZero(cx, SIFT_COL_NWORDS(m) * sizeof(uint64));
	bool		any = false;

	for (uint32 k = 0; k < m; k++)
		if (sift_col_bit_test(src, idx[k]))
		{
			sift_col_bit_set(out, k);
			any = true;
		}
	return any ? out : NULL;
}

/* VAR leaf: the strings at idx[0..m) (idx NULL: all of them) with their byte offsets; a unit without the chunk holds m empty strings */
static void
read_strs(MemoryContext arena, const SiftCChunk *c, const uint32 *idx, uint32 m, SiftColVec *vec)
{
	uint64	   *vo = MemoryContextAllocZero(arena, ((Size) m + 1) * sizeof(uint64));
	uint32	   *off;
	char	   *bytes;

	vec->voff = vo;
	vec->vbytes = "";
	if (c == NULL)
		return;
	sift_cchunk_strs(c, arena, &off, &bytes);
	for (uint32 k = 0; k < m; k++)
		vo[k + 1] = vo[k] + (idx ? off[idx[k] + 1] - off[idx[k]] : off[k + 1] - off[k]);
	vec->vbytes = bytes;
	if (idx == NULL)
		return;
	vec->vbytes = MemoryContextAlloc(arena, vo[m] + 1);
	for (uint32 k = 0; k < m; k++)
		memcpy((char *) vec->vbytes + vo[k], bytes + off[idx[k]], vo[k + 1] - vo[k]);
}

/* Decodes the values at positions idx[0..m) of leaf col's chunk (idx NULL: all of them) */
static void
read_positions(ColUnit *cu, int col, const uint32 *idx, uint32 m, uint32 total, SiftColVec *vec)
{
	MemoryContext arena = cu->src->arena;
	const SiftColDef *def = &cu->src->schema->cols[col];
	int64	   *keys = MemoryContextAllocZero(arena, (m + SIFT_COL_PAD / 8) * sizeof(int64));
	SiftCChunk	c;
	bool		present = unit_chunk(cu, col, &c);

	vec->store = def->store;
	vec->n = m;
	if (!present)
	{
		uint64	   *nulls = MemoryContextAllocZero(arena, SIFT_COL_NWORDS(m) * sizeof(uint64));

		set_ones(nulls, m);
		vec->nulls = nulls;
	}
	else
	{
		if (c.n != total)
			corrupt("The chunk of leaf %d holds %u values, %u expected.", col, c.n, total);
		if (idx)
			sift_cchunk_decode_sel(&c, idx, m, keys);
		else
			sift_cchunk_decode(&c, keys);
		cu->src->cnt.bytes_decoded += c.paylen;
		vec->nulls = c.nulls == NULL ? NULL : idx ? gather_bits(arena, c.nulls, idx, m) : c.nulls;
		vec->ovf = c.ovf == NULL ? NULL : idx ? gather_bits(arena, c.ovf, idx, m) : c.ovf;
	}
	switch (def->store)
	{
		case SIFT_COLS_INT:
			vec->i = keys;
			break;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			{
				double	   *f = MemoryContextAlloc(arena, (m + 1) * sizeof(double));

				for (uint32 k = 0; k < m; k++)
					f[k] = sift_col_f8_from_key(keys[k]);
				vec->f = f;
				break;
			}
		case SIFT_COLS_STR:
			{
				uint32	   *codes = MemoryContextAlloc(arena, (m + 1) * sizeof(uint32));

				for (uint32 k = 0; k < m; k++)
					codes[k] = (uint32) keys[k];
				vec->codes = codes;
				if (!cu->codes_only)
				{
					SiftColDict *d = MemoryContextAlloc(arena, sizeof(SiftColDict));

					sift_cdict_view(unit_dict(cu, col), dict_id(cu, col), d);
					sift_ccodes_below(codes, m, d->n + 1);
					vec->dict = d;
				}
				break;
			}
		case SIFT_COLS_VAR:
			read_strs(arena, present ? &c : NULL, idx, m, vec);
			break;
		default:
			SIFT_INVARIANT(false, "sift_col: storage class %d cannot be read yet", def->store);
	}
}

/* The row positions of sel, decoded once for the leaves read through it (a new selection replaces the cache) */
static const uint32 *
sel_positions(ColUnit *cu, const SiftColSel *sel)
{
	ColSelCache *sc = &cu->selc;

	if (sc->idx == NULL || sc->n != sel->n || sc->count != sel->count || memcmp(sc->bits, sel->bits, sel->nwords * sizeof(uint64)) != 0)
	{
		MemoryContext arena = cu->src->arena;

		sc->gen++;
		sc->n = sel->n;
		sc->count = sel->count;
		sc->bits = MemoryContextAlloc(arena, sel->nwords * sizeof(uint64));
		memcpy(sc->bits, sel->bits, sel->nwords * sizeof(uint64));
		sc->idx = MemoryContextAlloc(arena, (sel->count + SIFT_COL_IDXPAD) * sizeof(uint32));
		sift_col_sel_idx(sel, sc->idx);
	}
	return sc->idx;
}

/* The element positions (pos, ne of them) and the per-row offsets (m + 1) of the rows idx names in a group */
static void
group_expand(MemoryContext arena, const ColGroup *g, const uint32 *idx, uint32 m, uint32 **offsets, uint32 **pos, uint32 *nelems)
{
	uint32		ne = 0;

	for (uint32 k = 0; k < m; k++)
		ne += g->counts[idx[k]];
	*offsets = MemoryContextAlloc(arena, (m + 1) * sizeof(uint32));
	*pos = MemoryContextAlloc(arena, (ne + 1) * sizeof(uint32));
	ne = 0;
	for (uint32 k = 0; k < m; k++)
	{
		(*offsets)[k] = ne;
		for (uint32 e = g->eoff[idx[k]]; e < g->eoff[idx[k] + 1]; e++)
			(*pos)[ne++] = e;
	}
	(*offsets)[m] = ne;
	*nelems = ne;
}

/* The same for the cached selection's rows, built once per group */
static void
group_sel_elems(ColUnit *cu, ColGroup *g, const uint32 *idx, uint32 m)
{
	if (g->selgen != cu->selc.gen)
	{
		group_expand(cu->src->arena, g, idx, m, &g->seloff, &g->selpos, &g->selne);
		g->selgen = cu->selc.gen;
	}
}

void
sift_col_read_idx(const SiftColUnit *unit, int col, const uint32 *idx, uint32 m, SiftColVec *vec)
{
	ColUnit    *cu = unit->priv;
	const SiftColDef *def = &cu->src->schema->cols[col];

	memset(vec, 0, sizeof(SiftColVec));
	if (def->parent < 0)
	{
		read_positions(cu, col, idx, m, cu->v.nrows, vec);
		return;
	}
	{
		ColGroup   *g = unit_group(cu, def->parent);
		uint32	   *offsets,
				   *pos,
					ne;

		if (idx == NULL)
		{
			read_positions(cu, col, NULL, g->nelems, g->nelems, vec);
			vec->offsets = g->eoff;
			vec->nrows = m;
			return;
		}
		group_expand(cu->src->arena, g, idx, m, &offsets, &pos, &ne);
		read_positions(cu, col, pos, ne, g->nelems, vec);
		vec->offsets = offsets;
		vec->nrows = m;
	}
}

/* sift_col_read_idx over the rows of sel: its positions and a child leaf's element positions are cached while the selection is the same */
void
sift_col_read(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vec)
{
	ColUnit    *cu = unit->priv;
	const SiftColDef *def = &cu->src->schema->cols[col];
	const uint32 *idx = sel ? sel_positions(cu, sel) : NULL;
	uint32		m = sel ? sel->count : cu->v.nrows;

	if (idx != NULL && def->parent >= 0)
	{
		ColGroup   *g = unit_group(cu, def->parent);

		memset(vec, 0, sizeof(SiftColVec));
		group_sel_elems(cu, g, idx, m);
		read_positions(cu, col, g->selpos, g->selne, g->nelems, vec);
		vec->offsets = g->seloff;
		vec->nrows = m;
		return;
	}
	sift_col_read_idx(unit, col, idx, m, vec);
}

void
sift_col_read_text(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vec)
{
	ColUnit    *cu = unit->priv;
	uint32		m = sel ? sel->count : cu->v.nrows;
	bool		lazy = cu->seg != NULL && !unit_inline(cu, col) && sift_cseg_dict_lazy(cu->seg, col, m);
	SiftColVec	c;
	uint64	   *vo = MemoryContextAllocZero(cu->src->arena, ((Size) m + 1) * sizeof(uint64));
	StringInfoData sb;
	MemoryContext old;
	char		buf[SIFT_COL_DICT_MAX_ENTRY];

	cu->codes_only = lazy;
	sift_col_read(unit, col, sel, &c);
	cu->codes_only = false;
	old = MemoryContextSwitchTo(cu->src->arena);
	initStringInfo(&sb);
	for (uint32 k = 0; k < m; k++)
	{
		const char *s = "";
		uint32		len = 0;

		if (!(c.nulls && (c.nulls[k >> 6] >> (k & 63) & 1)))
		{
			if (lazy)
				s = sift_cseg_dict_get(cu->seg, col, c.codes[k], &len, buf);
			else
			{
				s = SIFT_COL_DICT_STR(c.dict, c.codes[k]);
				len = SIFT_COL_DICT_LEN(c.dict, c.codes[k]);
			}
		}
		appendBinaryStringInfo(&sb, s, len);
		vo[k + 1] = sb.len;
	}
	MemoryContextSwitchTo(old);
	*vec = c;
	vec->store = SIFT_COLS_VAR;
	vec->codes = NULL;
	vec->dict = NULL;
	vec->voff = vo;
	vec->vbytes = sb.data;
}

uint32
sift_col_jnull_bits(const SiftColUnit *unit, int col, const uint32 *idx, uint32 m, uint64 *bits, bool *known)
{
	ColUnit    *cu = unit->priv;
	const SiftColDef *def = &cu->src->schema->cols[col];
	ColGroup   *g = def->parent < 0 ? NULL : unit_group(cu, def->parent);
	uint32		total = g ? g->nelems : cu->v.nrows,
				n = total;
	const uint32 *pos = idx;
	SiftCChunk	c;
	uint64	   *tagged;

	if (idx && g)
	{
		uint32	   *offsets,
				   *epos;

		group_expand(cu->src->arena, g, idx, m, &offsets, &epos, &n);
		pos = epos;
	}
	else if (idx)
		n = m;
	memset(bits, 0, SIFT_COL_NWORDS(n) * sizeof(uint64));
	*known = true;
	if (!unit_chunk(cu, col, &c))
		return n;
	if (c.n != total)
		corrupt("The chunk of leaf %d holds %u values, %u expected.", col, c.n, total);
	tagged = palloc(SIFT_COL_NWORDS(c.n) * sizeof(uint64));
	sift_cchunk_tag_bits(&c, SIFT_COLTAG_JNULL, tagged);
	for (uint32 k = 0; k < n; k++)
		if (sift_col_bit_test(tagged, pos ? pos[k] : k))
			sift_col_bit_set(bits, k);
	pfree(tagged);
	*known = types_known(cu, &c, 1U << SIFT_COLTAG_JNULL, false, NULL);
	return n;
}

uint32
sift_col_lane_n(const SiftColUnit *unit, int col)
{
	ColUnit    *cu = unit->priv;
	int			parent = cu->src->schema->cols[col].parent;

	return parent < 0 ? cu->v.nrows : unit_group(cu, parent)->nelems;
}

static const size_t lane_size[] = {sizeof(int32), sizeof(int64), sizeof(double), sizeof(uint32)};

/* The lane a storage class is read into: the one its keys decode to without conversion, or (INT) the 32-bit one for int2, int4, date, bool */
static bool
lane_fits(SiftColStore store, SiftColLane lane)
{
	switch (store)
	{
		case SIFT_COLS_INT:
			return lane == SIFT_COLL_I32 || lane == SIFT_COLL_I64;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			return lane == SIFT_COLL_F64;
		case SIFT_COLS_STR:
			return lane == SIFT_COLL_CODE;
		default:
			return false;
	}
}

static void
decode_lane(const SiftCChunk *c, SiftColLane lane, uint32 ndict, void *dst)
{
	switch (lane)
	{
		case SIFT_COLL_CODE:
			sift_cchunk_decode_codes(c, ndict, dst);
			break;
		case SIFT_COLL_I64:
			sift_cchunk_decode(c, dst);
			break;
		case SIFT_COLL_F64:
			sift_cchunk_decode_f8(c, dst);
			break;
		default:
			sift_cchunk_decode_u32(c, dst);
	}
}

void
sift_col_read_lane(const SiftColUnit *unit, int col, SiftColLane lane, void *dst, uint8 *isnull, SiftColLaneInfo *info)
{
	ColUnit    *cu = unit->priv;
	const SiftColDef *def = &cu->src->schema->cols[col];
	const uint64 *nulls = NULL;
	uint32		n = sift_col_lane_n(unit, col);
	SiftCChunk	c;

	memset(info, 0, sizeof(*info));
	info->n = n;
	SIFT_INVARIANT(def->kind == SIFT_COLK_GROUP ? lane == SIFT_COLL_I32 : lane_fits(def->store, lane),
				   "sift_col: leaf %d (storage class %d) cannot be read as lane %d", col, def->store, lane);
	if (lane == SIFT_COLL_CODE)
	{
		SiftCDict  *dict = unit_dict(cu, col);	/* first: an inline chunk's dictionary and its id exist once the chunk is read */

		sift_cdict_view(dict, dict_id(cu, col), &info->dict);
	}
	if (def->kind == SIFT_COLK_GROUP)
	{
		ColGroup   *g = unit_group(cu, col);

		memcpy(dst, g->counts, n * sizeof(uint32));
		nulls = g->nulls;
		info->ovf = g->ovf != NULL;
	}
	else if (!unit_chunk(cu, col, &c))
	{
		memset(dst, 0, n * lane_size[lane]);
		if (isnull)
			memset(isnull, 1, n);
		info->nulls = isnull != NULL;
	}
	else
	{
		if (c.n != n)
			corrupt("The chunk of leaf %d holds %u values, %u expected.", col, c.n, n);
		decode_lane(&c, lane, info->dict.n, dst);
		cu->src->cnt.bytes_decoded += c.paylen;
		nulls = c.nulls;
		info->ovf = c.ovf != NULL;
	}
	if (nulls != NULL && isnull != NULL)
	{
		sift_colk->bits_to_flags(nulls, n, isnull);
		info->nulls = true;
	}
}

void
sift_col_group_parents(const SiftColUnit *unit, int group, const uint32 **parent, const uint32 **counts)
{
	ColUnit    *cu = unit->priv;
	ColGroup   *g = unit_group(cu, group);

	if (g->parent == NULL)
	{
		g->parent = MemoryContextAlloc(cu->src->arena, g->nelems * sizeof(uint32) + SIFT_COL_PAD);
		sift_colk->expand_parents(g->eoff + 1, cu->v.nrows, g->parent);
	}
	*parent = g->parent;
	*counts = g->counts;
}

bool
sift_col_unit_ref(const SiftColUnit *unit, SiftColSegment **seg, uint32 *u)
{
	const ColUnit *cu = unit->priv;

	if (cu->seg == NULL)
		return false;
	*seg = cu->seg;
	*u = cu->u;
	return true;
}

void
sift_col_tids(const SiftColUnit *unit, const SiftColSel *sel, ItemPointerData *tids)
{
	ColUnit    *cu = unit->priv;
	const SiftKey *keys;
	uint32		k = 0;

	/* a point lookup selects a row or two of thousands: only the pages that hold them are decoded */
	if (cu->keys == NULL && sel->count <= sel->n / 64)
	{
		uint32	   *ords = palloc(Max(sel->count, 1) * sizeof(uint32));

		FOREACH_BIT(sel->bits, (sel->n + 63) / 64, r, { ords[k++] = r; });
		sift_col_row_tids(unit, ords, k, tids);
		pfree(ords);
		return;
	}
	keys = unit_keys(cu);
	FOREACH_BIT(sel->bits, (sel->n + 63) / 64, r, { sift_key_to_tid(keys[r], &tids[k++]); });
}

/* The TIDs of the rows ords (ascending ordinals of the unit as stored), without the keys of the others */
void
sift_col_row_tids(const SiftColUnit *unit, const uint32 *ords, uint32 n, ItemPointerData *tids)
{
	ColUnit    *cu = unit->priv;
	SiftKey	   *keys = palloc(Max(n, 1) * sizeof(SiftKey));

	sift_cunit_row_keys(&cu->v, cu->sb, ords, n, keys);
	for (uint32 k = 0; k < n; k++)
		sift_key_to_tid(keys[k], &tids[k]);
	pfree(keys);
}
