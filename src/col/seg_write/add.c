/*
 * col/seg_write/add.c - what a batch adds to a segment: points, sets, runs, hll, excepts,
 * the pre-aggregates and the row versions
 * one function for each section the writer keeps in memory
 */
#include "core/host.h"
#include "col/hll.h"
#include "col/setsum.h"
#include "col/seg_write/seg_write_int.h"

/* Adds the rows of the unit just encoded to the point directories of the leaves that ask for one (the keys of cols: the segment's codes of a dictionary leaf) */
static void
add_points(SiftColSegWriter *w, const SiftColBatch *b, const SiftCPaggCol *cols)
{
	const SiftKey *rowkeys = b->keys;

	for (int c = 0; w->pt && c < w->schema->ncols; c++)
	{
		PointAcc   *a = &w->pt[c];
		const SiftCPaggCol *v = &cols[c];

		bool		wt = a->tids || a->ord;		/* the entries carry heap addresses */
		const SiftCPaggCol *gv = a->ord && segw_gorder_grouped(w, c) ? &cols[w->pagg.gorder_group - 1] : NULL;	/* the group of each row */

		if (!(a->want || a->ord) || a->bad)
			continue;
		if (a->ord && w->inl[c])
			a->bad = true;
		for (uint32 i = 0; v->ovf && i < SIFT_COL_NWORDS(v->n); i++)
			if (v->ovf[i])
				a->bad = true;
		for (uint32 i = 0; gv && gv->ovf && i < SIFT_COL_NWORDS(gv->n); i++)
			if (gv->ovf[i])
				a->bad = true;
		if (!v->ready || (gv && !gv->ready) || a->bad || w->rows + v->n >= SIFT_CPOINT_SEG_ROWS)
		{
			a->bad = true;
			continue;
		}
		if (a->n + v->n > a->cap)
		{
			MemoryContext old = MemoryContextSwitchTo(w->mcxt);

			a->cap = Max(Max(a->cap * 2, 4096), a->n + v->n);
			a->key = a->key ? repalloc_huge(a->key, (Size) a->cap * sizeof(int64)) : MemoryContextAllocHuge(CurrentMemoryContext, (Size) a->cap * sizeof(int64));
			a->row = a->row ? repalloc_huge(a->row, (Size) a->cap * sizeof(uint32)) : MemoryContextAllocHuge(CurrentMemoryContext, (Size) a->cap * sizeof(uint32));
			if (wt)
				a->tid = a->tid ? repalloc_huge(a->tid, (Size) a->cap * sizeof(uint64)) : MemoryContextAllocHuge(CurrentMemoryContext, (Size) a->cap * sizeof(uint64));
			MemoryContextSwitchTo(old);
		}
		if (a->ord && v->nulls && a->nn + v->n > a->ncap)
		{
			MemoryContext old = MemoryContextSwitchTo(w->mcxt);

			a->ncap = Max(Max(a->ncap * 2, 1024), a->nn + v->n);
			a->ntid = a->ntid ? repalloc_huge(a->ntid, (Size) a->ncap * sizeof(uint64)) : MemoryContextAllocHuge(CurrentMemoryContext, (Size) a->ncap * sizeof(uint64));
			if (gv)
			{
				a->ngk = a->ngk ? repalloc_huge(a->ngk, (Size) a->ncap * sizeof(int64)) : MemoryContextAllocHuge(CurrentMemoryContext, (Size) a->ncap * sizeof(int64));
				a->ngok = a->ngok ? repalloc_huge(a->ngok, (Size) a->ncap) : MemoryContextAllocHuge(CurrentMemoryContext, (Size) a->ncap);
			}
			MemoryContextSwitchTo(old);
		}
		if (gv && a->n + v->n > a->gcap)
		{
			MemoryContext old = MemoryContextSwitchTo(w->mcxt);

			a->gcap = a->cap;
			a->gk = a->gk ? repalloc_huge(a->gk, (Size) a->gcap * sizeof(int64)) : MemoryContextAllocHuge(CurrentMemoryContext, (Size) a->gcap * sizeof(int64));
			a->gok = a->gok ? repalloc_huge(a->gok, (Size) a->gcap) : MemoryContextAllocHuge(CurrentMemoryContext, (Size) a->gcap);
			MemoryContextSwitchTo(old);
		}
		if (segw_elem_point(w, c))
		{
			const uint32 *eoff = b->col[w->schema->cols[c].parent].eoff;

			for (uint32 r = 0; r < b->nrows; r++)
				for (uint32 j = eoff[r]; j < eoff[r + 1]; j++)
				{
					if (v->nulls && (v->nulls[j / 64] >> (j % 64) & 1))
						continue;
					a->key[a->n] = v->key[j];
					a->row[a->n++] = (uint32) w->rows + r;
				}
			continue;
		}
		for (uint32 i = 0; i < v->n; i++)
		{
			/* no offset number of a heap page of 8 kB reaches the codes' room, but a key from elsewhere must not be listed wrongly */
			if (wt && (rowkeys[i] & 0xFFFF) >= SIFT_CPOINT_TID_MAXOFF)
			{
				a->bad = true;
				break;
			}
			if (v->nulls && (v->nulls[i / 64] >> (i % 64) & 1))
			{
				if (a->ord)
				{
					if (gv)
					{
						bool		gnull = gv->nulls && (gv->nulls[i / 64] >> (i % 64) & 1);

						a->ngok[a->nn] = !gnull;
						a->ngk[a->nn] = gnull ? 0 : gv->key[i];
					}
					a->ntid[a->nn++] = rowkeys[i];
				}
				continue;
			}
			if (gv)
			{
				bool		gnull = gv->nulls && (gv->nulls[i / 64] >> (i % 64) & 1);

				a->gok[a->n] = !gnull;
				a->gk[a->n] = gnull ? 0 : gv->key[i];
			}
			a->key[a->n] = v->key[i];
			if (wt)
				a->tid[a->n] = rowkeys[i];
			a->row[a->n++] = (uint32) w->rows + i;
		}
	}
}

