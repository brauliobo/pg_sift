/*
 * sift_col_seg.c - immutable column segments: writer and reader over the page store (sift_col_seg.h, docs/COL.md 2.1, 2.3, 2.4, 2.7).
 *
 * Stream: UNITS (the unit records, 8-byte aligned), DICTS (a directory, then the serialized dictionary of each STR leaf), ZONES (a
 * directory, then per leaf the zone arrays of all units), then SCHEMA and SBDIR. The header page is a SiftSegHeaderData whose sections
 * are, in order of the stream: POSTINGS = UNITS, DICT = DICTS, DICTIDX = ZONES, DOCMAP = SCHEMA + SBDIR (dictroot_off = the schema's bytes,
 * dictroot_n = the units).
 */
#include "sift_host.h"

#include "sift_col_preagg.h"
#include "sift_col_seg.h"
#include "sift_col_ver.h"
#include "sift_live.h"

#define ULL(x)			((unsigned long long) (x))
#define ALIGN8(x)		TYPEALIGN(8, x)
#define DICTS_MAGIC		0x44444F43		/* "COLD" */
#define ZONES_MAGIC		0x5A4C4F43		/* "COLZ" */
#define PAGG_MAGIC		0x504C4F43		/* "COLP": the pre-aggregates, at the end of the ZONES section (docs/COL-PREAGG.md section 2) */
#define PAGG_ECUBE_MINOR	4			/* the column format minor that adds the element cubes: directory version 2 */
#define PAGG_VERSION(minor)	((minor) >= PAGG_ECUBE_MINOR ? 2 : 1)

#define VERS_MAGIC		0x564C4F43		/* "COLV": the version section, at the end of the DOCMAP section (docs/COL-TAM.md section 7) */

/* The version section: a header, a SiftColVerUnit per unit, then the vectors of the units that have any (xmin, then cmin) */
typedef struct VersHeader
{
	uint32		magic;
	uint32		version;
	uint32		nsb;
	uint32		flags;
	uint32		min_xid;		/* of the unfrozen rows, in the order of the server's xids */
	uint32		max_xid;
	uint64		blob_bytes;
} VersHeader;

#define VU_VEC		0x1			/* the unit has vectors (some row is not frozen) */
#define VU_FROZEN	0x2			/* some rows of such a unit are frozen */

typedef struct VersUnit
{
	uint32		xmin_lo;
	uint32		xmin_hi;
	uint32		cmin_hi;
	uint32		flags;
	uint64		off;			/* of the vectors, from the end of the unit directory */
	uint32		xlen;			/* bytes of the xmin vector; the cmin vector follows */
	uint32		clen;
} VersUnit;

StaticAssertDecl(sizeof(VersHeader) == 32 && sizeof(VersUnit) == 32, "the version section is made of 32 byte records");

#define corrupt(seg, ...) SIFT_SEG_CORRUPT((seg)->index, (seg)->entry.gen, __VA_ARGS__)

/* ---------- writer ---------- */

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
	uint8		format;			/* SiftSegEntry.format: sift.col_format_minor when the writer began (a sift_table writes at least SIFT_COL_FORMAT_VERS); 4 writes the tags of overflow values */
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
	w->format = format;
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

