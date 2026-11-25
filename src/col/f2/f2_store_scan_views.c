/*
 * col/f2/f2_store_scan_views.c - the columns of a granule of a sift_table as the scan reads them: decoded from the part, widened from scaled integers, or the constant of a column the part lacks
 * (col/f2/f2_store_scan_int.h).
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/f2/f2_store_scan_int.h"
#include "col/f2/f2_store_schema.h"
#include "table/f2/f2_blob.h"

static void
unsupported_form(const SiftF2Store *store, int leaf)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a part of the table holds column \"%s\" in a form the aggregate does not read", store->schema->cols[leaf].name),
					errhint("VACUUM (FREEZE) the table to rewrite its old parts, or run the statement with sift.enable_agg = off.")));
}

static ColView
constant_view(SiftStorePart *p, int leaf, const SiftF2ColMap *map)
{
	SiftF2GranuleBuf *gb = p->r->gb;
	int			n = gb->nrows;
	int16		attno = p->s->store->leaf_attno[leaf];
	SiftCellKind kind = p->s->store->kinds[leaf];
	ColView		v = {.kind = kind};
	bool	   *isnull = MemoryContextAlloc(p->r->gcx, n);
	uint8	   *tags = map->missing_cell[leaf].tag != 0 ? MemoryContextAllocZero(p->r->gcx, n) : NULL;
	int64	   *ints = kind == SIFT_CELL_INT ? MemoryContextAlloc(p->r->gcx, sizeof(int64) * n) : NULL;
	double	   *fs = kind == SIFT_CELL_F8 ? MemoryContextAlloc(p->r->gcx, sizeof(double) * n) : NULL;
	const char **ss = kind == SIFT_CELL_STR ? MemoryContextAlloc(p->r->gcx, sizeof(char *) * n) : NULL;
	uint32	   *lens = kind == SIFT_CELL_STR ? MemoryContextAlloc(p->r->gcx, sizeof(uint32) * n) : NULL;

	for (int r = 0; r < n; r++)
	{
		const SiftColDeltaPatch *patch = gb->patch != NULL ? sift_col_delta_find_row(gb->patch_arg, attno, (uint16) r) : NULL;
		SiftCell	c = map->missing_cell[leaf];

		if (patch != NULL)
		{
			c.tag = 0;
			c.isnull = (patch->flags & SIFT_CDCOL_IS_NULL) != 0;
			c.i = patch->val.i;
			c.f = patch->val.f;
			c.s = patch->val.s;
			c.slen = patch->vlen;
		}
		isnull[r] = c.isnull;
		if (tags != NULL)
			tags[r] = c.tag;
		if (ints != NULL)
			ints[r] = c.i;
		if (fs != NULL)
			fs[r] = c.f;
		if (ss != NULL)
		{
			ss[r] = c.s;
			lens[r] = c.slen;
		}
	}
	v.isnull = isnull;
	v.tag = tags;
	v.i = ints;
	v.f = fs;
	v.s = ss;
	v.slen = lens;
	return v;
}

static ColView
decoded_view(SiftStorePart *p, int leaf, int phys, SiftCellKind kind)
{
	const SiftStoreScan *s = p->s;
	SiftF2GranuleBuf *gb = p->r->gb;
	ColView		v = {.kind = kind};

	if (!gb->col_decoded[phys])
		sift_f2_gbuf_decode(gb, phys, kind, s->wide || s->store->hints[leaf] == SIFT_CHINT_NUM64 || sift_f2_leaf_is_path(s->store, leaf));
	v.isnull = gb->col_isnull[phys];
	v.tag = gb->col_tag[phys];
	v.i = gb->col_i[phys];
	v.f = gb->col_f[phys];
	if (kind == SIFT_CELL_STR)
	{
		const char **ss = MemoryContextAlloc(p->r->gcx, sizeof(char *) * gb->nrows);	/* check-memory: allow the rows of one granule (SIFT_GRANULE_ROWS at most) */
		uint32	   *lens = MemoryContextAlloc(p->r->gcx, sizeof(uint32) * gb->nrows);
		bool	   *nulls = memcpy(MemoryContextAlloc(p->r->gcx, gb->nrows), gb->col_isnull[phys], gb->nrows);
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(s->o->rel), s->store->leaf_attno[leaf] - 1);

		for (int r = 0; r < gb->nrows; r++)
		{
			const SiftStrCell *c = &gb->col_s[phys][r];

			if (gb->col_tag[phys] != NULL && gb->col_tag[phys][r] == SIFT_COLTAG_WIDE_BLOB)
			{
				uint32		len;
				const char *ref = sift_granule_exc_payload(&gb->view, phys, r, &len);
				struct varlena *val = (struct varlena *) DatumGetPointer(s->store->host->read_wide_blob(s->o->rel, ref, len, att->atttypid, att->attlen, p->r->gcx));

				ss[r] = (const char *) val;
				lens[r] = VARSIZE_ANY(val);
				nulls[r] = false;
				continue;
			}
			ss[r] = c->p;
			lens[r] = c->len;
		}
		v.s = ss;
		v.slen = lens;
		v.isnull = nulls;
	}
	if (sift_f2_leaf_is_path(s->store, leaf) && v.tag != NULL)
		sift_f2_decode_payloads(&gb->view, phys, gb->nrows, &v);
	return v;
}

