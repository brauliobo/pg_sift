/*
 * col/seg_write.c - the column segment writer: units, dictionaries, zones, pre-aggregates and version section into the stream (col/seg.h).
 */

#include "core/host.h"

#include "col/point.h"
#include "col/preagg.h"
#include "col/runs.h"
#include "col/seg.h"
#include "col/seg_int.h"
#include "col/setsum.h"
#include "col/ver.h"
#include "core/limits.h"
#include "core/live.h"

/* The entries of the point directory of one leaf: the key and the row number (in the segment) of each row with a value; bad: some unit cannot be listed (an overflow value, inline strings, too many rows) */
typedef struct PointAcc
{
	int64	   *key;
	uint32	   *row;
	uint32		n;
	uint32		cap;
	bool		bad;
	bool		want;			/* the index names the leaf (option point_columns) */
	uint32		nslots;			/* the block, once built at the end of the segment (0: none) */
	uint32		nkeys;
	uint32		words;
	StringInfoData tab;
} PointAcc;

/* The small-set summaries (or the sorted runs) of one leaf: the blobs of its units one after the other and where each starts */
#define CDICT_LOOKUP_MIN	1024
/* the dictionaries written in blocks that their leaves did not ask for (DICT_BLOCKS, not DICT_LOOKUP) add together at most 1/33 (3%) of the segment (what it wrote before the dictionaries, and these in the compact form) */
#define CDICT_BLOCKS_BUDGET_DENOM	33
/* an estimate of the blocks over this share of the compact form (1/6) is not worth serializing them for */
#define CDICT_BLOCKS_CLEAR_DENOM	6

bool		sift_col_table_lookup = false;

typedef struct SetAcc
{
	StringInfoData blobs;
	uint64	   *off;			/* [capsb + 1]; unit u's blob is blobs[off[u], off[u + 1]) */
	bool		any;			/* some unit has one */
	uint32		kinds;			/* SETS_EXACT, SETS_BLOOM, SETS_PRES (RUNS_ORD, RUNS_IDENT): the kinds of blob the units have */
} SetAcc;

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
	uint64	   *pmask;			/* [unit * SIFT_PAGEMASK_WORDS]: the pages of its superblock that hold the rows of the unit (superunits) */
	SiftColSBRef *sbdir;
	uint32		nsb;
	uint32		capsb;
	uint64		rows;
	uint32		sb_lo;
	uint32		sb_hi;
	uint8		format;			/* SiftSegEntry.format: what the writer was begun with (a segment with a version section needs SIFT_COL_FORMAT_VERS, the sift_table sees to it); 4 writes the tags of overflow values */
	uint8		setting;		/* the minor the caller asked for, before the schema and the kinds narrowed it: 7 lets a cube hold min and max, 9 a unit the rollup of its rows, 10 a unit the small-set summary of a leaf, 12 the sorted run of a leaf */
	SetAcc	   *set;			/* [ncols], NULL unless the segment keeps small-set summaries (sift_colw_set_preagg) */
	bool		anysets;
	bool		anypres;		/* some unit keeps the presence of a leaf (minor 15) */
	SetAcc	   *run;			/* [ncols], NULL unless the segment keeps sorted runs (sift_colw_set_preagg) */
	bool		anyruns;
	PointAcc   *pt;				/* [ncols], NULL unless the segment keeps point directories (sift_colw_set_preagg) */
	bool		anypoints;		/* some leaf has a point directory to write */
	bool		inexact;		/* some unit lacks its kinds: the segment keeps the minor it has (minor 14 promises complete kinds) */
	uint8	   *dform;			/* [ncols] the dictionary is written in blocks to be read in place (DICT_BLOCKS), and with the order to be searched in place (DICT_LOOKUP): minor 14; 0: the compact form */
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
	w->setting = format;
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
	w->pmask = palloc(w->capsb * SIFT_PAGEMASK_WORDS * sizeof(uint64));
	sift_cpagg_spec_default(schema, &w->pagg);
	/* a sift_table has no option for its dictionaries: its leaves have the order to search stored only when sift.table_dict_lookup says so */
	for (int c = 0; c < schema->ncols; c++)
		w->pagg.lookup[c] = sift_col_table_lookup;
	/* a sift_table has no index option for its pre-aggregates and writes this default (a sift_col index sets its own spec, which has calendar dims when it asks) */
	if (format >= SIFT_COL_FORMAT_CALENDAR && !schema->kinds)
	{
		sift_cpagg_spec_calendar(schema, &w->pagg);
		for (int d = 0; d < w->pagg.ndims; d++)
			if (w->pagg.dim[d].cal)
				w->format = SIFT_COL_FORMAT_CALENDAR;
	}
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