uint64
sift_colw_bytes(const SiftColSegWriter *w)
{
	return sift_sw_tell(w->sw);
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

/* One leaf's zone arrays: min, max, sum (u64 each), nnull, nvals, flags (u32 each, padded to 8), set (STR) */
static Size
zone_block_size(const SiftColDef *def, uint32 nsb)
{
	return ALIGN8(nsb * (3 * sizeof(uint64) + 3 * sizeof(uint32))) + (def->store == SIFT_COLS_STR ? nsb * 4 * sizeof(uint64) : 0);
}

/*
 * The pre-aggregates, at the end of the ZONES section (docs/COL-PREAGG.md section 2): a header (magic, version, ncols, nsb, the offset of the
 * cube directory and its blob bytes, the offset of each INT leaf's sums), then per INT leaf the sums of all units as arrays, then the cube
 * directory (u64 offset of each unit's blob, nsb + 1 entries) and the blobs. Offsets are from the start of the header.
 * Version 2 (column format minor 4) adds two entries after the offsets of the sums, the offset of the element directory and its blob bytes,
 * and after the cubes that directory (8-aligned) and the element blobs, laid out like the cubes.
 */
typedef struct PaggHeader
{
	uint32		magic;
	uint32		version;
	uint32		ncols;
	uint32		nsb;
	uint64		cubedir_off;			/* 0: no cube in any unit */
	uint64		cube_bytes;
	uint64		lagg_off[FLEXIBLE_ARRAY_MEMBER];	/* [ncols], 0: the leaf has none */
} PaggHeader;

#define PAGG_HDR_SIZE(ver, ncols)	ALIGN8(offsetof(PaggHeader, lagg_off) + ((ncols) + ((ver) > 1 ? 2 : 0)) * sizeof(uint64))
#define PAGG_ECUBEDIR(h)		((h)->lagg_off[(h)->ncols])
#define PAGG_ECUBE_BYTES(h)		((h)->lagg_off[(h)->ncols + 1])

/* One INT leaf's sums: sum_lo, sum_hi, sumsq_lo, sumsq_hi (u64 each), flags (u32), all over the units */
static Size
lagg_block_size(uint32 nsb)
{
	return ALIGN8((Size) nsb * (4 * sizeof(uint64) + sizeof(uint32)));
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
			flags[u] = z->flags;
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

/* ---------- reader ---------- */

typedef struct ZoneArrays
{
	const int64 *min;
	const int64 *max;
	const int64 *sum;
	const uint32 *nnull;
	const uint32 *nvals;
	const uint32 *flags;
	const uint64 (*set)[4];
	bool		any_ovf;		/* some unit flags SIFT_COLZ_OVERFLOW */
	bool		any_inl;		/* some unit flags SIFT_COLZ_INLINE: its min and max are prefix keys, not codes */
} ZoneArrays;

typedef struct SegPriv
{
	SiftLive	live;
	SiftCDict **dict;
	ZoneArrays **zones;
	uint8	   *unitbuf;
	Size		unitcap;
	PaggHeader *pagg;			/* the sidecar's header, read on first use */
	Size		pagg_end;		/* offset of the sidecar in the ZONES section */
	uint64	   *cubedir;		/* nsb + 1 */
	uint8	   *cubebuf;
	Size		cubecap;
	uint64	   *ecubedir;		/* the same for the element blobs */
	uint8	   *ecubebuf;
	Size		ecubecap;
	const uint64 **lagg;		/* [ncols] sums of the INT leaves, read on first use: slo, shi, qlo, qhi, then flags */
	Size		vers_at;		/* offset of the version section in the DOCMAP section; vers_len 0: there is none */
	Size		vers_len;
	VersUnit   *vu;				/* its directory of units, read on first use */
	uint64		vblob_at;		/* stream offset of the vectors */
	uint64		vblob_len;
	uint32		vmin;
	uint32		vmax;
} SegPriv;

#define PRIV(seg)	((SegPriv *) (seg)->priv)

static void
seg_read(SiftColSegment *seg, uint64 off, Size len, void *dst)
{
	char	   *p = dst;

	/* in pieces of a page's payload: sift_sr_get copies across pages into scratch only when asked for a contiguous range */
	while (len > 0)
	{
		Size		n = Min(len, SIFT_PAGE_PAYLOAD * 8);
		const void *src = sift_sr_get(&seg->sr, off, n, p);

		if (src != p)
			memcpy(p, src, n);
		p += n;
		off += n;
		len -= n;
	}
	/* a segment that stays open between calls keeps its page pinned, never locked: a lock holds off interrupts and parallel workers */
	sift_sr_unlock(&seg->sr);
}

static const SiftSectionRef *
sec(const SiftColSegment *seg, SiftSection s)
{
	return &seg->hdr->sections[s];
}

SiftColSegment *
sift_cseg_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftColSegment *seg = palloc0(sizeof(SiftColSegment));
	SegPriv    *pv = palloc0(sizeof(SegPriv));
	SiftSegHeaderData *h;
	uint64		size,
				rank = 0;
	char	   *schema;
	const SiftSectionRef *s;

	seg->index = index;
	seg->entry = *entry;
	seg->mcxt = mcxt;
	seg->priv = pv;
	if (entry->kind != SIFT_SEG_IMMUTABLE)
		elog(ERROR, "sift_col: segment %llu is not immutable", ULL(entry->gen));
	if (entry->format < SIFT_COL_FORMAT_MIN_READ || entry->format > SIFT_COL_FORMAT_MINOR)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" has a segment of column format 1.%d, this build reads 1.%d to 1.%d",
						RelationGetRelationName(index), entry->format, SIFT_COL_FORMAT_MIN_READ, SIFT_COL_FORMAT_MINOR),
				 errhint(entry->format > SIFT_COL_FORMAT_MINOR ? "Use a newer pg_sift, or REINDEX." : "REINDEX; segments of a pre-release column format are not read.")));
	seg->hdr = sift_seg_read_header(index, entry);
	h = seg->hdr;
	if (h->gen != entry->gen || h->ndocs != entry->ndocs || h->ndocs == 0 || h->sb_lo != entry->sb_lo ||
		h->sb_hi != entry->sb_hi || h->sb_lo > h->sb_hi || h->live_npages == 0 || h->dictroot_n == 0 ||
		h->dictroot_n > h->sb_hi - h->sb_lo + 1 || h->nterms == 0 || h->nterms > SIFT_COL_MAX_COLS)
		corrupt(seg, "Header (gen %llu, %llu rows, SBs %u-%u, %u units) does not match its directory entry.",
				ULL(h->gen), ULL(h->ndocs), h->sb_lo, h->sb_hi, h->dictroot_n);
	sift_sr_init(&seg->sr, index, h->extents, h->nextents, entry->gen);
	size = (uint64) seg->sr.npages * SIFT_PAGE_PAYLOAD;
	s = h->sections;
	if (s[SIFT_SEC_POSTINGS].off != 0 || s[SIFT_SEC_DICT].off != s[SIFT_SEC_POSTINGS].len ||
		s[SIFT_SEC_DICTIDX].off != s[SIFT_SEC_DICT].off + s[SIFT_SEC_DICT].len ||
		s[SIFT_SEC_DOCMAP].off != s[SIFT_SEC_DICTIDX].off + s[SIFT_SEC_DICTIDX].len ||
		s[SIFT_SEC_DOCMAP].off + s[SIFT_SEC_DOCMAP].len > size ||
		h->dictroot_off + (uint64) h->dictroot_n * sizeof(SiftColSBRef) > s[SIFT_SEC_DOCMAP].len)
		corrupt(seg, "Sections do not tile the %llu-byte stream.", ULL(size));
	schema = palloc(h->dictroot_off);
	seg_read(seg, s[SIFT_SEC_DOCMAP].off, h->dictroot_off, schema);
	seg->schema = sift_cschema_parse(mcxt, schema, h->dictroot_off);
	if (seg->schema == NULL || seg->schema->ncols != (int) h->nterms)
		corrupt(seg, "The schema section is not a schema of %llu leaves.", ULL(h->nterms));
	pfree(schema);
	seg->nsb = h->dictroot_n;
	{
		SiftColSBRef *dir = palloc((Size) seg->nsb * sizeof(SiftColSBRef));

		seg_read(seg, s[SIFT_SEC_DOCMAP].off + ALIGN8(h->dictroot_off), (Size) seg->nsb * sizeof(SiftColSBRef), dir);
		for (uint32 i = 0; i < seg->nsb; i++)
		{
			const SiftColSBRef *r = &dir[i];

			if (r->sb < h->sb_lo || r->sb > h->sb_hi || (i > 0 && r->sb <= dir[i - 1].sb) || r->off % 8 != 0 ||
				r->nrows == 0 || r->nrows > SIFT_COL_UNIT_MAX_ROWS || r->rank_base != rank ||
				r->off + r->len > s[SIFT_SEC_POSTINGS].len || (i > 0 && r->off < dir[i - 1].off + dir[i - 1].len))
				corrupt(seg, "SBDIR entry %u (SB %u at %llu, %u rows, rank %llu) is out of order.", i, r->sb,
						ULL(r->off), r->nrows, ULL(r->rank_base));
			rank += r->nrows;
		}
		if (rank != h->ndocs)
			corrupt(seg, "SBDIR lists %llu rows, the header %llu.", ULL(rank), ULL(h->ndocs));
		seg->sbdir = dir;
	}
	pv->vers_at = ALIGN8(h->dictroot_off) + (Size) seg->nsb * sizeof(SiftColSBRef);
	if (pv->vers_at > s[SIFT_SEC_DOCMAP].len)
		corrupt(seg, "The DOCMAP section ends inside the unit directory.");
	pv->vers_len = s[SIFT_SEC_DOCMAP].len - pv->vers_at;
	if (pv->vers_len > 0 && (entry->format < SIFT_COL_FORMAT_VERS || pv->vers_len < sizeof(VersHeader) + (Size) seg->nsb * sizeof(VersUnit)))
		corrupt(seg, "The DOCMAP section holds %zu bytes after the unit directory, which is no version section of %u units (format 1.%d).",
				pv->vers_len, seg->nsb, entry->format);
	if ((entry->flags & SIFT_SEG_VERS) && pv->vers_len == 0)
		corrupt(seg, "The directory entry says the segment has unfrozen rows but it has no version section.");
	pv->live =(SiftLive) {index, entry->gen, h->live_first, h->live_npages, h->sb_hi - h->sb_lo + 1, h->ndocs};
	sift_live_check(&pv->live);
	pv->dict = palloc0(h->nterms * sizeof(SiftCDict *));
	pv->zones = palloc0(h->nterms * sizeof(ZoneArrays *));
	MemoryContextSwitchTo(old);
	return seg;
}

void
sift_cseg_close(SiftColSegment *seg)
{
	sift_sr_release(&seg->sr);
}

uint32
sift_cseg_find_unit(const SiftColSegment *seg, uint32 sb)
{
	uint32		lo = 0,
				hi = seg->nsb;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (seg->sbdir[mid].sb < sb)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

const uint8 *
sift_cseg_unit(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const SiftColSBRef *r = &seg->sbdir[u];

	if (r->len > pv->unitcap)
	{
		pv->unitcap = Max(r->len, 2 * pv->unitcap);
		pv->unitbuf = pv->unitbuf ? repalloc_huge(pv->unitbuf, pv->unitcap) : MemoryContextAlloc(seg->mcxt, pv->unitcap);
	}
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + r->off, r->len, pv->unitbuf);
	*len = r->len;
	return pv->unitbuf;
}