/* A numeric column that ALTER TABLE .. TYPE left unconstrained or wide keeps the parts written as scaled integers: their values are read as the numerics they hold, a NaN from its exception */
static ColView
widened_view(SiftStorePart *p, int leaf, int phys, uint8 scale)
{
	SiftF2GranuleBuf *gb = p->r->gb;
	int			n = gb->nrows;
	const char **ss = MemoryContextAlloc(p->r->gcx, sizeof(char *) * n);	/* check-memory: allow the rows of one granule (SIFT_GRANULE_ROWS at most) */
	uint32	   *lens = MemoryContextAlloc(p->r->gcx, sizeof(uint32) * n);
	bool	   *nulls = MemoryContextAlloc(p->r->gcx, n);
	MemoryContext old;
	ColView		v;

	if (!gb->col_decoded[phys])
		sift_f2_gbuf_decode(gb, phys, SIFT_CELL_INT, true);
	v = decoded_view(p, leaf, phys, SIFT_CELL_INT);
	old = MemoryContextSwitchTo(p->r->gcx);
	for (int r = 0; r < n; r++)
	{
		const bool	exc = v.tag != NULL && v.tag[r] != 0;

		nulls[r] = v.isnull[r] && !exc;
		ss[r] = NULL;
		lens[r] = 0;
		if (exc)
		{
			const char *payload = sift_granule_exc_payload(&gb->view, phys, r, &lens[r]);

			ss[r] = DatumGetPointer(sift_f2_num64_exception_datum(payload, lens[r], p->r->gcx));
		}
		else if (!nulls[r])
		{
			ss[r] = DatumGetPointer(sift_f2_num64_datum(v.i[r], scale));
			lens[r] = VARSIZE_ANY(ss[r]);
		}
	}
	MemoryContextSwitchTo(old);
	return (ColView) {.kind = SIFT_CELL_STR,.isnull = nulls,.s = ss,.slen = lens};
}

void
sift_store_scan_load_views(SiftStorePart *p)
{
	const SiftStoreScan *s = p->s;
	const SiftF2ColMap *map = p->r->gb->pin->col_map;

	for (int leaf = 0; leaf < s->store->ncols; leaf++)
	{
		int			phys = map != NULL ? map->phys_col[leaf] : leaf;

		if (!s->want[leaf])
			continue;
		if (phys < 0 && sift_f2_leaf_is_path(s->store, leaf))
			unsupported_form(s->store, leaf);
		else if (phys < 0)
			p->r->views[leaf] = constant_view(p, leaf, map);
		else if (map != NULL && map->phys_kind[leaf] == SIFT_CELL_INT && s->store->kinds[leaf] == SIFT_CELL_STR)
			p->r->views[leaf] = widened_view(p, leaf, phys, map->old_scale[leaf]);
		else
			p->r->views[leaf] = decoded_view(p, leaf, phys, s->store->kinds[leaf]);
	}
}