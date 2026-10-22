/*
 * col/seg_write.c - the column segment writer: units, dictionaries, zones, pre-aggregates and version section into the stream (col/seg.h).
 */

#include "core/host.h"

#include "col/preagg.h"
#include "col/seg.h"
#include "col/seg_int.h"
#include "col/ver.h"
#include "core/live.h"

struct SiftColSegWriter
{
	SiftWriteTarget *wt;
	uint64		gen;
	const SiftColSchema *schema;
	const SiftCcPolicy *pol;	/* [ncols] compression policy per leaf, NULL: plain chunks */
	MemoryContext mcxt;
	SiftStreamWriter *sw;
	SiftCDict **dict;			/* [ncols], STR leaves */
	uint32	  **dictmap;		/* [ncols] scratch: batch code -> segment code */
	bool	   *inl;			/* [ncols] scratch: the STR leaf is stored inline in the unit being added */
	SiftCZone  *zones;			/* [unit * ncols + col] */
	SiftCPaggSpec pagg;			/* the pre-aggregates to write (docs/COL-PREAGG.md) */
	SiftCLeafAgg *lagg;			/* [unit * ncols + col] exact sums of the INT leaves */
	uint64	   *cubeoff;		/* [unit]: offset of the unit's cube blob in cube; [nsb] the end */
	StringInfoData cube;		/* the cube blobs of all units */
	uint64	   *ecubeoff;		/* the same for the element blobs (minor 4) */
	StringInfoData ecube;
	SiftColSBRef *sbdir;
	uint32		nsb;
	uint32		capsb;
	uint64		rows;
	uint32		sb_lo;
	uint32		sb_hi;
	uint8		format;			/* SiftSegEntry.format: what the writer was begun with (a segment with a version section needs SIFT_COL_FORMAT_VERS, the sift_table sees to it); 4 writes the tags of overflow values */
	StringInfoData buf;
	/* the version section: one VersUnit per unit, the vectors of the units, the range of the xids */
	VersUnit   *vunit;
	StringInfoData vblob;
	bool		anyver;
	uint32		vmin;
	uint32		vmax;
};

static void
sw_pad8(SiftStreamWriter *sw)
{
	static const char zeros[8] = {0};

	sift_sw_write(sw, zeros, ALIGN8(sift_sw_tell(sw)) - sift_sw_tell(sw));
}

int
sift_col_write_minor(const SiftColSchema *schema, int setting, bool kinds_exact)
{
	int			minor = setting;

	if (minor >= SIFT_COL_FORMAT_KINDS)
		minor = schema->kinds && kinds_exact ? SIFT_COL_FORMAT_KINDS : SIFT_COL_FORMAT_TAGS;
	if (schema->wide)
		minor = Max(minor, SIFT_COL_FORMAT_WIDE);	/* a wide schema has no older form */
	return minor;
}

SiftColSegWriter *
sift_colw_begin(SiftWriteTarget *wt, uint64 gen, const SiftColSchema *schema, const SiftCcPolicy *pol, int format, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColSegWriter *w = palloc0(sizeof(SiftColSegWriter));

	w->wt = wt;
	w->gen = gen;
	w->schema = schema;
	w->pol = pol;
	w->mcxt = mcxt;
	w->format = sift_col_write_minor(schema, format, true);
	w->sw = sift_sw_begin(wt, gen);
	w->dict = palloc0(schema->ncols * sizeof(SiftCDict *));
	w->dictmap = palloc0(schema->ncols * sizeof(uint32 *));
	w->inl = palloc0(schema->ncols * sizeof(bool));
	for (int c = 0; c < schema->ncols; c++)
		if (schema->cols[c].store == SIFT_COLS_STR)
			w->dict[c] = sift_cdict_create(mcxt);
	w->capsb = 64;
	w->vunit = palloc0(w->capsb * sizeof(VersUnit));
	initStringInfo(&w->vblob);
	w->zones = palloc(w->capsb * schema->ncols * sizeof(SiftCZone));
	w->lagg = palloc(w->capsb * schema->ncols * sizeof(SiftCLeafAgg));
	w->cubeoff = palloc((w->capsb + 1) * sizeof(uint64));
	w->ecubeoff = palloc((w->capsb + 1) * sizeof(uint64));
	sift_cpagg_spec_default(schema, &w->pagg);
	initStringInfo(&w->cube);
	initStringInfo(&w->ecube);
	w->sbdir = palloc(w->capsb * sizeof(SiftColSBRef));
	initStringInfo(&w->buf);
	MemoryContextSwitchTo(old);
	return w;
}