/* The accumulators of the leaves flagged in want, NULL when there are none */
static SetAcc *
acc_new(SiftColSegWriter *w, const bool *want)
{
	SetAcc	   *acc = NULL;
	MemoryContext old = MemoryContextSwitchTo(w->mcxt);

	for (int c = 0; c < w->schema->ncols; c++)
		if (want[c])
		{
			if (acc == NULL)
				acc = palloc0(w->schema->ncols * sizeof(SetAcc));
			initStringInfo(&acc[c].blobs);
			acc[c].off = palloc0(((Size) w->capsb + 1) * sizeof(uint64));
		}
	MemoryContextSwitchTo(old);
	return acc;
}

void
sift_colw_set_preagg(SiftColSegWriter *w, const SiftCPaggSpec *spec)
{
	w->pagg = *spec;
	/* the cube asks for min and max: the segment is minor 7, which the caller may not have allowed (sift.col_format_minor) or the kinds of its rows forbid */
	w->pagg.minmax = spec->minmax && spec->ndims > 0 && w->setting >= SIFT_COL_FORMAT_MINMAX;
	if (w->pagg.minmax)
		w->format = SIFT_COL_FORMAT_MINMAX;
	else if (w->format == SIFT_COL_FORMAT_CALENDAR)
		w->format = sift_col_write_minor(w->schema, w->setting, true);	/* the calendar dims came with the default spec this one replaces */
	/* an index that names calendar columns (calendar_columns): minor 8, where the setting allows it; under a lower one the segment is written as before, without them */
	w->pagg.ndims = 0;
	for (int d = 0; d < spec->ndims; d++)
	{
		if (spec->dim[d].cal && w->setting < SIFT_COL_FORMAT_CALENDAR)
			continue;
		w->pagg.dim[w->pagg.ndims++] = spec->dim[d];
		if (spec->dim[d].cal)
			w->format = Max(w->format, SIFT_COL_FORMAT_CALENDAR);
	}
	/* the rollup of the rows is minor 9: under a lower setting the segment is written as before, without it (the entry is -1, the first of the sorted rollups) */
	if (w->pagg.nrollups > 0 && w->pagg.rollup[0].list < 0)
	{
		if (w->setting >= SIFT_COL_FORMAT_ROWS)
			w->format = SIFT_COL_FORMAT_ROWS;
		else
		{
			w->pagg.nrollups--;
			memmove(w->pagg.rollup, w->pagg.rollup + 1, w->pagg.nrollups * sizeof(SiftCPaggRollup));
		}
	}
	/* a leaf asks for its small-set summary, its key presence or its sorted run: minor 10, 15 or 13 when some unit gets one (sift_colw_finish) */
	if (w->setting < SIFT_COL_FORMAT_PRESENCE)
		memset(w->pagg.pres, 0, sizeof(w->pagg.pres));
	if (w->setting >= SIFT_COL_FORMAT_SETS && w->set == NULL)
	{
		bool		want[SIFT_COL_MAX_COLS];

		for (int c = 0; c < w->schema->ncols; c++)
			want[c] = spec->set[c] || w->pagg.pres[c];
		w->set = acc_new(w, want);
	}
	/* the superunit summaries are minor 12, which the caller may not have allowed (sift.col_format_minor) or the kinds of its rows forbid */
	w->pagg.super = spec->super >= SIFT_SUPER_MIN_FAN && w->setting >= SIFT_COL_FORMAT_SUPER ? Min(spec->super, SIFT_SUPER_MAX_FAN) : 0;
	if (w->pagg.super)
		w->format = SIFT_COL_FORMAT_SUPER;
	if (w->setting >= SIFT_COL_FORMAT_RUNS && w->run == NULL)
		w->run = acc_new(w, spec->run);
	/* the point directories are written with the dictionaries, minor 14 */
	if (w->setting >= SIFT_COL_FORMAT_DICTIDX && w->pt == NULL)
		for (int c = 0; c < w->schema->ncols; c++)
			if (spec->point[c])
			{
				MemoryContext old = MemoryContextSwitchTo(w->mcxt);

				if (w->pt == NULL)
					w->pt = palloc0(w->schema->ncols * sizeof(PointAcc));
				MemoryContextSwitchTo(old);
				w->pt[c].want = true;
			}
}

