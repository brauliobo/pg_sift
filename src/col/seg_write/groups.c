/*
 * col/seg_write/groups.c - the key groups of a scope segment (docs/COL-SCOPE.md "Key groups"), written after the units they summarise
 *
 * A scope key whose rows fill several units has a run of units that hold that key and no other. The superunit summary of such a run is a group of its own (the units of a key do not
 * start at a multiple of the superunit fan). Its cube is the merge of the cubes of its units, as for any group, and then the dims the units lack, made from the rows of the run:
 * a unit holds a few rows of each member of the key, so it has no table of the members (a histogram of groups of fewer than SIFT_PAGG_SCOPE_DIM_ROWS rows is not kept), and the run holds
 * hundreds of rows of each. Those dims have a byte budget of their own (SIFT_PAGG_KEYGROUP_PERMILLE of the bytes of the units) and the smallest are kept first.
 */
/* check-memory: allow-allocs a group holds at most SIFT_SUPER_KEY_ROWS rows of the leaves its cube reads */
#include "core/host.h"
#include "col/seg_write/seg_write_int.h"

/* The cube of a group: the dims of the segment's cube that are not calendar cells or lists, with their integer measures (a float sum depends on the order of its additions, which the units fix) */
static void
kg_spec(SiftColSegWriter *w, KeyGroups *g)
{
	const SiftColSchema *s = w->schema;
	bool		seen[SIFT_COL_MAX_COLS] = {false};

	g->spec = w->pagg;
	g->spec.ndims = 0;
	g->spec.nrollups = 0;
	for (int r = 0; r < w->pagg.nrollups; r++)
		if (w->pagg.rollup[r].list < 0)
		{
			SiftCPaggRollup *ru = &g->spec.rollup[g->spec.nrollups++];

			*ru = w->pagg.rollup[r];
			for (int d = 0; d < ru->ndims; d++)
				seen[ru->dim[d]] = true;
			for (int m = 0; m < ru->nmeas; m++)
				seen[ru->meas[m]] = true;
		}
	g->spec.permille = SIFT_PAGG_KEYGROUP_PERMILLE;
	g->spec.max_groups = SIFT_PAGG_CUBE_GROUPS;
	g->spec.minmax_groups = SIFT_PAGG_KEYGROUP_MINMAX_GROUPS;
	for (int d = 0; d < w->pagg.ndims; d++)
	{
		SiftCPaggDim dim = w->pagg.dim[d];

		if (dim.cal || s->cols[dim.leaf].parent >= 0 || s->cols[dim.leaf].type == SIFT_COLT_DATE || s->cols[dim.leaf].type == SIFT_COLT_TIMESTAMP ||
			s->cols[dim.leaf].type == SIFT_COLT_TIMESTAMPTZ)
			continue;			/* the calendar cells of a time column answer what its histogram does */
		dim.nmeas = 0;
		for (int m = 0; m < w->pagg.dim[d].nmeas; m++)
		{
			int			leaf = w->pagg.dim[d].meas[m];

			if (s->cols[leaf].store == SIFT_COLS_INT && s->cols[leaf].parent < 0)
				dim.meas[dim.nmeas++] = leaf;
		}
		g->spec.dim[g->spec.ndims++] = dim;
		seen[dim.leaf] = true;
		for (int m = 0; m < dim.nmeas; m++)
			seen[dim.meas[m]] = true;
	}
	for (int c = 0; c < s->ncols; c++)
		if (seen[c])
			g->leaf[g->nleaf++] = c;
}