/* Interns the batch dictionary of each STR leaf not stored inline into the segment's: dictmap[col][batch code] = segment code */
static void
map_dicts(SiftColSegWriter *w, const SiftColBatch *b, const bool *inl)
{
	for (int c = 0; c < w->schema->ncols; c++)
	{
		const SiftCDict *bd = b->dict[c];

		if (w->dict[c] == NULL || inl[c])
			continue;
		w->dictmap[c] = w->dictmap[c] ? repalloc(w->dictmap[c], Max(bd->n, 1) * sizeof(uint32)) :
			MemoryContextAlloc(w->mcxt, Max(bd->n, 1) * sizeof(uint32));
		for (uint32 i = 0; i < bd->n; i++)
			w->dictmap[c][i] = sift_cdict_intern(w->dict[c], bd->bytes + bd->off[i], bd->off[i + 1] - bd->off[i]);
	}
}

void
sift_colw_set_preagg(SiftColSegWriter *w, const SiftCPaggSpec *spec)
{
	w->pagg = *spec;
}

/*
 * The pre-aggregates of the unit just encoded (docs/COL-PREAGG.md): the exact sums of its INT leaves and the cube blob, from the batch's
 * columns with the dictionary leaves in the segment's codes (what a reader of the unit sees).
 */
static void
add_preagg(SiftColSegWriter *w, const SiftColBatch *b, Size unit_bytes)
{
	int			ncols = w->schema->ncols;
	SiftCPaggCol *cols = palloc(ncols * sizeof(SiftCPaggCol));
	bool	   *dim = palloc0(ncols * sizeof(bool));
	int64	  **tk = palloc0(ncols * sizeof(int64 *));
	SiftCLeafAgg *la = w->lagg + (Size) w->nsb * ncols;

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
		if (def->store == SIFT_COLS_STR && dim[c] && !w->inl[c])
		{
			tk[c] = palloc(Max(v->n, 1) * sizeof(int64));
			for (uint32 i = 0; i < v->n; i++)
				tk[c][i] = (v->nulls && (v->nulls[i / 64] >> (i % 64) & 1)) ? 0 : w->dictmap[c][v->key[i]];
			cols[c].key = tk[c];
		}
		if (def->store == SIFT_COLS_INT)
			sift_cpagg_leaf(&cols[c], &la[c]);
	}
	w->cubeoff[w->nsb] = w->cube.len;
	w->ecubeoff[w->nsb] = w->ecube.len;
	sift_cpagg_cube(w->schema, &w->pagg, cols, b->nrows, unit_bytes, &w->cube, w->format >= PAGG_ECUBE_MINOR ? &w->ecube : NULL);
	w->cubeoff[w->nsb + 1] = w->cube.len;
	w->ecubeoff[w->nsb + 1] = w->ecube.len;
	for (int c = 0; c < ncols; c++)
		if (tk[c])
			pfree(tk[c]);
	pfree(tk);
	pfree(dim);
	pfree(cols);
}

/* Keeps the version vectors of the unit just encoded (the batch's, when it has any unfrozen row) and folds the range of the xids */
static void
add_vers(SiftColSegWriter *w, const SiftColBatch *b)
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