const uint8 *
sift_cseg_unit_head(SiftColSegment *seg, uint32 u, uint32 *len, uint32 *avail)
{
	SegPriv    *pv = PRIV(seg);
	const SiftColSBRef *r = &seg->sbdir[u];

	if (r->len > pv->unitcap)
	{
		pv->unitcap = Max(r->len, 2 * pv->unitcap);
		pv->unitbuf = pv->unitbuf ? repalloc_huge(pv->unitbuf, pv->unitcap) : MemoryContextAlloc(seg->mcxt, pv->unitcap);
	}
	*len = r->len;
	*avail = Min(r->len, SIFT_COL_UNIT_HEAD);
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + r->off, *avail, pv->unitbuf);
	return pv->unitbuf;
}

void
sift_cseg_unit_fill(SiftColSegment *seg, uint32 u, uint32 off, uint32 len)
{
	SegPriv    *pv = PRIV(seg);

	Assert((uint64) off + len <= seg->sbdir[u].len);
	seg_read(seg, sec(seg, SIFT_SEC_POSTINGS)->off + seg->sbdir[u].off + off, len, pv->unitbuf + off);
}

/* Where the dictionary of leaf col lies in the stream: offset and length (up to the next dictionary or the end of the section) */
static void
dict_region(SiftColSegment *seg, int col, uint64 *off, uint64 *len)
{
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICT);
	uint32		hdr[2];
	uint64		next = s->len,
				tab[SIFT_COL_MAX_COLS];

	seg_read(seg, s->off, sizeof(hdr), hdr);
	if (hdr[0] != DICTS_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
		corrupt(seg, "DICTS section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
	seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
	*off = tab[col];
	for (int c = 0; c < seg->schema->ncols; c++)
		if (tab[c] > *off && tab[c] < next)
			next = tab[c];
	if (*off == 0 || *off >= s->len)
		corrupt(seg, "Leaf %d has no dictionary.", col);
	*len = next - *off;
}

uint64
sift_cseg_dict_stored(SiftColSegment *seg, int col)
{
	uint64		off,
				len;

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return 0;
	dict_region(seg, col, &off, &len);
	return len;
}

SiftCDict *
sift_cseg_dict(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);

	if (seg->schema->cols[col].store != SIFT_COLS_STR)
		return NULL;
	if (pv->dict[col] == NULL)
	{
		uint64		off,
					len;
		char	   *data;
		Size		used;

		dict_region(seg, col, &off, &len);
		data = palloc(len);
		seg_read(seg, sec(seg, SIFT_SEC_DICT)->off + off, len, data);
		pv->dict[col] = sift_cdict_parse(seg->mcxt, data, len, &used);
		if (pv->dict[col] == NULL)
			corrupt(seg, "The dictionary of leaf %d is malformed.", col);
		pfree(data);
	}
	return pv->dict[col];
}

static const ZoneArrays *
zone_arrays(SiftColSegment *seg, int col)
{
	SegPriv    *pv = PRIV(seg);
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICTIDX);

	if (pv->zones[col] == NULL)
	{
		const SiftColDef *def = &seg->schema->cols[col];
		uint32		hdr[2];
		uint64		tab[SIFT_COL_MAX_COLS],
					off;
		Size		sz = zone_block_size(def, seg->nsb);
		char	   *blk;
		ZoneArrays *z = MemoryContextAllocZero(seg->mcxt, sizeof(ZoneArrays));

		seg_read(seg, s->off, sizeof(hdr), hdr);
		if (hdr[0] != ZONES_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
			corrupt(seg, "ZONES section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
		seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
		off = tab[col];
		if (off % 8 != 0 || off + sz > s->len)
			corrupt(seg, "The zones of leaf %d (%llu, %zu bytes) lie outside the section.", col, ULL(off), sz);
		blk = MemoryContextAlloc(seg->mcxt, sz);
		seg_read(seg, s->off + off, sz, blk);
		z->min = (const int64 *) blk;
		z->max = z->min + seg->nsb;
		z->sum = z->max + seg->nsb;
		z->nnull = (const uint32 *) (z->sum + seg->nsb);
		z->nvals = z->nnull + seg->nsb;
		z->flags = z->nvals + seg->nsb;
		if (def->store == SIFT_COLS_STR)
			z->set = (const uint64 (*)[4]) (blk + ALIGN8(seg->nsb * (3 * sizeof(uint64) + 3 * sizeof(uint32))));
		for (uint32 u = 0; u < seg->nsb; u++)
			z->any_ovf |= (z->flags[u] & SIFT_COLZ_OVERFLOW) != 0, z->any_inl |= (z->flags[u] & SIFT_COLZ_INLINE) != 0;
		pv->zones[col] = z;
	}
	return pv->zones[col];
}

void
sift_cseg_zone(SiftColSegment *seg, int col, uint32 u, SiftCZone *out)
{
	const ZoneArrays *z = zone_arrays(seg, col);
	const SiftColDef *def = &seg->schema->cols[col];

	memset(out, 0, sizeof(SiftCZone));
	out->min = z->min[u];
	out->max = z->max[u];
	if (def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8)
		memcpy(&out->sum_f, &z->sum[u], sizeof(double));
	else
		out->sum_i = z->sum[u];
	out->nnull = z->nnull[u];
	out->nvals = z->nvals[u];
	out->flags = z->flags[u];
	if (z->set)
		memcpy(out->set, z->set[u], 4 * sizeof(uint64));
}

/* The entries of the segment's dictionary of leaf col: the count in its header (after the form), without parsing the dictionary */
static uint32
dict_count(SiftColSegment *seg, int col)
{
	const SiftSectionRef *s = sec(seg, SIFT_SEC_DICT);
	uint32		hdr[2],
				n;
	uint64		tab[SIFT_COL_MAX_COLS];

	seg_read(seg, s->off, sizeof(hdr), hdr);
	if (hdr[0] != DICTS_MAGIC || hdr[1] != (uint32) seg->schema->ncols)
		corrupt(seg, "DICTS section starts with 0x%08x, %u leaves.", hdr[0], hdr[1]);
	seg_read(seg, s->off + sizeof(hdr), seg->schema->ncols * sizeof(uint64), tab);
	if (tab[col] == 0 || tab[col] + 2 * sizeof(n) > s->len)
		corrupt(seg, "Leaf %d has no dictionary.", col);
	seg_read(seg, s->off + tab[col] + sizeof(n), sizeof(n), &n);
	return n;
}

void
sift_col_leaf_stats_init(SiftColLeafStats *acc)
{
	memset(acc, 0, sizeof(*acc));
	acc->min = PG_INT64_MAX;
	acc->max = PG_INT64_MIN;
	acc->has_range = acc->has_sum = true;
}

void
sift_cseg_leaf_stats(SiftColSegment *seg, int col, SiftColLeafStats *acc)
{
	const SiftColDef *def = &seg->schema->cols[col];
	const ZoneArrays *z = zone_arrays(seg, col);

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		bool		any = z->nnull[u] < z->nvals[u];

		acc->nvals += z->nvals[u];
		acc->nnull += z->nnull[u];
		if (z->flags[u] & SIFT_COLZ_OVERFLOW)
		{
			acc->has_range = acc->has_sum = false;
			acc->has_ovf = true;
		}
		if (def->kind == SIFT_COLK_GROUP)
			acc->has_sum &= !(z->flags[u] & SIFT_COLZ_NOSUM);
		else if (def->store == SIFT_COLS_INT && any)
		{
			acc->min = Min(acc->min, z->min[u]);
			acc->max = Max(acc->max, z->max[u]);
		}
		else if (def->store != SIFT_COLS_INT)
			acc->has_range = false;
		if (def->kind == SIFT_COLK_GROUP)
			acc->sum += (double) z->sum[u];
	}
	if (def->store == SIFT_COLS_STR)
	{
		acc->inl |= z->any_inl;
		acc->dict_n = Max(acc->dict_n, dict_count(seg, col));
	}
}