static bool
kg_begin(SiftColSegWriter *w)
{
	KeyGroups  *g;
	Size		words = SIFT_SUPER_KEY_ROWS / 64 + 2;

	if (!w->keyed || w->keycol < 0 || !(w->schema->cols[w->keycol].flags & SIFT_COLD_SCOPE) || w->pagg.super == 0 || w->pagg.off || w->pagg.ndims == 0)
		return false;
	g = palloc0(sizeof(KeyGroups));
	kg_spec(w, g);
	if (g->spec.ndims == 0)
	{
		pfree(g);
		return false;
	}
	g->vals = palloc(g->nleaf * sizeof(int64 *));
	g->nulls = palloc(g->nleaf * sizeof(uint64 *));
	g->ovf = palloc(g->nleaf * sizeof(uint64 *));
	g->anynull = palloc0(g->nleaf * sizeof(bool));
	g->anyovf = palloc0(g->nleaf * sizeof(bool));
	g->ready = palloc(g->nleaf * sizeof(bool));
	for (int i = 0; i < g->nleaf; i++)
	{
		g->vals[i] = palloc(SIFT_SUPER_KEY_ROWS * sizeof(int64));
		g->nulls[i] = palloc0(words * sizeof(uint64));
		g->ovf[i] = palloc0(words * sizeof(uint64));
		g->ready[i] = true;
	}
	g->cx = AllocSetContextCreate(w->mcxt, "sift col key groups", ALLOCSET_DEFAULT_SIZES);
	g->cap = 16;
	g->gfirst = palloc(g->cap * sizeof(uint32));
	g->gn = palloc(g->cap * sizeof(uint32));
	g->xsplit = palloc(g->cap * sizeof(uint64));
	g->cubeoff = palloc((g->cap + 1) * sizeof(uint64));
	g->cubeoff[0] = 0;
	initStringInfo(&g->cube);
	g->on = true;
	w->kg = g;
	return true;
}

static void
kg_reset(KeyGroups *g)
{
	for (int i = 0; i < g->nleaf; i++)
	{
		if (g->anynull[i])
			memset(g->nulls[i], 0, (g->rows / 64 + 2) * sizeof(uint64));
		if (g->anyovf[i])
			memset(g->ovf[i], 0, (g->rows / 64 + 2) * sizeof(uint64));
		g->anynull[i] = g->anyovf[i] = false;
		g->ready[i] = true;
	}
	g->nunits = g->rows = 0;
}

/* The merge of the cubes of the units [first, first + n) (what a group of a fan of units holds), in merged; empty when a unit has no cube or the dims differ */
static void
kg_merged(SiftColSegWriter *w, uint32 first, uint32 n, StringInfo merged)
{
	const uint8 **blob = palloc(n * sizeof(uint8 *));
	uint32	   *len = palloc(n * sizeof(uint32));
	bool		anycube = false;

	for (uint32 i = 0; i < n; i++)
	{
		blob[i] = (const uint8 *) w->cube.data + w->cubeoff[first + i];
		len[i] = w->cubeoff[first + i + 1] - w->cubeoff[first + i];
		anycube |= len[i] > 0;
	}
	if (!anycube || !sift_cpagg_super_cube(w->schema, blob, len, n, merged))
		resetStringInfo(merged);
	pfree(blob);
	pfree(len);
}

/* The dims of the spec of the rows that the merged cube lacks, built from the rows of the open group within their budget, in extra */
static void
kg_extra(SiftColSegWriter *w, KeyGroups *g, const StringInfoData *merged, StringInfo extra, StringInfo rollup)
{
	const SiftColSchema *s = w->schema;
	SiftCHist	h;
	SiftCPaggSpec xs = g->spec;
	SiftCPaggCol *cols;
	Size		unit_bytes = 0;

	memset(&h, 0, sizeof(h));
	if (merged->len > 0 && !sift_cpagg_decode(s, (const uint8 *) merged->data, merged->len, CurrentMemoryContext, &h))
		h.ndims = 0;
	xs.ndims = 0;
	for (int d = 0; d < g->spec.ndims; d++)
	{
		int			k = 0;

		while (k < h.ndims && h.dim[k].leaf != g->spec.dim[d].leaf)
			k++;
		if (k == h.ndims)
			xs.dim[xs.ndims++] = g->spec.dim[d];
	}
	for (uint32 i = 0; i < g->nunits; i++)
		unit_bytes += w->sbdir[g->first + i].len;
	cols = palloc0(s->ncols * sizeof(SiftCPaggCol));
	for (int i = 0; i < g->nleaf; i++)
		cols[g->leaf[i]] = (SiftCPaggCol) {g->vals[i], g->anynull[i] ? g->nulls[i] : NULL, g->anyovf[i] ? g->ovf[i] : NULL, g->rows, g->ready[i]};
	sift_cpagg_cube(s, &xs, cols, g->rows, unit_bytes, extra, rollup);
	pfree(cols);
}