void
sift_colw_add(SiftColSegWriter *w, const SiftColBatch *b)
{
	int			ncols = w->schema->ncols;
	Size		start;
	uint64		off;

	if (!sift_cbatch_kinds_exact(b))
		w->format = sift_col_write_minor(w->schema, Min(w->format, SIFT_COL_FORMAT_TAGS), false);
	Assert(b->nrows > 0 && b->schema->ncols == ncols);
	if (w->nsb > 0 && b->sb <= w->sbdir[w->nsb - 1].sb)
		elog(ERROR, "sift_col: superblock %u added after %u", b->sb, w->sbdir[w->nsb - 1].sb);
	if (w->nsb == w->capsb)
	{
		w->capsb *= 2;
		w->zones = repalloc_huge(w->zones, (Size) w->capsb * ncols * sizeof(SiftCZone));
		w->lagg = repalloc_huge(w->lagg, (Size) w->capsb * ncols * sizeof(SiftCLeafAgg));
		w->cubeoff = repalloc_huge(w->cubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		w->ecubeoff = repalloc_huge(w->ecubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		w->sbdir = repalloc_huge(w->sbdir, w->capsb * sizeof(SiftColSBRef));
		w->vunit = repalloc_huge(w->vunit, w->capsb * sizeof(VersUnit));
		memset(w->vunit + w->capsb / 2, 0, w->capsb / 2 * sizeof(VersUnit));
	}
	for (int c = 0; c < ncols; c++)
		w->inl[c] = w->dict[c] != NULL && sift_cbatch_str_inline(b, c, w->dict[c], SIFT_COL_DICT_MAX_BYTES);
	map_dicts(w, b, w->inl);
	resetStringInfo(&w->buf);
	sift_cunit_encode(b, w->dictmap, w->inl, w->pol, w->format >= 4, &w->buf, &start, w->zones + (Size) w->nsb * ncols);
	add_preagg(w, b, w->buf.len - start);
	add_vers(w, b);
	sw_pad8(w->sw);
	off = sift_sw_tell(w->sw);
	sift_sw_write(w->sw, w->buf.data + start, w->buf.len - start);
	w->sbdir[w->nsb] = (SiftColSBRef) {b->sb, b->nrows, off, w->rows, w->buf.len - start, 0};
	if (w->nsb == 0)
		w->sb_lo = b->sb;
	w->sb_hi = b->sb;
	w->nsb++;
	w->rows += b->nrows;
}

uint64
sift_colw_rows(const SiftColSegWriter *w)
{
	return w->rows;
}

Size
sift_colw_memory(const SiftColSegWriter *w)
{
	Size		m = (Size) w->nsb * (w->schema->ncols * (sizeof(SiftCZone) + sizeof(SiftCLeafAgg)) + sizeof(SiftColSBRef) + 2 * sizeof(uint64)) + w->cube.len + w->ecube.len;

	for (int c = 0; c < w->schema->ncols; c++)
		if (w->dict[c])
			m += w->dict[c]->nbytes + (Size) w->dict[c]->n * 3 * sizeof(uint32);
	return m;
}

/* Appends len bytes of buf at the 8-aligned stream position; returns the offset */
static uint64
put8(SiftColSegWriter *w, const void *data, Size len)
{
	uint64		off;

	sw_pad8(w->sw);
	off = sift_sw_tell(w->sw);
	sift_sw_write(w->sw, data, len);
	return off;
}

static void
write_dicts(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	uint64	   *offs = palloc0(sizeof(uint32) * 2 + ncols * sizeof(uint64));
	uint32	   *hdr = (uint32 *) offs;
	uint64	   *tab = (uint64 *) (hdr + 2);
	StringInfoData blob;

	sw_pad8(w->sw);
	sec->off = sift_sw_tell(w->sw);
	hdr[0] = DICTS_MAGIC;
	hdr[1] = ncols;
	/* the directory is written last to know the offsets: serialize the dictionaries first into memory */
	initStringInfo(&blob);
	for (int c = 0; c < ncols; c++)
	{
		if (w->dict[c] == NULL)
			continue;
		while (blob.len % 8)
			appendStringInfoChar(&blob, '\0');
		tab[c] = sizeof(uint32) * 2 + ncols * sizeof(uint64) + blob.len;
		sift_cdict_serialize(w->dict[c], w->pol ? &w->pol[c] : NULL, &blob);
	}
	sift_sw_write(w->sw, offs, sizeof(uint32) * 2 + ncols * sizeof(uint64));
	sift_sw_write(w->sw, blob.data, blob.len);
	sw_pad8(w->sw);
	sec->len = sift_sw_tell(w->sw) - sec->off;
	pfree(blob.data);
	pfree(offs);
}

static void
write_pagg(SiftColSegWriter *w)
{
	int			ncols = w->schema->ncols;
	uint32		nsb = w->nsb;
	int			ver = PAGG_VERSION(w->format);
	Size		hsz = PAGG_HDR_SIZE(ver, ncols),
				at = hsz;
	PaggHeader *h = palloc0(hsz);

	h->magic = PAGG_MAGIC;
	h->version = ver;
	h->ncols = ncols;
	h->nsb = nsb;
	for (int c = 0; c < ncols; c++)
		if (w->schema->cols[c].store == SIFT_COLS_INT)
		{
			h->lagg_off[c] = at;
			at += lagg_block_size(nsb);
		}
	if (w->cube.len > 0)
	{
		h->cubedir_off = at;
		h->cube_bytes = w->cube.len;
		at = ALIGN8(at + (nsb + 1) * sizeof(uint64) + w->cube.len);
	}
	if (w->ecube.len > 0)
	{
		PAGG_ECUBEDIR(h) = at;
		PAGG_ECUBE_BYTES(h) = w->ecube.len;
	}
	sift_sw_write(w->sw, h, hsz);
	for (int c = 0; c < ncols; c++)
	{
		uint64	   *blk;
		uint64	   *slo,
				   *shi,
				   *qlo,
				   *qhi;
		uint32	   *fl;

		if (!h->lagg_off[c])
			continue;
		blk = palloc0(lagg_block_size(nsb));
		slo = blk;
		shi = slo + nsb;
		qlo = shi + nsb;
		qhi = qlo + nsb;
		fl = (uint32 *) (qhi + nsb);
		for (uint32 u = 0; u < nsb; u++)
		{
			const SiftCLeafAgg *a = &w->lagg[(Size) u * ncols + c];

			slo[u] = (uint64) a->sum;
			shi[u] = (uint64) (a->sum >> 64);
			qlo[u] = (uint64) a->sumsq;
			qhi[u] = (uint64) (a->sumsq >> 64);
			fl[u] = a->flags;
		}
		sift_sw_write(w->sw, blk, lagg_block_size(nsb));
		pfree(blk);
	}
	if (w->cube.len > 0)
	{
		sift_sw_write(w->sw, w->cubeoff, (Size) (nsb + 1) * sizeof(uint64));
		sift_sw_write(w->sw, w->cube.data, w->cube.len);
	}
	if (w->ecube.len > 0)
	{
		sw_pad8(w->sw);
		sift_sw_write(w->sw, w->ecubeoff, (Size) (nsb + 1) * sizeof(uint64));
		sift_sw_write(w->sw, w->ecube.data, w->ecube.len);
	}
	pfree(h);
}

static void
write_zones(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	Size		dirsz = sizeof(uint32) * 2 + ncols * sizeof(uint64),
				at = ALIGN8(dirsz);
	char	   *dir = palloc0(ALIGN8(dirsz));
	uint32	   *hdr = (uint32 *) dir;
	uint64	   *tab = (uint64 *) (dir + sizeof(uint32) * 2);

	hdr[0] = ZONES_MAGIC;
	hdr[1] = ncols;
	for (int c = 0; c < ncols; c++)
	{
		tab[c] = at;
		at += zone_block_size(&w->schema->cols[c], w->nsb);
	}
	sw_pad8(w->sw);
	sec->off = sift_sw_tell(w->sw);
	sift_sw_write(w->sw, dir, ALIGN8(dirsz));
	for (int c = 0; c < ncols; c++)
	{
		const SiftColDef *def = &w->schema->cols[c];
		Size		sz = zone_block_size(def, w->nsb);
		char	   *blk = palloc0(sz);
		uint64	   *mn = (uint64 *) blk,
				   *mx = mn + w->nsb,
				   *sum = mx + w->nsb;
		uint32	   *nnull = (uint32 *) (sum + w->nsb),
				   *nvals = nnull + w->nsb,
				   *flags = nvals + w->nsb;
		uint64	   *set = (uint64 *) (blk + ALIGN8(w->nsb * (3 * sizeof(uint64) + 3 * sizeof(uint32))));

		for (uint32 u = 0; u < w->nsb; u++)
		{
			const SiftCZone *z = &w->zones[(Size) u * ncols + c];

			mn[u] = z->min;
			mx[u] = z->max;
			if (def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8)
				memcpy(&sum[u], &z->sum_f, sizeof(double));
			else
				sum[u] = z->sum_i;
			nnull[u] = z->nnull;
			nvals[u] = z->nvals;
			/* 3.1 compares the stored flags with the chunk's exactly: the flag of 3.2 is only in a segment of the minor that says the kinds are complete */
			flags[u] = w->format >= SIFT_COL_FORMAT_KINDS ? z->flags : z->flags & ~SIFT_COLZ_JNULL;
			if (def->store == SIFT_COLS_STR)
				memcpy(set + (Size) u * 4, z->set, 4 * sizeof(uint64));
		}
		sift_sw_write(w->sw, blk, sz);
		pfree(blk);
	}
	sw_pad8(w->sw);
	write_pagg(w);
	sw_pad8(w->sw);
	sec->len = sift_sw_tell(w->sw) - sec->off;
	pfree(dir);
}

/* The version section: its header, the directory of the units, the vectors */
static void
write_vers(SiftColSegWriter *w)
{
	VersHeader	h = {.magic = VERS_MAGIC,.version = 1,.nsb = w->nsb,.min_xid = w->vmin,.max_xid = w->vmax,.blob_bytes = w->vblob.len};

	sift_sw_write(w->sw, &h, sizeof(h));
	sift_sw_write(w->sw, w->vunit, (Size) w->nsb * sizeof(VersUnit));
	sift_sw_write(w->sw, w->vblob.data, w->vblob.len);
	sw_pad8(w->sw);
}

void
sift_colw_finish(SiftColSegWriter *w, SiftSegEntry *entry)
{
	PGIOAlignedBlock page;
	SiftSegHeaderData *hdr = (SiftSegHeaderData *) SiftPageGetData(page.data);
	SiftSegHeaderData h = {0};
	StringInfoData schema;
	SiftExtent *ext;
	int			n;
	uint64		blocks;

	if (w->nsb == 0)
		elog(ERROR, "sift_col: a segment needs at least one unit");
	Assert(!w->anyver || w->format >= SIFT_COL_FORMAT_VERS);
	h.sections[SIFT_SEC_POSTINGS] = (SiftSectionRef) {0, sift_sw_tell(w->sw)};
	write_dicts(w, &h.sections[SIFT_SEC_DICT]);
	write_zones(w, &h.sections[SIFT_SEC_DICTIDX]);
	initStringInfo(&schema);
	sift_cschema_serialize(w->schema, &schema);
	sw_pad8(w->sw);
	h.sections[SIFT_SEC_DOCMAP].off = sift_sw_tell(w->sw);
	h.dictroot_off = schema.len;
	sift_sw_write(w->sw, schema.data, schema.len);
	put8(w, w->sbdir, (Size) w->nsb * sizeof(SiftColSBRef));
	sw_pad8(w->sw);
	if (w->anyver)
		write_vers(w);
	h.sections[SIFT_SEC_DOCMAP].len = sift_sw_tell(w->sw) - h.sections[SIFT_SEC_DOCMAP].off;
	ext = sift_sw_finish(w->sw, &n);
	sift_live_write(w->wt, w->gen, w->sb_hi - w->sb_lo + 1, w->rows, &h.live_first, &h.live_npages);
	h.gen = w->gen;
	h.ndocs = w->rows;
	h.nterms = w->schema->ncols;
	h.sb_lo = w->sb_lo;
	h.sb_hi = w->sb_hi;
	h.dictroot_n = w->nsb;
	h.nextents = n;
	sift_page_init(page.data, SIFT_PT_SEGHEADER, w->gen);
	memcpy(hdr, &h, offsetof(SiftSegHeaderData, extents));
	memcpy(hdr->extents, ext, n * sizeof(SiftExtent));
	SiftPageSetUsed(page.data, offsetof(SiftSegHeaderData, extents) + n * sizeof(SiftExtent));
	blocks = 1 + h.live_npages;
	for (int i = 0; i < n; i++)
		blocks += ext[i].nblocks;
	memset(entry, 0, sizeof(SiftSegEntry));
	entry->gen = w->gen;
	entry->kind = SIFT_SEG_IMMUTABLE;
	entry->format = w->format;
	entry->header_blk = sift_wt_write_page(w->wt, page.data);
	entry->sb_lo = w->sb_lo;
	entry->sb_hi = w->sb_hi;
	if (w->anyver)
	{
		entry->flags |= SIFT_SEG_VERS;
		entry->reserved = w->vmin;
	}
	entry->ndocs = w->rows;
	entry->bytes = blocks * BLCKSZ;
	pfree(schema.data);
}