/* ---------- pre-aggregates (docs/COL-PREAGG.md) ---------- */

/* Where the sidecar starts: after the zone blocks, which are laid out back to back */
static Size
zones_end(const SiftColSegment *seg)
{
	Size		at = ALIGN8(sizeof(uint32) * 2 + seg->schema->ncols * sizeof(uint64));

	for (int c = 0; c < seg->schema->ncols; c++)
		at += zone_block_size(&seg->schema->cols[c], seg->nsb);
	return at;
}

static const PaggHeader *
pagg_header(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (pv->pagg == NULL)
	{
		const SiftSectionRef *s = sec(seg, SIFT_SEC_DICTIDX);
		int			ncols = seg->schema->ncols;
		int			ver = PAGG_VERSION(seg->entry.format);
		Size		end = zones_end(seg),
					hsz = PAGG_HDR_SIZE(ver, ncols);
		PaggHeader *h;

		if (end + hsz > s->len)
			corrupt(seg, "The ZONES section holds %llu bytes, no room for the pre-aggregates after %zu.", ULL(s->len), end);
		h = MemoryContextAlloc(seg->mcxt, hsz);
		seg_read(seg, s->off + end, hsz, h);
		if (h->magic != PAGG_MAGIC || h->version != ver || h->ncols != (uint32) ncols || h->nsb != seg->nsb)
			corrupt(seg, "The pre-aggregates start with 0x%08x, version %u, %u leaves, %u units; a segment of column format 1.%d has version %d.",
					h->magic, h->version, h->ncols, h->nsb, seg->entry.format, ver);
		for (int c = 0; c < ncols; c++)
			if (h->lagg_off[c] % 8 != 0 || (h->lagg_off[c] && end + h->lagg_off[c] + lagg_block_size(seg->nsb) > s->len))
				corrupt(seg, "The sums of leaf %d lie outside the section.", c);
		if (h->cubedir_off && (h->cubedir_off % 8 != 0 || end + h->cubedir_off + (seg->nsb + 1) * sizeof(uint64) + h->cube_bytes > s->len))
			corrupt(seg, "The cubes (%llu bytes at %llu) lie outside the section.", ULL(h->cube_bytes), ULL(h->cubedir_off));
		if (ver > 1 && PAGG_ECUBEDIR(h) &&
			(PAGG_ECUBEDIR(h) % 8 != 0 || end + PAGG_ECUBEDIR(h) + (seg->nsb + 1) * sizeof(uint64) + PAGG_ECUBE_BYTES(h) > s->len))
			corrupt(seg, "The element cubes (%llu bytes at %llu) lie outside the section.", ULL(PAGG_ECUBE_BYTES(h)), ULL(PAGG_ECUBEDIR(h)));
		pv->pagg_end = end;
		pv->pagg = h;
		pv->lagg = MemoryContextAllocZero(seg->mcxt, ncols * sizeof(uint64 *));
	}
	return pv->pagg;
}

void
sift_cseg_agg(SiftColSegment *seg, int col, uint32 u, SiftCAgg *a)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = pagg_header(seg);
	SiftCZone	z;

	sift_cseg_zone(seg, col, u, &z);
	memset(a, 0, sizeof(*a));
	a->nvals = z.nvals;
	a->nnull = z.nnull;
	a->min = z.min;
	a->max = z.max;
	a->sum_f = z.sum_f;
	a->zflags = z.flags;
	if (h->lagg_off[col])
	{
		const uint64 *b = pv->lagg[col];

		if (b == NULL)
		{
			uint64	   *blk = MemoryContextAlloc(seg->mcxt, lagg_block_size(seg->nsb));

			seg_read(seg, sec(seg, SIFT_SEC_DICTIDX)->off + pv->pagg_end + h->lagg_off[col], lagg_block_size(seg->nsb), blk);
			pv->lagg[col] = b = blk;
		}
		a->sum = (SiftCI128) (((unsigned __int128) b[seg->nsb + u] << 64) | b[u]);
		a->sumsq = (SiftCI128) (((unsigned __int128) b[3 * seg->nsb + u] << 64) | b[2 * seg->nsb + u]);
		a->flags = ((const uint32 *) (b + 4 * seg->nsb))[u];
		a->has_sums = true;
	}
}

/* The blob of unit u from the directory at dir_off (nsb + 1 offsets, then the blobs) of the sidecar; NULL when the unit has none */
static const uint8 *
unit_blob(SiftColSegment *seg, uint32 u, uint64 dir_off, uint64 bytes, uint64 **dir, uint8 **buf, Size *cap, uint32 *len)
{
	uint64		at = sec(seg, SIFT_SEC_DICTIDX)->off + PRIV(seg)->pagg_end + dir_off,
				b0,
				b1;

	*len = 0;
	if (!dir_off)
		return NULL;
	if (*dir == NULL)
	{
		*dir = MemoryContextAlloc(seg->mcxt, (seg->nsb + 1) * sizeof(uint64));
		seg_read(seg, at, (seg->nsb + 1) * sizeof(uint64), *dir);
		for (uint32 i = 0; i < seg->nsb; i++)
			if ((*dir)[i] > (*dir)[i + 1] || (*dir)[i + 1] > bytes)
				corrupt(seg, "The cube directory is out of order at unit %u.", i);
	}
	b0 = (*dir)[u];
	b1 = (*dir)[u + 1];
	if (b1 == b0)
		return NULL;
	if (b1 - b0 > *cap)
	{
		*cap = Max(b1 - b0, 2 * *cap);
		*buf = *buf ? repalloc(*buf, *cap) : MemoryContextAlloc(seg->mcxt, *cap);
	}
	seg_read(seg, at + (seg->nsb + 1) * sizeof(uint64) + b0, b1 - b0, *buf);
	*len = b1 - b0;
	return *buf;
}

const uint8 *
sift_cseg_cube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = pagg_header(seg);

	return unit_blob(seg, u, h->cubedir_off, h->cube_bytes, &pv->cubedir, &pv->cubebuf, &pv->cubecap, len);
}

const uint8 *
sift_cseg_ecube(SiftColSegment *seg, uint32 u, uint32 *len)
{
	SegPriv    *pv = PRIV(seg);
	const PaggHeader *h = pagg_header(seg);

	if (h->version < 2)
	{
		*len = 0;
		return NULL;
	}
	return unit_blob(seg, u, PAGG_ECUBEDIR(h), PAGG_ECUBE_BYTES(h), &pv->ecubedir, &pv->ecubebuf, &pv->ecubecap, len);
}