/* A group of the units [first, first + n) with the cube of the merge of its units and the cube of the dims of its rows (either possibly empty: the sums of its leaves are still its own) */
static void
kg_push(KeyGroups *g, uint32 first, uint32 n, const StringInfoData *merged, const StringInfoData *extra, const StringInfoData *rollup)
{
	if (g->ng == g->cap)
	{
		g->cap *= 2;
		g->gfirst = repalloc(g->gfirst, g->cap * sizeof(uint32));
		g->gn = repalloc(g->gn, g->cap * sizeof(uint32));
		g->xsplit = repalloc(g->xsplit, g->cap * sizeof(uint64));
		g->cubeoff = repalloc(g->cubeoff, (g->cap + 1) * sizeof(uint64));
	}
	appendBinaryStringInfo(&g->cube, merged->data, merged->len);
	appendBinaryStringInfo(&g->cube, extra->data, extra->len);
	appendBinaryStringInfo(&g->cube, rollup->data, rollup->len);
	g->gfirst[g->ng] = first;
	g->gn[g->ng] = n;
	g->xsplit[g->ng] = SUPER_X(merged->len, rollup->len > 0 ? extra->len : 0, rollup->len > 0);
	g->cubeoff[++g->ng] = g->cube.len;
}

/*
 * The open group is complete. A run of two units or more is a group: its merged cube and the dims of its rows. A run of more units than the fan of the index is followed by the groups
 * of fan units inside it, with the merged cubes only: a read whose time range cuts the run takes those where it can.
 */
void
segw_kg_close(SiftColSegWriter *w)
{
	KeyGroups  *g = w->kg;

	if (g == NULL || g->nunits == 0)
		return;
	if (g->nunits >= SIFT_SUPER_MIN_FAN)
	{
		MemoryContext old = MemoryContextSwitchTo(g->cx);
		StringInfoData merged,
					extra,
					rollup,
					none;

		initStringInfo(&merged);
		initStringInfo(&extra);
		initStringInfo(&rollup);
		initStringInfo(&none);
		kg_merged(w, g->first, g->nunits, &merged);
		kg_extra(w, g, &merged, &extra, &rollup);
		kg_push(g, g->first, g->nunits, &merged, &extra, &rollup);
		for (uint32 at = 0; g->nunits > w->pagg.super && at + SIFT_SUPER_MIN_FAN <= g->nunits; at += w->pagg.super)
		{
			uint32		n = Min(w->pagg.super, g->nunits - at);

			resetStringInfo(&merged);
			kg_merged(w, g->first + at, n, &merged);
			kg_push(g, g->first + at, n, &merged, &none, &none);
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(g->cx);
	}
	kg_reset(g);
}

void
segw_kg_add(SiftColSegWriter *w, const SiftCPaggCol *cols, uint32 nrows)
{
	const SiftCZone *z = w->zones + (Size) w->nsb * w->schema->ncols + Max(w->keycol, 0);
	KeyGroups  *g;
	bool		one;

	if (!w->kg_known)
	{
		kg_begin(w);
		w->kg_known = true;
	}
	if ((g = w->kg) == NULL)
		return;
	one = z->nnull == 0 && z->min == z->max;
	if (g->nunits > 0 && !(one && z->min == g->key && g->nunits < SIFT_SUPER_MAX_FAN && g->rows + nrows <= SIFT_SUPER_KEY_ROWS))
		segw_kg_close(w);
	if (!one)
		return;
	if (g->nunits == 0)
	{
		g->first = w->nsb;
		g->key = z->min;
	}
	for (int i = 0; i < g->nleaf; i++)
	{
		const SiftCPaggCol *c = &cols[g->leaf[i]];

		g->ready[i] &= c->ready && c->n == nrows;
		if (c->key == NULL || c->n != nrows)
		{
			g->ready[i] = false;
			memset(g->vals[i] + g->rows, 0, nrows * sizeof(int64));
		}
		else
			memcpy(g->vals[i] + g->rows, c->key, nrows * sizeof(int64));
		if (c->nulls)
		{
			seg_bits_put(g->nulls[i], g->rows, c->nulls, nrows);
			g->anynull[i] = true;
		}
		if (c->ovf)
		{
			seg_bits_put(g->ovf[i], g->rows, c->ovf, nrows);
			g->anyovf[i] = true;
		}
	}
	g->nunits++;
	g->rows += nrows;
}