/* Adds the rows of the unit just encoded to the point directories of the leaves that ask for one (the keys of cols: the segment's codes of a dictionary leaf) */
static void
add_points(SiftColSegWriter *w, const SiftCPaggCol *cols)
{
	for (int c = 0; w->pt && c < w->schema->ncols; c++)
	{
		PointAcc   *a = &w->pt[c];
		const SiftCPaggCol *v = &cols[c];

		if (!a->want || a->bad)
			continue;
		for (uint32 i = 0; v->ovf && i < SIFT_COL_NWORDS(v->n); i++)
			if (v->ovf[i])
				a->bad = true;
		if (!v->ready || a->bad || w->rows + v->n >= SIFT_CPOINT_SEG_ROWS)
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
			MemoryContextSwitchTo(old);
		}
		for (uint32 i = 0; i < v->n; i++)
		{
			if (v->nulls && (v->nulls[i / 64] >> (i % 64) & 1))
				continue;
			a->key[a->n] = v->key[i];
			a->row[a->n++] = (uint32) w->rows + i;
		}
	}
}

/*
 * The small-set summary of each leaf that asks for one, from the keys of the unit just encoded (in the segment's codes for a dictionary leaf; none for an
 * inline one). A leaf of key_presence keeps all its keys as a bitmap where that takes a few percent of the unit's bytes (unit_bytes), else its set summary.
 */
static void
add_sets(SiftColSegWriter *w, const SiftCPaggCol *cols, Size unit_bytes)
{
	int64	   *keys = NULL;

	for (int c = 0; w->set && c < w->schema->ncols; c++)
	{
		SetAcc	   *a = &w->set[c];
		uint32		nd;

		if (a->blobs.data == NULL)
			continue;
		a->off[w->nsb] = a->blobs.len;
		if (cols[c].ready && cols[c].n > 1)
		{
			keys = keys ? repalloc(keys, (Size) cols[c].n * sizeof(int64)) : palloc((Size) cols[c].n * sizeof(int64));
			nd = sift_cset_keys(&cols[c], keys);
			if (w->pagg.pres[c] && nd > 0 && sift_cset_build_pres(keys, nd, unit_bytes / 100 * SIFT_CSET_PRES_PCT, &a->blobs))
			{
				a->any = w->anysets = w->anypres = true;
				a->kinds |= SETS_PRES;
			}
			else if (w->pagg.set[c] && sift_cset_build(keys, nd, w->pagg.set_limit ? w->pagg.set_limit : SIFT_CSET_LIMIT, &a->blobs))
			{
				a->any = w->anysets = true;
				a->kinds |= a->blobs.data[a->off[w->nsb]] == SIFT_CSET_EXACT ? SETS_EXACT : SETS_BLOOM;
			}
		}
		a->off[w->nsb + 1] = a->blobs.len;
	}
	if (keys)
		pfree(keys);
}