const uint8 *
sift_cseg_rollup(SiftColSegment *seg, uint32 u, int list, uint32 *len)
{
	const uint8 *e;
	uint32		elen;

	e = sift_cseg_ecube(seg, u, &elen);
	if (e == NULL)
	{
		*len = 0;
		return NULL;
	}
	e = sift_cpagg_elem_cube(seg->schema, e, elen, list, len);
	if (*len == UINT32_MAX)
		corrupt(seg, "The rollups of unit %u do not decode.", u);
	return e;
}

const uint8 *
sift_cseg_dim_cube(SiftColSegment *seg, uint32 u, int dim, uint32 *len)
{
	int			list = seg->schema->cols[dim].parent;

	return list < 0 ? sift_cseg_cube(seg, u, len) : sift_cseg_rollup(seg, u, list, len);
}

uint64
sift_cseg_preagg_bytes(SiftColSegment *seg)
{
	pagg_header(seg);
	return sec(seg, SIFT_SEC_DICTIDX)->len - PRIV(seg)->pagg_end;
}

uint32
sift_cseg_preagg_units(SiftColSegment *seg)
{
	uint32		n = 0,
				len;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		const uint8 *c = sift_cseg_cube(seg, u, &len);

		n += c != NULL || sift_cseg_ecube(seg, u, &len) != NULL;
	}
	return n;
}

void
sift_cseg_prune(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep)
{
	const ZoneArrays *z = zone_arrays(seg, col);
	uint32		n = seg->nsb,
				nw = SIFT_COL_NWORDS(n);
	uint64	   *tmp = palloc(nw * sizeof(uint64));
	uint64	   *save = NULL;

	if (z->any_ovf || z->any_inl)
	{
		save = palloc(nw * sizeof(uint64));
		memcpy(save, keep, nw * sizeof(uint64));
	}
	switch (t->kind)
	{
		case SIFT_CT_NONE:
			memset(keep, 0, nw * sizeof(uint64));
			break;
		case SIFT_CT_SET:			/* lo..hi bound the keys that can match */
		case SIFT_CT_RANGE:
			sift_colk->zone_range(z->min, z->max, n, t->lo, t->hi, tmp);
			sift_colk->bits_and(keep, tmp, nw);
			sift_colk->zone_notnull(z->nnull, z->nvals, n, keep);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set, n, t->fold, keep);
			break;
		case SIFT_CT_LUT:
			sift_colk->zone_notnull(z->nnull, z->nvals, n, keep);
			if (t->fold_valid && z->set)
				sift_colk->zone_set(z->set, n, t->fold, keep);
			break;
		case SIFT_CT_NONNULL:
		case SIFT_CT_NOTNULL:
		case SIFT_CT_NSET:
		case SIFT_CT_STRS:
			sift_colk->zone_notnull(z->nnull, z->nvals, n, keep);
			break;
		case SIFT_CT_ISNULL:
			for (uint32 u = 0; u < n; u++)
				if (z->nnull[u] == 0)
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			break;
		case SIFT_CT_NRANGE:
			for (uint32 u = 0; u < n; u++)
				if (z->nnull[u] < z->nvals[u] && z->min[u] >= t->lo && z->max[u] <= t->hi)
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
				else if (z->nnull[u] == z->nvals[u])
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			break;
	}
	if (save)
	{
		/* an inline unit has no codes: the passes above said nothing about it, its zone is the strings' */
		for (uint32 u = 0; z->any_inl && u < n; u++)
			if (z->flags[u] & SIFT_COLZ_INLINE)
			{
				SiftCZone	uz = {.min = z->min[u], .max = z->max[u], .nnull = z->nnull[u], .nvals = z->nvals[u], .flags = z->flags[u]};

				if (sift_ctest_zone(t, &uz))
					keep[u / 64] |= save[u / 64] & (UINT64CONST(1) << (u % 64));
				else
					keep[u / 64] &= ~(UINT64CONST(1) << (u % 64));
			}
		/* units with overflow values cannot be pruned by this leaf: their zone does not describe the heap */
		for (uint32 u = 0; z->any_ovf && u < n; u++)
			if (z->flags[u] & SIFT_COLZ_OVERFLOW)
				keep[u / 64] |= save[u / 64] & (UINT64CONST(1) << (u % 64));
		pfree(save);
	}
	pfree(tmp);
}

/* The superblocks the immutable segments of dir cover: the ranges of the segments of a parallel build interleave, so they are not summed */
uint64
sift_cseg_dir_units(const SiftDirSnapshot *dir)
{
	const SiftSegEntry *imm[SIFT_MAX_SEGMENTS];
	int			n = 0;
	uint64		units = 0,
				end = 0;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE)
		{
			int			j = n++;

			for (; j > 0 && imm[j - 1]->sb_lo > dir->segs[i].sb_lo; j--)
				imm[j] = imm[j - 1];
			imm[j] = &dir->segs[i];
		}
	for (int i = 0; i < n; i++)
	{
		uint64		lo = Max(imm[i]->sb_lo, end),
					hi = (uint64) imm[i]->sb_hi + 1;

		if (hi > lo)
		{
			units += hi - lo;
			end = hi;
		}
	}
	return units;
}

/* ---------- liveness ---------- */

/* The dirty bit of the unit's SB, read from the LIVE page now: neither the directory snapshot nor an earlier read may stand for it, because
 * a scan must read liveness after it reads the all-visible mask (docs/COL.md 2.7, "Order of reads") */
static bool
unit_dirty(SiftColSegment *seg, uint32 u)
{
	uint32		i = seg->sbdir[u].sb - seg->hdr->sb_lo;
	Buffer		buf = InvalidBuffer;
	uint8		b = sift_live_byte(&PRIV(seg)->live, sizeof(SiftLiveHeaderData) + i / 8, &buf);

	UnlockReleaseBuffer(buf);
	return (b >> (i % 8)) & 1;
}

bool
sift_cseg_dirty(SiftColSegment *seg, uint32 u)
{
	return unit_dirty(seg, u);
}

void
sift_cseg_live(SiftColSegment *seg, uint32 u, uint64 *bits)
{
	uint32		n = seg->sbdir[u].nrows,
				nw = SIFT_COL_NWORDS(n);

	if (unit_dirty(seg, u))
	{
		sift_live_bits(&PRIV(seg)->live, seg->sbdir[u].rank_base, n, bits, nw);
		return;
	}
	memset(bits, 0, nw * sizeof(uint64));
	memset(bits, 0xFF, n / 64 * sizeof(uint64));
	if (n % 64)
		bits[n / 64] = (UINT64CONST(1) << (n % 64)) - 1;
}

uint64
sift_cseg_live_rows(SiftColSegment *seg)
{
	if (!(seg->entry.flags & SIFT_SEG_DIRTY))
		return seg->hdr->ndocs;
	return seg->hdr->ndocs - sift_live_ndead(&PRIV(seg)->live);
}


/* ---------- versions of the rows ---------- */

/* A group of 3.0.1: one transaction's one command wrote every row (entry.reserved = xid, header sum_dl = cid) */
static bool
vers_tag(const SiftColSegment *seg)
{
	return seg->entry.format < SIFT_COL_FORMAT_VERS && seg->entry.reserved != 0;
}

bool
sift_cseg_vers(const SiftColSegment *seg)
{
	return vers_tag(seg) || ((seg->entry.flags & SIFT_SEG_VERS) && PRIV(seg)->vers_len > 0);
}