/*
 * The keys of each leaf of key_columns, from the keys of the unit just encoded (in the segment's codes for a dictionary leaf; none for an inline one): all of
 * them as a bitmap or a list where that takes the share of the unit's bytes (unit_bytes) that the leaves left, else, for a leaf the user named, a summary
 * (up to SIFT_CSET_LIMIT keys exactly, a bloom filter beyond). A leaf the build chose (keyauto) has no summary: it was chosen for its lists.
 */
static void
add_sets(SiftColSegWriter *w, const SiftCPaggCol *cols, Size unit_bytes)
{
	int64	   *keys = NULL;
	Size		left = unit_bytes / 100 * SIFT_CSET_PRES_PCT;	/* the key lists and bitmaps of all the leaves of a unit share this, the first leaves first */
	bool	   *filter = NULL;	/* the leaves of a default key_columns that got no list: a bloom filter of what the lists left */

	for (int c = 0; w->set && c < w->schema->ncols; c++)
	{
		SetAcc	   *a = &w->set[c];
		uint32		nd;
		Size		had = a->blobs.len;

		if (a->blobs.data == NULL)
			continue;
		a->off[w->nsb] = a->blobs.len;
		if (cols[c].ready && cols[c].n > 1)
		{
			if (w->pagg.pres[c] && sift_cset_build_pres(&cols[c], left, &a->blobs))
			{
				left -= a->blobs.len - had;
				a->any = w->anysets = w->anypres = true;
				a->kinds |= SETS_PRES;
				w->anyrunl |= a->blobs.data[had] == SIFT_CSET_RUNS;
			}
			else if (w->pagg.pres[c] && w->pagg.keyauto)
			{
				if (filter == NULL)
					filter = palloc0(w->schema->ncols * sizeof(bool));
				filter[c] = true;
			}
			else if (w->pagg.set[c] && !w->pagg.keyauto)
			{
				keys = keys ? repalloc(keys, (Size) cols[c].n * sizeof(int64)) : palloc((Size) cols[c].n * sizeof(int64));
				nd = sift_cset_keys(&cols[c], keys);
				if (sift_cset_build(keys, nd, SIFT_CSET_LIMIT, &a->blobs))
				{
					a->any = w->anysets = true;
					a->kinds |= a->blobs.data[a->off[w->nsb]] == SIFT_CSET_EXACT ? SETS_EXACT : SETS_BLOOM;
				}
			}
		}
		a->off[w->nsb + 1] = a->blobs.len;
	}
	left += unit_bytes / 100 * (SIFT_CSET_FILTER_PCT - SIFT_CSET_PRES_PCT);
	for (int c = 0; filter && c < w->schema->ncols; c++)
	{
		SetAcc	   *a = &w->set[c];
		uint32		nd;
		Size		had = a->blobs.len;

		if (!filter[c])
			continue;
		keys = keys ? repalloc(keys, (Size) cols[c].n * sizeof(int64)) : palloc((Size) cols[c].n * sizeof(int64));
		nd = sift_cset_keys(&cols[c], keys);
		if (sift_cset_build_filter(keys, nd, left, &a->blobs))
		{
			left -= a->blobs.len - had;
			a->any = w->anysets = true;
			a->kinds |= SETS_BLOOM;
			a->off[w->nsb + 1] = a->blobs.len;
		}
	}
	if (filter)
		pfree(filter);
	if (keys)
		pfree(keys);
}