/* The sorted run of each leaf that asks for one, from the keys of the unit just encoded */
static void
add_runs(SiftColSegWriter *w, const SiftCPaggCol *cols)
{
	for (int c = 0; w->run && c < w->schema->ncols; c++)
	{
		SetAcc	   *a = &w->run[c];

		if (a->blobs.data == NULL)
			continue;
		a->off[w->nsb] = a->blobs.len;
		if (sift_crun_build(&w->schema->cols[c], &cols[c], &a->blobs))
		{
			a->any = w->anyruns = true;
			a->kinds |= a->blobs.data[a->off[w->nsb]] == SIFT_CRUN_ORD ? RUNS_ORD : RUNS_IDENT;
		}
		a->off[w->nsb + 1] = a->blobs.len;
	}
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
		if (def->store == SIFT_COLS_STR && (dim[c] || (w->set && w->set[c].blobs.data) || (w->pt && w->pt[c].want)) && !w->inl[c])
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
	add_sets(w, cols, unit_bytes);
	add_runs(w, cols);
	add_points(w, cols);
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
	{
		/* the callers lower the setting before they begin when a row of the segment may lack its kind: minor 7, 9, 10 and 12 promise complete kinds */
		SIFT_INVARIANT(!((w->format >= SIFT_COL_FORMAT_MINMAX && w->format != SIFT_COL_FORMAT_CALENDAR) || w->set || w->run),
					   "sift_col: a unit without its kinds in a segment of column format %d", w->run ? SIFT_COL_FORMAT_RUNS : w->set ? SIFT_COL_FORMAT_SETS : w->format);
		w->inexact = true;
		/* a schema with a calendar dim has no leaf with kinds to lose: the minor stays */
		if (w->format != SIFT_COL_FORMAT_CALENDAR)
			w->format = sift_col_write_minor(w->schema, Min(w->format, SIFT_COL_FORMAT_TAGS), false);
	}
	Assert(b->nrows > 0 && b->schema->ncols == ncols);
	SIFT_INVARIANT(w->nsb == 0 || b->sb > w->sbdir[w->nsb - 1].sb, "sift_col: superblock %u added after %u", b->sb,
				   w->nsb > 0 ? w->sbdir[w->nsb - 1].sb : 0);
	if (w->nsb == w->capsb)
	{
		w->capsb *= 2;
		w->zones = repalloc_huge(w->zones, (Size) w->capsb * ncols * sizeof(SiftCZone));
		w->lagg = repalloc_huge(w->lagg, (Size) w->capsb * ncols * sizeof(SiftCLeafAgg));
		w->cubeoff = repalloc_huge(w->cubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		w->ecubeoff = repalloc_huge(w->ecubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		for (int c = 0; c < ncols; c++)
		{
			if (w->set && w->set[c].off)
				w->set[c].off = repalloc_huge(w->set[c].off, ((Size) w->capsb + 1) * sizeof(uint64));
			if (w->run && w->run[c].off)
				w->run[c].off = repalloc_huge(w->run[c].off, ((Size) w->capsb + 1) * sizeof(uint64));
		}
		w->pmask = repalloc_huge(w->pmask, (Size) w->capsb * SIFT_PAGEMASK_WORDS * sizeof(uint64));
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
	memcpy(w->pmask + (Size) w->nsb * SIFT_PAGEMASK_WORDS, w->buf.data + start + 4 * sizeof(uint32), SIFT_PAGEMASK_WORDS * sizeof(uint64));	/* after the unit's head */
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

/* The blocks of the point directories, built when the last unit is in */
static void
build_points(SiftColSegWriter *w)
{
	for (int c = 0; w->pt && c < w->schema->ncols; c++)
	{
		PointAcc   *a = &w->pt[c];

		if (a->want && !a->bad && a->n > 0)
		{
			MemoryContext old = MemoryContextSwitchTo(w->mcxt);

			initStringInfo(&a->tab);
			MemoryContextSwitchTo(old);
			sift_cpoint_build(a->key, a->row, a->n, &a->tab, &a->nslots, &a->nkeys, &a->words);
			w->anypoints = true;
		}
		if (a->key)
		{
			pfree(a->key);
			pfree(a->row);
			a->key = NULL;
			a->row = NULL;
		}
	}
}

/*
 * The point directories after the dictionaries, in the DICT section (docs/COL.md 2.17): a header {u32 magic, u32 version, u32 nleaves, u32 0}, a record
 * {u32 leaf, u32 nslots, u32 rows, u32 keys, u32 words, u32 0, u64 offset in the section} for each leaf, the blocks, and the last 16 bytes of the section
 * {u64 offset of the header, u32 magic, u32 version}.
 */
static void
write_points(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	uint64		block = sift_sw_tell(w->sw) - sec->off;
	uint32		hdr[4] = {SIFT_CPOINT_MAGIC, SIFT_CPOINT_VERSION, 0, 0};
	uint64		foot[2];
	uint32		ftail[2] = {SIFT_CPOINT_MAGIC, SIFT_CPOINT_VERSION};
	uint64		at;

	if (!w->anypoints)
		return;
	for (int c = 0; c < ncols; c++)
		hdr[2] += w->pt[c].nslots > 0;
	at = block + sizeof(hdr) + (uint64) hdr[2] * SIFT_CPOINT_RECLEN;
	sift_sw_write(w->sw, hdr, sizeof(hdr));
	for (int c = 0; c < ncols; c++)
	{
		const PointAcc *a = &w->pt[c];
		uint32		rec[6] = {c, a->nslots, a->n, a->nkeys, a->words, 0};

		if (a->nslots == 0)
			continue;
		sift_sw_write(w->sw, rec, sizeof(rec));
		sift_sw_write(w->sw, &at, sizeof(at));
		at += a->tab.len;
	}
	for (int c = 0; c < ncols; c++)
		if (w->pt[c].nslots > 0)
			sift_sw_write(w->sw, w->pt[c].tab.data, w->pt[c].tab.len);
	foot[0] = block;
	memcpy(&foot[1], ftail, sizeof(ftail));
	sift_sw_write(w->sw, foot, sizeof(foot));
}

static void
write_dicts(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	uint64	   *offs = palloc0(sizeof(uint32) * 2 + ncols * sizeof(uint64));
	uint32	   *hdr = (uint32 *) offs;
	uint64	   *tab = (uint64 *) (hdr + 2);
	Size	   *extra = palloc0(ncols * sizeof(Size)),
			   *compact = palloc0(ncols * sizeof(Size)),
				budget = sift_sw_tell(w->sw);
	bool		blocks = w->setting >= SIFT_COL_FORMAT_DICTIDX && !w->inexact;
	StringInfoData blob;

	sw_pad8(w->sw);
	sec->off = sift_sw_tell(w->sw);
	hdr[0] = DICTS_MAGIC;
	hdr[1] = ncols;
	/*
	 * A dictionary of many strings that did not ask for the order is stored in blocks, to be read in place, when that adds little to the segment (the
	 * budget): ids, hashes and uuids compress as well in blocks as whole, short strings that repeat their words do not. The estimates from a sample
	 * decide what is clear (they run high for the blocks: an estimate within the budget is), and the blocks are serialized next to the compact form
	 * only for a dictionary in between
	 */
	for (int c = 0; blocks && c < ncols; c++)
	{
		if (w->dict[c] == NULL || (w->dform && w->dform[c]) || w->dict[c]->n < sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN))
			extra[c] = SIZE_MAX;
		else
			extra[c] = sift_cdict_blocks_extra(w->dict[c], w->pol ? &w->pol[c] : NULL, &compact[c]);
		budget += compact[c];
	}
	budget /= CDICT_BLOCKS_BUDGET_DENOM;
	/* the directory is written last to know the offsets: serialize the dictionaries first into memory */
	initStringInfo(&blob);
	for (int c = 0; c < ncols; c++)
	{
		uint32		form = w->dform ? w->dform[c] : 0;

		if (w->dict[c] == NULL)
			continue;
		while (blob.len % 8)
			appendStringInfoChar(&blob, '\0');
		tab[c] = sizeof(uint32) * 2 + ncols * sizeof(uint64) + blob.len;
		if (extra[c] != SIZE_MAX && blocks)
		{
			if (extra[c] <= budget)
			{
				form = DICT_BLOCKS;
				budget -= extra[c];
			}
			else if (extra[c] <= compact[c] / CDICT_BLOCKS_CLEAR_DENOM)
			{
				StringInfoData whole,
							alt;

				initStringInfo(&whole);
				initStringInfo(&alt);
				sift_cdict_serialize(w->dict[c], w->pol ? &w->pol[c] : NULL, 0, &whole);
				sift_cdict_serialize(w->dict[c], w->pol ? &w->pol[c] : NULL, DICT_BLOCKS, &alt);
				if (alt.len <= whole.len + budget)
				{
					budget -= Max(alt.len, whole.len) - whole.len;
					appendBinaryStringInfo(&blob, alt.data, alt.len);
					w->format = Max(w->format, SIFT_COL_FORMAT_DICTIDX);
				}
				else
					appendBinaryStringInfo(&blob, whole.data, whole.len);
				pfree(whole.data);
				pfree(alt.data);
				continue;
			}
		}
		if (form == DICT_BLOCKS)
			w->format = Max(w->format, SIFT_COL_FORMAT_DICTIDX);
		sift_cdict_serialize(w->dict[c], w->pol ? &w->pol[c] : NULL, form, &blob);
	}
	pfree(extra);
	pfree(compact);
	sift_sw_write(w->sw, offs, sizeof(uint32) * 2 + ncols * sizeof(uint64));
	sift_sw_write(w->sw, blob.data, blob.len);
	sw_pad8(w->sw);
	write_points(w, sec);
	sec->len = sift_sw_tell(w->sw) - sec->off;
	pfree(blob.data);
	pfree(offs);
}

/*
 * The records of the leaves of acc that have blobs (the small-set summaries, or the sorted runs) and where the section of them ends up, from at (8-aligned,
 * from the start of the header): *start and *bytes are what the header says of it
 */
static SetsLeaf *
layout_blobs(SiftColSegWriter *w, const SetAcc *acc, Size at, Size *start, Size *bytes, int *nleaves)
{
	int			n = 0;
	SetsLeaf   *sl = palloc0(w->schema->ncols * sizeof(SetsLeaf));

	*start = at;
	for (int c = 0; c < w->schema->ncols; c++)
		if (acc[c].any)
			n++;
	at += sizeof(SetsHeader) + n * sizeof(SetsLeaf);
	n = 0;
	for (int c = 0; c < w->schema->ncols; c++)
		if (acc[c].any)
		{
			at = ALIGN8(at);
			sl[n++] = (SetsLeaf) {c, acc[c].kinds, at, acc[c].blobs.len};
			at += (w->nsb + 1) * sizeof(uint64) + acc[c].blobs.len;
		}
	*bytes = ALIGN8(at) - *start;
	*nleaves = n;
	return sl;
}

static void
write_blobs(SiftColSegWriter *w, const SetAcc *acc, uint32 magic, SetsLeaf *sl, int n)
{
	SetsHeader	sh = {magic, 1, n, w->nsb};

	sw_pad8(w->sw);
	sift_sw_write(w->sw, &sh, sizeof(sh));
	sift_sw_write(w->sw, sl, n * sizeof(SetsLeaf));
	for (int i = 0; i < n; i++)
	{
		const SetAcc *a = &acc[sl[i].leaf];

		sw_pad8(w->sw);
		sift_sw_write(w->sw, a->off, (Size) (w->nsb + 1) * sizeof(uint64));
		sift_sw_write(w->sw, a->blobs.data, a->blobs.len);
	}
	sw_pad8(w->sw);
	pfree(sl);
}

/* The sums and the zone of a unit's INT leaf as the reader returns them (sift_cseg_agg) */
static void
unit_agg_of(const SiftCZone *z, const SiftCLeafAgg *la, SiftCAgg *a)
{
	memset(a, 0, sizeof(*a));
	a->nvals = z->nvals;
	a->nnull = z->nnull;
	a->min = z->min;
	a->max = z->max;
	a->zflags = z->flags;
	if (la)
	{
		a->sum = la->sum;
		a->sumsq = la->sumsq;
		a->flags = la->flags;
	}
	a->has_sums = la != NULL;
}

/* The superunit block (seg_int.h) of the units written so far, in out; false when the segment has none (not asked for, one unit) */
static bool
build_super(SiftColSegWriter *w, StringInfo out)
{
	int			ncols = w->schema->ncols;
	uint32		nsb = w->nsb,
				fan = Min(w->pagg.super, nsb),	/* one group of all the units is stored with that count: the group size is then not free to change unseen */
				nsu = fan ? (nsb + fan - 1) / fan : 0;
	SuperHeader h = {.magic = SUPER_MAGIC,.fan = fan,.nsu = nsu};
	MemoryContext cx,
				old;
	SiftCAgg   *ua;
	uint64	   *cdir;
	StringInfoData cubes;

	if (fan == 0 || nsb < 2)
		return false;
	for (int c = 0; c < ncols; c++)
		h.nleaf += super_leaf_ok(&w->schema->cols[c]);
	cx = AllocSetContextCreate(w->mcxt, "sift col superunits", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cx);
	ua = palloc(fan * sizeof(SiftCAgg));
	cdir = palloc((nsu + 1) * sizeof(uint64));
	initStringInfo(&cubes);
	for (uint32 k = 0; k < nsu; k++)
	{
		uint32		n = Min(fan, nsb - k * fan);
		const uint8 **blob = palloc(n * sizeof(uint8 *));
		uint32	   *len = palloc(n * sizeof(uint32));
		bool		anycube = false;
		Size		before = cubes.len;

		cdir[k] = cubes.len;
		for (uint32 i = 0; i < n; i++)
		{
			uint32		u = k * fan + i;

			blob[i] = (const uint8 *) w->cube.data + w->cubeoff[u];
			len[i] = w->cubeoff[u + 1] - w->cubeoff[u];
			anycube |= len[i] > 0;
		}
		if (n >= 2 && anycube && !sift_cpagg_super_cube(w->schema, blob, len, n, &cubes))
			cubes.len = before;
		pfree(blob);
		pfree(len);
	}
	cdir[nsu] = cubes.len;
	h.cubedir_off = cubes.len > 0 ? ALIGN8(sizeof(SuperHeader) + (Size) nsb * SIFT_PAGEMASK_WORDS * sizeof(uint64) + (Size) h.nleaf * nsu * sizeof(SiftCSuperLeaf)) : 0;
	h.cube_bytes = cubes.len;
	MemoryContextSwitchTo(old);
	appendBinaryStringInfo(out, (const char *) &h, sizeof(h));
	appendBinaryStringInfo(out, (const char *) w->pmask, (Size) nsb * SIFT_PAGEMASK_WORDS * sizeof(uint64));
	for (int c = 0; c < ncols; c++)
	{
		if (!super_leaf_ok(&w->schema->cols[c]))
			continue;
		for (uint32 k = 0; k < nsu; k++)
		{
			uint32		n = Min(fan, nsb - k * fan);
			bool		sums = w->schema->cols[c].store == SIFT_COLS_INT;
			SiftCSuperLeaf sl;

			for (uint32 i = 0; i < n; i++)
				unit_agg_of(&w->zones[(Size) (k * fan + i) * ncols + c], sums ? &w->lagg[(Size) (k * fan + i) * ncols + c] : NULL, &ua[i]);
			if (n < 2 || !sift_cpagg_super_leaf(ua, n, sums, &sl))
				memset(&sl, 0, sizeof(sl));
			appendBinaryStringInfo(out, (const char *) &sl, sizeof(sl));
		}
	}
	if (h.cubedir_off)
	{
		appendBinaryStringInfo(out, (const char *) cdir, (Size) (nsu + 1) * sizeof(uint64));
		appendBinaryStringInfo(out, cubes.data, cubes.len);
	}
	MemoryContextDelete(cx);
	return true;
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
	SetsLeaf   *sets = NULL,
			   *runs = NULL;
	int			nsets = 0,
				nruns = 0;
	StringInfoData super;
	bool		has_super;

	initStringInfo(&super);
	has_super = ver > 3 && build_super(w, &super);
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
		at = ALIGN8(at + (nsb + 1) * sizeof(uint64) + w->ecube.len);
	}
	if (ver > 2 && w->anysets)
	{
		Size		start,
					bytes;

		sets = layout_blobs(w, w->set, at, &start, &bytes, &nsets);
		PAGG_SETS(h) = start;
		PAGG_SETS_BYTES(h) = bytes;
		at = start + bytes;
	}
	if (has_super)
	{
		PAGG_SUPERDIR(h) = at;
		PAGG_SUPER_BYTES(h) = super.len;
		at = ALIGN8(at + super.len);
	}
	if (ver > 4 && w->anyruns)
	{
		Size		start,
					bytes;

		runs = layout_blobs(w, w->run, at, &start, &bytes, &nruns);
		PAGG_RUNS(h) = start;
		PAGG_RUNS_BYTES(h) = bytes;
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
	if (sets)
		write_blobs(w, w->set, SETS_MAGIC, sets, nsets);
	if (has_super)
	{
		sw_pad8(w->sw);
		sift_sw_write(w->sw, super.data, super.len);
	}
	pfree(super.data);
	if (runs)
		write_blobs(w, w->run, RUNS_MAGIC, runs, nruns);
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

	SIFT_INVARIANT(w->nsb != 0, "sift_col: a segment needs at least one unit");
	Assert(!w->anyver || w->format >= SIFT_COL_FORMAT_VERS);
	if (w->anysets)
		w->format = Max(w->format, w->anypres ? SIFT_COL_FORMAT_PRESENCE : SIFT_COL_FORMAT_SETS);
	if (w->anyruns)
		w->format = Max(w->format, SIFT_COL_FORMAT_RUNS);
	build_points(w);
	if (w->anypoints)
		w->format = Max(w->format, SIFT_COL_FORMAT_DICTIDX);
	/* a dictionary of many strings is written with the order to be searched in place when the setting allows it, the leaf asks for it (spec.lookup) and the kinds are complete */
	if (w->setting >= SIFT_COL_FORMAT_DICTIDX && !w->inexact)
	{
		w->dform = palloc0(w->schema->ncols * sizeof(uint8));
		for (int c = 0; c < w->schema->ncols; c++)
			if (w->dict[c] != NULL && w->pagg.lookup[c] && w->dict[c]->n >= sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN))
			{
				w->dform[c] = DICT_LOOKUP;
				w->format = Max(w->format, SIFT_COL_FORMAT_DICTIDX);
			}
	}
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