static const VersUnit *
vers_units(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);

	if (pv->vu == NULL)
	{
		VersHeader	h;
		uint64		at = sec(seg, SIFT_SEC_DOCMAP)->off + pv->vers_at;
		Size		dirsz = (Size) seg->nsb * sizeof(VersUnit);
		VersUnit   *vu;

		if (pv->vers_len == 0)
			corrupt(seg, "The segment has no version section.");
		seg_read(seg, at, sizeof(h), &h);
		if (h.magic != VERS_MAGIC || h.version != 1 || h.nsb != seg->nsb || sizeof(h) + dirsz + h.blob_bytes > pv->vers_len)
			corrupt(seg, "The version section starts with 0x%08x, version %u, %u units, %llu bytes of vectors.", h.magic, h.version, h.nsb,
					ULL(h.blob_bytes));
		vu = MemoryContextAlloc(seg->mcxt, dirsz);
		seg_read(seg, at + sizeof(h), dirsz, vu);
		for (uint32 u = 0; u < seg->nsb; u++)
			if ((vu[u].flags & ~(VU_VEC | VU_FROZEN)) != 0 || (!(vu[u].flags & VU_VEC) && (vu[u].xlen || vu[u].clen)) ||
				vu[u].off > h.blob_bytes || (uint64) vu[u].xlen + vu[u].clen > h.blob_bytes - vu[u].off)
				corrupt(seg, "The versions of unit %u (flags %u, %u + %u bytes at %llu) lie outside the section.", u, vu[u].flags,
						vu[u].xlen, vu[u].clen, ULL(vu[u].off));
		pv->vblob_at = at + sizeof(h) + dirsz;
		pv->vblob_len = h.blob_bytes;
		pv->vmin = h.min_xid;
		pv->vmax = h.max_xid;
		pv->vu = vu;
	}
	return pv->vu;
}

void
sift_cseg_ver_range(SiftColSegment *seg, uint32 *xmin_lo, uint32 *xmin_hi)
{
	Assert(sift_cseg_vers(seg));
	if (vers_tag(seg))
		*xmin_lo = *xmin_hi = seg->entry.reserved;
	else
	{
		(void) vers_units(seg);
		*xmin_lo = PRIV(seg)->vmin;
		*xmin_hi = PRIV(seg)->vmax;
	}
}

void
sift_cseg_ver_sum(SiftColSegment *seg, uint32 u, SiftColVerSum *sum)
{
	memset(sum, 0, sizeof(*sum));
	if (vers_tag(seg))
	{
		*sum = (SiftColVerSum) {.any = true,.xmin_lo = seg->entry.reserved,.xmin_hi = seg->entry.reserved,.cmin_hi = (uint32) seg->hdr->sum_dl};
		return;
	}
	if (!sift_cseg_vers(seg))
		return;
	{
		const VersUnit *vu = &vers_units(seg)[u];

		*sum = (SiftColVerSum) {.any = (vu->flags & VU_VEC) != 0,.frozen_rows = !(vu->flags & VU_VEC) || (vu->flags & VU_FROZEN),
		.xmin_lo = vu->xmin_lo,.xmin_hi = vu->xmin_hi,.cmin_hi = vu->cmin_hi};
	}
}

void
sift_cseg_ver_unit(SiftColSegment *seg, uint32 u, uint32 *xmin, uint32 *cmin)
{
	uint32		n = seg->sbdir[u].nrows;

	if (vers_tag(seg))
	{
		for (uint32 i = 0; i < n; i++)
		{
			xmin[i] = seg->entry.reserved;
			cmin[i] = (uint32) seg->hdr->sum_dl;
		}
		return;
	}
	if (sift_cseg_vers(seg) && (vers_units(seg)[u].flags & VU_VEC))
	{
		const VersUnit *vu = &vers_units(seg)[u];
		uint8	   *buf = palloc(vu->xlen + vu->clen);

		seg_read(seg, PRIV(seg)->vblob_at + vu->off, vu->xlen + vu->clen, buf);
		if (!sift_cver_decode(buf, vu->xlen, n, xmin) || !sift_cver_decode(buf + vu->xlen, vu->clen, n, cmin))
			corrupt(seg, "The versions of unit %u (SB %u) do not decode as %u rows.", u, seg->sbdir[u].sb, n);
		pfree(buf);
		return;
	}
	memset(xmin, 0, n * sizeof(uint32));
	memset(cmin, 0, n * sizeof(uint32));
}