/* The sorted run of each leaf that asks for one, from the keys of the unit just encoded */
static void
add_runs(SiftColSegWriter *w, const SiftCPaggCol *cols, const SiftKey *addr, Size unit_bytes)
{
	Size		left = unit_bytes / 100 * SIFT_CSET_PRES_PCT;	/* the default runs of a table share this, as the key lists do */

	for (int c = 0; w->run && c < w->schema->ncols; c++)
	{
		SetAcc	   *a = &w->run[c];

		if (a->blobs.data == NULL)
			continue;
		a->off[w->nsb] = a->blobs.len;
		if (sift_crun_build(&w->schema->cols[c], &cols[c], addr, !w->keyed && w->setting >= SIFT_COL_FORMAT_PRESENCE, &a->blobs))
		{
			Size		len = a->blobs.len - a->off[w->nsb];

			if (w->pagg.table && len > left)
				a->blobs.len = a->off[w->nsb];
			else
			{
				left -= Min(len, left);
				a->any = w->anyruns = true;
				w->anyheads |= a->blobs.data[a->off[w->nsb] + 2] != 0;
				a->kinds |= a->blobs.data[a->off[w->nsb]] == SIFT_CRUN_ORD ? RUNS_ORD : RUNS_IDENT;
			}
		}
		a->off[w->nsb + 1] = a->blobs.len;
	}
}

/* The sketch of each leaf that asks for one, from the unit just encoded */
static void
add_hll(SiftColSegWriter *w, const SiftCPaggCol *cols)
{
	for (int c = 0; w->hll && c < w->schema->ncols; c++)
	{
		SetAcc	   *a = &w->hll[c];

		if (a->blobs.data == NULL)
			continue;
		a->off[w->nsb] = a->blobs.len;
		if (sift_hll_build(&w->schema->cols[c], &cols[c], w->dict[c], &a->blobs))
		{
			a->any = w->anyhll = true;
			a->kinds |= a->blobs.data[a->off[w->nsb]];
		}
		a->off[w->nsb + 1] = a->blobs.len;
	}
}

/*
 * The exceptions of the unit (docs/COL-PREAGG.md 11): when one INT leaf is NULL in nearly every row (a soft-delete stamp), the cube and the element rollups of the few
 * rows where it is not, with the dims and measures of the unit's own blobs (cube bytes from cubeat, element bytes from ecubeat). `leaf IS NULL` over the unit is then
 * its aggregates minus these. The blob starts with the leaf; a unit with none has an empty one (sift_cpagg_excepts).
 */
static void
add_excepts(SiftColSegWriter *w, const SiftColBatch *b, const SiftCPaggCol *cols, Size cubeat, Size ecubeat)
{
	int			ncols = w->schema->ncols;
	const uint32 **eoff = palloc0(ncols * sizeof(uint32 *));

	w->xcubeoff[w->nsb] = w->xcube.len;
	w->xecubeoff[w->nsb] = w->xecube.len;
	for (int c = 0; c < ncols; c++)
		if (w->schema->cols[c].parent >= 0)
			eoff[c] = b->col[w->schema->cols[c].parent].eoff;
	if (w->excepts &&
		sift_cpagg_excepts(w->schema, &w->pagg, cols, eoff, b->nrows, -1, (const uint8 *) w->cube.data + cubeat, w->cube.len - cubeat,
						   (const uint8 *) w->ecube.data + ecubeat, w->ecube.len - ecubeat, &w->xcube, &w->xecube))
		w->anyexc = true;
	w->xcubeoff[w->nsb + 1] = w->xcube.len;
	w->xecubeoff[w->nsb + 1] = w->xecube.len;
	pfree(eoff);
}

/*
 * The pre-aggregates of the unit just encoded (docs/COL-PREAGG.md): the exact sums of its INT leaves and the cube blob, from the batch's
 * columns with the dictionary leaves in the segment's codes (what a reader of the unit sees).
 */