bool
sift_cseg_page_rows(SiftColSegment *seg, uint32 u, BlockNumber blk, uint32 *first, uint64 *offmask)
{
	SiftCUnitView v;
	uint32		len,
				avail;
	const uint8 *rec = sift_cseg_unit_head(seg, u, &len, &avail);
	uint32		pg = blk % SIFT_SB_PAGES;

	if (!sift_cunit_parse_head(rec, avail, len, seg->schema, &v) || v.nrows != seg->sbdir[u].nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse.", u, seg->sbdir[u].sb);
	for (uint32 p = 0; p < v.npages; p++)
		if (v.page[p] == pg)
		{
			*first = v.rowstart[p];
			sift_cunit_page_offmask(&v, p, offmask);
			return true;
		}
	return false;
}

void
sift_cseg_unit_keys(SiftColSegment *seg, uint32 u, SiftKey *keys)
{
	SiftCUnitView v;
	uint32		len;
	const uint8 *rec = sift_cseg_unit(seg, u, &len);

	if (!sift_cunit_parse(rec, len, seg->schema, &v) || v.nrows != seg->sbdir[u].nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse.", u, seg->sbdir[u].sb);
	sift_cunit_keys(&v, seg->sbdir[u].sb, keys);
}

/* ---------- decoding for rewrites and verification ---------- */

void
sift_cseg_decode_unit_freeze(SiftColSegment *seg, uint32 u, SiftColBatch *dst, SiftColFreezeFn freeze, void *arg)
{
	const SiftColSBRef *r = &seg->sbdir[u];
	SiftCUnitView v;
	uint32		len;
	const uint8 *rec = sift_cseg_unit(seg, u, &len);
	SiftCDict **dicts = palloc0(seg->schema->ncols * sizeof(SiftCDict *));
	uint64	   *live = palloc(SIFT_COL_NWORDS(r->nrows) * sizeof(uint64));
	bool		dirty = unit_dirty(seg, u);

	if (!sift_cunit_parse(rec, len, seg->schema, &v) || v.nrows != r->nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse.", u, r->sb);
	for (int c = 0; c < seg->schema->ncols; c++)
		dicts[c] = sift_cseg_dict(seg, c);
	sift_cseg_live(seg, u, live);
	sift_cbatch_reset(dst, r->sb);
	sift_cunit_decode(&v, r->sb, dicts, dirty ? live : NULL, dst);
	pfree(dicts);
	if (sift_cseg_vers(seg))
	{
		SiftColVerSum sum;

		sift_cseg_ver_sum(seg, u, &sum);
		if (sum.any)
		{
			uint32	   *xm = palloc(r->nrows * sizeof(uint32)),
					   *cm = palloc(r->nrows * sizeof(uint32));
			uint32		k = 0;

			sift_cseg_ver_unit(seg, u, xm, cm);
			for (uint32 i = 0; i < r->nrows; i++)
			{
				if (dirty && !(live[i / 64] >> (i % 64) & 1))
					continue;
				if (xm[i] != 0 && freeze != NULL && freeze(arg, xm[i], cm[i]))
					xm[i] = 0;
				sift_cbatch_set_ver(dst, k++, xm[i], cm[i]);
			}
			Assert(k == dst->nrows);
			pfree(xm);
			pfree(cm);
		}
	}
	pfree(live);
}

void
sift_cseg_decode_unit(SiftColSegment *seg, uint32 u, SiftColBatch *dst)
{
	sift_cseg_decode_unit_freeze(seg, u, dst, NULL, NULL);
}

static bool
vals_have_ovf(const SiftCVals *v)
{
	for (uint32 w = 0; v->ovf != NULL && w < SIFT_COL_NWORDS(v->n); w++)
		if (v->ovf[w])
			return true;
	return false;
}

bool
sift_cseg_stale_ovf(SiftColSegment *seg)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col stale overflow", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColBatch *live = sift_cbatch_create(cx, seg->schema);
	bool		stale = false;

	for (uint32 u = 0; u < seg->nsb && !stale; u++)
	{
		bool		decoded = false;

		for (int c = 0; c < seg->schema->ncols && !stale; c++)
		{
			const ZoneArrays *z = zone_arrays(seg, c);

			if (!z->any_ovf || !(z->flags[u] & SIFT_COLZ_OVERFLOW) || !unit_dirty(seg, u))
				continue;
			if (!decoded)
				sift_cseg_decode_unit(seg, u, live);
			decoded = true;
			stale = !vals_have_ovf(&live->col[c]);
		}
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return stale;
}

/* Does row r of the batch hold an overflow value in a leaf, or in an element of a group (the eoff of the group says which are the row's)? */
static bool
batch_row_ovf(const SiftColBatch *b, uint32 r)
{
	for (int c = 0; c < b->schema->ncols; c++)
	{
		const SiftCVals *v = &b->col[c];
		int			parent = b->schema->cols[c].parent;
		uint32		lo = parent < 0 ? r : b->col[parent].eoff[r],
					hi = parent < 0 ? r + 1 : b->col[parent].eoff[r + 1];

		for (uint32 i = lo; v->ovf != NULL && i < hi; i++)
			if (sift_col_bit_test(v->ovf, i))
				return true;
	}
	return false;
}

int64
sift_cseg_reap_overflow(SiftColSegment *seg, SiftCsegDead dead, void *arg)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col reap overflow", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftColBatch *b = sift_cbatch_create(cx, seg->schema);
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	uint64	   *gone = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint64));
	int64		removed = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		const SiftColSBRef *r = &seg->sbdir[u];
		uint32		j = 0;
		int			nd = 0;
		bool		flagged = false;

		for (int c = 0; c < seg->schema->ncols; c++)
			flagged |= zone_arrays(seg, c)->any_ovf && (zone_arrays(seg, c)->flags[u] & SIFT_COLZ_OVERFLOW);
		if (!flagged)
			continue;
		sift_cseg_decode_unit(seg, u, b);
		sift_cseg_live(seg, u, live);
		for (uint32 i = 0; i < r->nrows; i++)
		{
			ItemPointerData tid;

			if (!sift_col_bit_test(live, i))
				continue;
			sift_key_to_tid(b->keys[j], &tid);
			if (batch_row_ovf(b, j) && dead(&tid, arg))
				gone[nd++] = r->rank_base + i;
			j++;
		}
		if (nd == 0)
			continue;
		sift_seg_mark_dirty(seg->index, &seg->entry);
		sift_live_clear(&PRIV(seg)->live, r->sb - seg->hdr->sb_lo, gone, nd);
		removed += nd;
		CHECK_FOR_INTERRUPTS();
	}
	sift_sr_release(&seg->sr);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return removed;
}

/* ---------- VACUUM ---------- */

int64
sift_cseg_bulkdelete(SiftColSegment *seg, IndexBulkDeleteCallback cb, void *cbstate, IndexBulkDeleteResult *stats)
{
	SegPriv    *pv = PRIV(seg);
	uint64	   *dead = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint64));
	SiftKey    *keys = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(SiftKey));
	uint64	   *live = palloc(SIFT_COL_NWORDS(SIFT_COL_UNIT_MAX_ROWS) * sizeof(uint64));
	int64		removed = 0;

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		const SiftColSBRef *r = &seg->sbdir[u];
		SiftCUnitView v;
		const uint8 *rec;
		uint32		len;
		int			nd = 0;

		rec = sift_cseg_unit(seg, u, &len);
		if (!sift_cunit_parse(rec, len, seg->schema, &v) || v.nrows != r->nrows)
			corrupt(seg, "Unit %u (SB %u) does not parse.", u, r->sb);
		sift_cunit_keys(&v, r->sb, keys);
		sift_cseg_live(seg, u, live);
		for (uint32 i = 0; i < r->nrows; i++)
		{
			ItemPointerData tid;

			if (!(live[i / 64] >> (i % 64) & 1))
				continue;
			sift_key_to_tid(keys[i], &tid);
			if (cb(&tid, cbstate))
				dead[nd++] = r->rank_base + i;
			else
				stats->num_index_tuples++;
		}
		if (nd == 0)
			continue;
		sift_seg_mark_dirty(seg->index, &seg->entry);
		sift_live_clear(&pv->live, r->sb - seg->hdr->sb_lo, dead, nd);
		removed += nd;
		CHECK_FOR_INTERRUPTS();
	}
	sift_sr_release(&seg->sr);
	pfree(dead);
	pfree(keys);
	pfree(live);
	stats->tuples_removed += removed;
	return removed;
}

/* ---------- verification ---------- */

static bool
zone_equal(const SiftColDef *def, const SiftCZone *a, const SiftCZone *b)
{
	bool		fl = def->store == SIFT_COLS_F4 || def->store == SIFT_COLS_F8;

	if (a->nvals != b->nvals || a->nnull != b->nnull || a->flags != b->flags)
		return false;
	if (a->nnull == a->nvals)
		return true;
	if (a->min != b->min || a->max != b->max)
		return false;
	if (!fl && !(a->flags & SIFT_COLZ_NOSUM) && a->sum_i != b->sum_i)
		return false;
	if (fl && memcmp(&a->sum_f, &b->sum_f, sizeof(double)) != 0)
		return false;
	return def->store != SIFT_COLS_STR || memcmp(a->set, b->set, sizeof(a->set)) == 0;
}