void
segw_add_preagg(SiftColSegWriter *w, const SiftColBatch *b, Size unit_bytes)
{
	int			ncols = w->schema->ncols;
	SiftCPaggCol *cols = palloc(ncols * sizeof(SiftCPaggCol));
	bool	   *dim = palloc0(ncols * sizeof(bool));
	int64	  **tk = palloc0(ncols * sizeof(int64 *));
	SiftCLeafAgg *la = w->lagg + (Size) w->nsb * ncols;
	Size		cubeat,
				ecubeat;

	for (int d = 0; d < w->pagg.ndims; d++)
		dim[w->pagg.dim[d].leaf] = true;
	for (int r = 0; r < w->pagg.nrollups; r++)
		for (int d = 0; d < w->pagg.rollup[r].ndims; d++)
			dim[w->pagg.rollup[r].dim[d]] = true;
	for (int c = 0; c < ncols; c++)
	{
		const SiftCVals *v = &b->col[c];
		const SiftColDef *def = &w->schema->cols[c];

		cols[c] = (SiftCPaggCol) {v->key, v->nulls, v->ovf, v->n, def->store != SIFT_COLS_VAR && !w->inl[c]};
		if (def->store == SIFT_COLS_STR && (dim[c] || (w->set && w->set[c].blobs.data) || (w->hll && w->hll[c].blobs.data) || (w->pt && (w->pt[c].want || w->pt[c].ord))) && !w->inl[c])
		{
			tk[c] = palloc(Max(v->n, 1) * sizeof(int64));
			for (uint32 i = 0; i < v->n; i++)
				tk[c][i] = (v->nulls && (v->nulls[i / 64] >> (i % 64) & 1)) ? 0 : w->dictmap[c][v->key[i]];
			cols[c].key = tk[c];
		}
		if (def->store == SIFT_COLS_INT)
			sift_cpagg_leaf(&cols[c], &la[c]);
	}
	w->cubeoff[w->nsb] = cubeat = w->cube.len;
	w->ecubeoff[w->nsb] = ecubeat = w->ecube.len;
	/* a key-cut unit holds fewer rows than a superblock: its cube may take what the cube of the unit of that many rows would. With less, the dims that fit differ from unit to unit and the groups of units (superunits) keep none */
	sift_cpagg_cube(w->schema, &w->pagg, cols, b->nrows, w->keyed && b->nrows < w->aim_rows ? (Size) ((uint64) unit_bytes * w->aim_rows / b->nrows) : unit_bytes, &w->cube, w->format >= PAGG_ECUBE_MINOR ? &w->ecube : NULL);
	w->cubeoff[w->nsb + 1] = w->cube.len;
	w->ecubeoff[w->nsb + 1] = w->ecube.len;
	w->anymeaslen |= w->pagg.measlen && w->ecube.len > ecubeat;
	add_excepts(w, b, cols, cubeat, ecubeat);
	add_sets(w, cols, unit_bytes);
	add_runs(w, cols, b->keys, unit_bytes);
	add_hll(w, cols);
	if (w->kt)
		sift_ktab_add(w->kt, cols, b->nrows);
	add_points(w, b, cols);
	for (int c = 0; c < ncols; c++)
		if (tk[c])
			pfree(tk[c]);
	pfree(tk);
	pfree(dim);
	pfree(cols);
}

/* Keeps the version vectors of the unit just encoded (the batch's, when it has any unfrozen row) and folds the range of the xids */
void
segw_add_vers(SiftColSegWriter *w, const SiftColBatch *b)
{
	VersUnit   *vu = &w->vunit[w->nsb];
	bool		first = true;

	if (!b->hasver)
		return;
	for (uint32 i = 0; i < b->nrows; i++)
	{
		uint32		x = b->xmin[i];

		if (x == 0)
		{
			vu->flags |= VU_FROZEN;
			continue;
		}
		if (first || sift_cver_xid_precedes(x, vu->xmin_lo))
			vu->xmin_lo = x;
		if (first || sift_cver_xid_precedes(vu->xmin_hi, x))
			vu->xmin_hi = x;
		vu->cmin_hi = Max(vu->cmin_hi, b->cmin[i]);
		first = false;
	}
	if (first)
		return;
	vu->flags |= VU_VEC;
	vu->off = w->vblob.len;
	sift_cver_encode(b->xmin, b->nrows, &w->vblob);
	vu->xlen = w->vblob.len - vu->off;
	sift_cver_encode(b->cmin, b->nrows, &w->vblob);
	vu->clen = w->vblob.len - vu->off - vu->xlen;
	if (!w->anyver || sift_cver_xid_precedes(vu->xmin_lo, w->vmin))
		w->vmin = vu->xmin_lo;
	if (!w->anyver || sift_cver_xid_precedes(w->vmax, vu->xmin_hi))
		w->vmax = vu->xmin_hi;
	w->anyver = true;
}