/* The unit checks of verify: structure, chunk sizes, dictionary codes, ascending keys inside the SB, zones */
static void
verify_unit(SiftColSegment *seg, uint32 u, SiftKey *keys)
{
	const SiftColSBRef *r = &seg->sbdir[u];
	const SiftColSchema *sc = seg->schema;
	SiftCUnitView v;
	uint32		len;
	const uint8 *rec = sift_cseg_unit(seg, u, &len);
	uint64		group_elems[SIFT_COL_MAX_COLS] = {0};
	SiftCPaggCol pcol[SIFT_COL_MAX_COLS];
	int64	   *kept[SIFT_COL_MAX_COLS] = {NULL};
	bool		need[SIFT_COL_MAX_COLS] = {false};
	uint32		clen,
				elen;
	const uint8 *cube = sift_cseg_cube(seg, u, &clen);
	const uint8 *ecube = sift_cseg_ecube(seg, u, &elen);

	if (!sift_cunit_parse(rec, len, sc, &v) || v.nrows != r->nrows)
		corrupt(seg, "Unit %u (SB %u) does not parse or lists another row count.", u, r->sb);
	if (!sift_cpagg_leaves(sc, cube, clen, ecube, elen, need))
		corrupt(seg, "The cube or the element cubes of the unit of SB %u do not decode.", r->sb);
	sift_cunit_keys(&v, r->sb, keys);
	for (uint32 i = 0; i < r->nrows; i++)
		if (sift_block_sb(sift_key_block(keys[i])) != r->sb || (i > 0 && keys[i] <= keys[i - 1]) ||
			(keys[i] & 0xFFFF) == 0)
			corrupt(seg, "Row %u of the unit of SB %u has key %llx out of order or outside the SB.", i, r->sb, ULL(keys[i]));
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *def = &sc->cols[c];
		SiftCChunk	ch;
		SiftCZone	stored,
					computed;
		int64	   *vals;

		if (!sift_cunit_chunk(&v, c, &ch))
			corrupt(seg, "Unit %u has no chunk for leaf %d.", u, c);
		if (((ch.flags & SIFT_COLC_TAGS) != 0) != (seg->entry.format >= 4 && ch.ovf != NULL))
			corrupt(seg, "The chunk of leaf %d in the unit of SB %u %s the types of its overflow values (segment of column format 1.%d).", c, r->sb,
					ch.flags & SIFT_COLC_TAGS ? "has" : "lacks", seg->entry.format);
		if ((ch.flags & SIFT_COLC_KINDS) && seg->entry.format < 4)
			corrupt(seg, "The chunk of leaf %d in the unit of SB %u has kinds in a segment of column format 1.%d.", c, r->sb, seg->entry.format);
		if (ch.n !=(def->parent < 0 ? r->nrows : group_elems[def->parent]))
			corrupt(seg, "Chunk of leaf %d in the unit of SB %u holds %u values, expected %llu.", c, r->sb, ch.n,
					ULL(def->parent < 0 ? r->nrows : group_elems[def->parent]));
		vals = palloc((Size) ch.n * sizeof(int64) + SIFT_COL_PAD);
		sift_cchunk_decode(&ch, vals);
		sift_cchunk_zone(&ch, def->store, &computed);
		sift_cseg_zone(seg, c, u, &stored);
		if (!zone_equal(def, &stored, &computed))
			corrupt(seg, "The zone of leaf %d in the unit of SB %u differs from its chunk.", c, r->sb);
		if (def->kind == SIFT_COLK_GROUP)
			group_elems[c] = computed.sum_i;
		pcol[c] = (SiftCPaggCol) {vals, ch.nulls, ch.ovf, ch.n, def->store != SIFT_COLS_VAR && ch.enc != SIFT_COLENC_ISTR};
		if (def->store == SIFT_COLS_INT)
		{
			SiftCLeafAgg la;
			SiftCAgg	sa;

			sift_cpagg_leaf(&pcol[c], &la);
			sift_cseg_agg(seg, c, u, &sa);
			if (sa.sum != la.sum || sa.sumsq != la.sumsq || sa.flags != la.flags)
				corrupt(seg, "The exact sums of leaf %d in the unit of SB %u differ from its chunk.", c, r->sb);
		}
		if (def->store == SIFT_COLS_STR && ch.enc != SIFT_COLENC_ISTR)
		{
			SiftCDict  *d = sift_cseg_dict(seg, c);

			for (uint32 i = 0; i < ch.n; i++)
				if (!(ch.nulls && (ch.nulls[i / 64] >> (i % 64) & 1)) && (uint64) vals[i] >= d->n)
					corrupt(seg, "Leaf %d of the unit of SB %u holds code %lld of a dictionary of %u.", c, r->sb,
							(long long) vals[i], d->n);
		}
		if (need[c])
			kept[c] = vals;
		else
			pfree(vals);
	}
	if (!sift_cpagg_check(sc, pcol, r->nrows, cube, clen, ecube, elen))
		corrupt(seg, "The cube or the element cubes of the unit of SB %u differ from its chunks.", r->sb);
	for (int c = 0; c < sc->ncols; c++)
		if (kept[c])
			pfree(kept[c]);
}

/* The version section: every unit's vectors decode, say what the directory of the section says, and the range of the xmins is the header's and the entry's */
static void
verify_vers(SiftColSegment *seg)
{
	SegPriv    *pv = PRIV(seg);
	const VersUnit *vu;
	uint32	   *xm,
			   *cm;
	uint32		lo = 0,
				hi = 0;
	bool		any = false;

	if (pv->vers_len == 0)
		return;
	vu = vers_units(seg);
	xm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	cm = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(uint32));
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		uint32		n = seg->sbdir[u].nrows,
					ulo = 0,
					uhi = 0,
					cmax = 0,
					flags = 0;
		bool		first = true;

		if (!(vu[u].flags & VU_VEC))
		{
			if (vu[u].xmin_lo || vu[u].xmin_hi || vu[u].cmin_hi || (vu[u].flags & VU_FROZEN))
				corrupt(seg, "Unit %u has no versions but a summary of them.", u);
			continue;
		}
		sift_cseg_ver_unit(seg, u, xm, cm);
		for (uint32 i = 0; i < n; i++)
		{
			if (xm[i] == 0)
			{
				flags |= VU_FROZEN;
				if (cm[i] != 0)
					corrupt(seg, "Row %u of unit %u is frozen and has a command.", i, u);
				continue;
			}
			if (first || sift_cver_xid_precedes(xm[i], ulo))
				ulo = xm[i];
			if (first || sift_cver_xid_precedes(uhi, xm[i]))
				uhi = xm[i];
			cmax = Max(cmax, cm[i]);
			first = false;
		}
		if (first || ulo != vu[u].xmin_lo || uhi != vu[u].xmin_hi || cmax != vu[u].cmin_hi || flags != (vu[u].flags & VU_FROZEN))
			corrupt(seg, "The summary of the versions of unit %u (xmin %u..%u, cmin up to %u) is not what its vectors hold.", u, vu[u].xmin_lo,
					vu[u].xmin_hi, vu[u].cmin_hi);
		if (!any || sift_cver_xid_precedes(ulo, lo))
			lo = ulo;
		if (!any || sift_cver_xid_precedes(hi, uhi))
			hi = uhi;
		any = true;
		CHECK_FOR_INTERRUPTS();
	}
	if (!any || lo != pv->vmin || hi != pv->vmax || ((seg->entry.flags & SIFT_SEG_VERS) && seg->entry.reserved != lo))
		corrupt(seg, "The smallest xmin of the entry (%u) and the range of the version section (%u..%u) are not what the vectors (%u..%u) say.",
				seg->entry.reserved, pv->vmin, pv->vmax, lo, hi);
	pfree(xm);
	pfree(cm);
}

uint64
sift_cseg_verify(SiftColSegment *seg)
{
	SiftKey    *keys = palloc(SIFT_COL_UNIT_MAX_ROWS * sizeof(SiftKey));
	uint64		live = seg->hdr->ndocs;

	verify_vers(seg);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		verify_unit(seg, u, keys);
		CHECK_FOR_INTERRUPTS();
	}
	if (seg->entry.flags & SIFT_SEG_DIRTY)
		live -= sift_live_ndead(&PRIV(seg)->live);
	sift_sr_release(&seg->sr);
	pfree(keys);
	return live;
}
