/*
 * col/seg_write.c - the column segment writer: units, dictionaries, zones, pre-aggregates and version section into the stream (col/seg.h).
 */

#include "core/host.h"

#include "col/gorder.h"
#include "col/point.h"
#include "col/preagg.h"
#include "col/hll.h"
#include "col/runs.h"
#include "col/seg.h"
#include "col/ktab.h"
#include "col/seg_int.h"
#include "col/setsum.h"
#include "col/ver.h"
#include "core/limits.h"
#include "core/live.h"

static bool elem_point(const SiftColSegWriter *w, int c);

/* The entries of the point directory of one leaf: the key and the row number (in the segment) of each row with a value; bad: some unit cannot be listed (an overflow value, inline strings, too many rows) */
typedef struct OrdBlock
{
	uint32		nch;			/* chunks (0: none) */
	uint32		nent;
	uint32		nvals;
	StringInfoData tab;
} OrdBlock;

typedef struct PointAcc
{
	int64	   *key;
	uint32	   *row;
	uint64	   *tid;			/* the heap address of each entry (a table of addresses, option point_tid_columns), else NULL */
	uint32		n;
	uint32		cap;
	bool		bad;
	bool		want;			/* the index names the leaf (option point_columns or point_tid_columns) */
	bool		tids;			/* ... and wants the table to list heap addresses */
	bool		ord;			/* the leaf also gets a global order block (col/gorder.h): the entries are kept for it, with the addresses of the rows without a value */
	int64	   *gk;				/* ord with a group leaf: the group of each entry of key, and of each one of ntid (a row with no group is left out when the block is built) */
	uint8	   *gok;
	int64	   *ngk;
	uint8	   *ngok;
	uint64	   *ntid;
	uint32		nn;
	uint32		ncap;
	uint32		gcap;
	OrdBlock	ob[2];			/* its blocks once built: the plain order, and (the group of the build's choice) the order sorted by group, which is the second slot of the leaf */
	uint32		nslots;			/* the block, once built at the end of the segment (0: none) */
	bool		exact;			/* ... with the slot count of SIFT_CPOINT_EXACT */
	SiftCPackPar par;			/* ... or as a packed table (par.pbits not 0, SIFT_CPOINT_PACKED): nslots is its entries */
	uint32		nkeys;
	uint32		words;
	StringInfoData tab;
} PointAcc;

/* The rows of each code of the dictionary of a leaf of the option count_columns, over the units added; bad: a unit stores strings inline or has an overflow value, so the rows would not add up */
typedef struct CountAcc
{
	uint32	   *rows;
	uint32		cap;
	bool		bad;
} CountAcc;

/* The small-set summaries (or the sorted runs) of one leaf: the blobs of its units one after the other and where each starts */
#define CDICT_LOOKUP_MIN	1024
#define CDICT_TSIG_MIN_KB	1024
#define LST_MIN_UNITS		64		/* a segment of fewer units keeps no leaf statistics: its zone arrays are a few pages */
#define LST_SHARE_DIV		33		/* the leaf statistics take at most 1/33 (3%) of the segment: units of a few rows and many leaves keep none */
/* the dictionaries written in blocks that their leaves did not ask for (DICT_BLOCKS, not DICT_LOOKUP) add together at most 1/33 (3%) of the segment (what it wrote before the dictionaries, and these in the compact form) */
#define CDICT_BLOCKS_BUDGET_DENOM	33
/* an estimate of the blocks over this share of the compact form (1/6) is not worth serializing them for */
#define CDICT_BLOCKS_CLEAR_DENOM	6

bool		sift_col_table_lookup = true;
bool		sift_col_table_key_table = true;

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
	uint64		index_rows;		/* the rows of the index (sift_colw_set_index_rows) */
	uint64		gen;
	const SiftColSchema *schema;
	const SiftCcPolicy *pol;	/* [ncols] compression policy per leaf, NULL: plain chunks */
	MemoryContext mcxt;
	SiftStreamWriter *sw;
	SiftCDict **dict;			/* [ncols], STR leaves */
	uint32	  **dictmap;		/* [ncols] scratch: batch code -> segment code */
	bool	   *inl;			/* [ncols] scratch: the STR leaf is stored inline in the unit being added */
	SiftCZone  *zones;			/* [unit * ncols + col] */
	uint8	   *lst;			/* the leaf statistics block (seg_int.h LstHeader), made with the zones; NULL for a segment of fewer than LST_MIN_UNITS units */
	Size		lstlen;
	SiftCPaggSpec pagg;			/* the pre-aggregates to write (docs/COL-PREAGG.md) */
	SiftCLeafAgg *lagg;			/* [unit * ncols + col] exact sums of the INT leaves */
	uint64	   *cubeoff;		/* [unit]: offset of the unit's cube blob in cube; [nsb] the end */
	StringInfoData cube;		/* the cube blobs of all units */
	uint64	   *ecubeoff;		/* the same for the element blobs (minor 4) */
	StringInfoData ecube;
	uint64	   *xcubeoff;		/* the same for the exception cubes and element blobs (minor 19): a unit with none has an empty blob */
	StringInfoData xcube;
	uint64	   *xecubeoff;
	StringInfoData xecube;
	bool		excepts;		/* the units may get exceptions: a sift_col index, written under a setting that allows them */
	bool		anyexc;			/* some unit has exceptions: the segment is column format minor 19 */
	bool		anymeaslen;		/* some unit has a rollup with the length of each measure: the segment is column format minor 36 */
	uint64	   *pmask;			/* [unit * SIFT_PAGEMASK_WORDS]: the pages of its superblock that hold the rows of the unit (superunits) */
	SiftColSBRef *sbdir;
	uint32		nsb;
	uint32		capsb;
	uint64		rows;
	uint32		sb_lo;
	uint32		sb_hi;
	uint32		aim_rows;		/* the rows of a unit of the page cut of the table: a key-cut unit's cube is budgeted as that unit's */
	bool		keyed;			/* the units are key-cut: the segment is column format minor 17 */
	uint8		format;			/* SiftSegEntry.format: what the writer was begun with (a segment with a version section needs SIFT_COL_FORMAT_VERS, the sift_table sees to it); 4 writes the tags of overflow values */
	uint8		setting;		/* the minor the caller asked for, before the schema and the kinds narrowed it: 7 lets a cube hold min and max, 9 a unit the rollup of its rows, 10 a unit the small-set summary of a leaf, 12 the sorted run of a leaf */
	SetAcc	   *set;			/* [ncols], NULL unless the segment keeps small-set summaries (sift_colw_set_preagg) */
	bool		anysets;
	bool		anypres;		/* some unit keeps a key list or bitmap of a leaf (minor 15) */
	bool		anyrunl;		/* ... and one of them as runs of consecutive keys (minor 24) */
	SetAcc	   *run;			/* [ncols], NULL unless the segment keeps sorted runs (sift_colw_set_preagg) */
	bool		anyruns;
	bool		anyheads;		/* ... and one of them has its head (column format minor 18) */
	SetAcc	   *hll;			/* [ncols], NULL unless the segment keeps HyperLogLog sketches (sift_colw_set_preagg) */
	bool		anyhll;
	SiftCKtab  *kt;				/* the key tables of the leaves the index keeps key lists for (minor 22), NULL when the segment has none to keep */
	StringInfoData ktab;
	CountAcc   *cnt;			/* [ncols], NULL unless the segment counts the rows of the codes of some leaf (option count_columns, minor 24) */
	PointAcc   *pt;				/* [ncols], NULL unless the segment keeps point directories (sift_colw_set_preagg) */
	bool		anypoints;		/* some leaf has a point directory to write */
	bool		anyord;			/* some leaf has a global order block to write (minor 30; minor 21 had chunks of SIFT_CGORD_ROWS) */
	bool		anydual;		/* ... and some leaf a second one, sorted by group (minor 31) */
	bool		anytextord;		/* ... and some leaf of text (minor 32) */
	bool		anypacked;		/* ... and one of them is a packed table (column format minor 36) */
	bool		anytids;		/* ... and one of them lists heap addresses, or the rows of the values of a list in the frame of the addresses (column format minor 16) */
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
	if (schema->contain)
		minor = Max(minor, SIFT_COL_FORMAT_CONTAIN);
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
	w->xcubeoff = palloc((w->capsb + 1) * sizeof(uint64));
	w->xecubeoff = palloc((w->capsb + 1) * sizeof(uint64));
	w->pmask = palloc(w->capsb * SIFT_PAGEMASK_WORDS * sizeof(uint64));
	initStringInfo(&w->cube);
	initStringInfo(&w->ecube);
	initStringInfo(&w->xcube);
	initStringInfo(&w->xecube);
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

/* The rows of each segment code of the leaves that count them (option count_columns): a unit that stores its strings inline, has an overflow value or belongs to a child leaf makes the leaf's counts unusable */
static void
count_rows(SiftColSegWriter *w, const SiftColBatch *b)
{
	for (int c = 0; w->cnt != NULL && c < w->schema->ncols; c++)
	{
		CountAcc   *a = &w->cnt[c];
		const SiftCVals *v = &b->col[c];
		uint32		n = w->dict[c] != NULL ? w->dict[c]->n : 0;

		if (!w->pagg.count[c] || a->bad)
			continue;
		if (w->dict[c] == NULL || w->inl[c] || v->ovf != NULL || v->n != b->nrows)
		{
			a->bad = true;
			continue;
		}
		if (a->cap < n)
		{
			uint32		cap = Max(n, 2 * a->cap);

			a->rows = a->rows ? repalloc(a->rows, (Size) cap * sizeof(uint32)) : MemoryContextAlloc(w->mcxt, (Size) cap * sizeof(uint32));
			memset(a->rows + a->cap, 0, (Size) (cap - a->cap) * sizeof(uint32));
			a->cap = cap;
		}
		for (uint32 i = 0; i < v->n; i++)
			if (!(v->nulls && sift_col_bit_test(v->nulls, i)))
				a->rows[w->dictmap[c][v->key[i]]]++;
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
	w->excepts = w->setting >= SIFT_COL_FORMAT_PRESENCE && !spec->table;
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
	/* the rollups carry the length of each measure (minor 36) under the setting of the compact forms of the 3.7 minors (15), else as before */
	w->pagg.measlen = w->setting >= SIFT_COL_FORMAT_PRESENCE;
	/* a leaf asks for its keys (a summary, or a key list or bitmap) or its sorted run: minor 10, 15 or 13 when some unit gets one (sift_colw_finish) */
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
	/* the option is what stamps the minor 18: the setting stays at 15 */
	if (w->setting >= SIFT_COL_FORMAT_PRESENCE && w->hll == NULL)
		w->hll = acc_new(w, spec->hll);
	/* the key tables stamp minor 22 themselves, as the setting says: under 15 the segment stays readable by 3.3 */
	if (w->setting >= SIFT_COL_FORMAT_PRESENCE && w->kt == NULL && spec->key_table)
	{
		w->kt = sift_ktab_begin(w->schema, spec->set, w->mcxt);
	}
	/* the rows of each string are written with the order of its dictionary: the option stamps minor 24, the setting stays at 15 */
	if (w->setting >= SIFT_COL_FORMAT_PRESENCE && w->cnt == NULL)
		for (int c = 0; c < w->schema->ncols; c++)
			if (spec->count[c])
			{
				if (w->cnt == NULL)
					w->cnt = MemoryContextAllocZero(w->mcxt, w->schema->ncols * sizeof(CountAcc));
			}
	/* the point directories are written with the dictionaries, minor 14 */
	if (w->setting >= SIFT_COL_FORMAT_DICTIDX && w->pt == NULL)
		for (int c = 0; c < w->schema->ncols; c++)
			if (spec->point[c] || spec->point_tid[c] || spec->gorder[c] || elem_point(w, c))
			{
				MemoryContext old = MemoryContextSwitchTo(w->mcxt);

				if (w->pt == NULL)
					w->pt = palloc0(w->schema->ncols * sizeof(PointAcc));
				MemoryContextSwitchTo(old);
				w->pt[c].want = spec->point[c] || spec->point_tid[c] || elem_point(w, c);
				w->pt[c].ord = spec->gorder[c] && w->setting >= SIFT_COL_FORMAT_PRESENCE;	/* stamps minor 21 itself, as the lists of addresses do 16 */
				w->pt[c].tids = spec->point_tid[c] && w->setting >= SIFT_COL_FORMAT_PRESENCE && !w->keyed;	/* the option is what stamps minor 16: the setting stays at 15 */
			}
}

/*
 * The units of a key-cut segment are not block ranges, so a heap address does not name its unit: its point directories list the rows of the segment
 *, whatever point_tid_columns says, and a lookup reads the unit of each row.
 */
void
sift_colw_set_keyed(SiftColSegWriter *w, uint32 aim_rows)
{
	SIFT_INVARIANT(w->nsb == 0, "sift_col: a segment is keyed before its first unit");
	w->keyed = true;
	w->aim_rows = aim_rows;
	if (w->pt)
		for (int c = 0; c < w->schema->ncols; c++)
			w->pt[c].tids = false;
}

/* the element of a list of strings (not a map): its directory lists the rows that own the value, so that an element test finds them without reading the units */
static bool
elem_point(const SiftColSegWriter *w, int c)
{
	const SiftColSchema *s = w->schema;
	const SiftColDef *d = &s->cols[c];

	return w->setting >= SIFT_COL_FORMAT_PRESENCE && d->parent >= 0 && d->kind == SIFT_COLK_SCALAR &&
		(d->store == SIFT_COLS_STR || d->type == SIFT_COLT_INT2 || d->type == SIFT_COLT_INT4 || d->type == SIFT_COLT_INT8) &&
		s->cols[d->parent].kind == SIFT_COLK_GROUP && strstr(s->cols[d->parent].path, ".*") == NULL && strstr(d->path, "#key") == NULL;
}

/* The leaf c of the segment gets a global order block sorted by (group, key) as well (the group of order_array_group: the build's own choice groups a time column only, and keeps the plain order beside it in the second slot; a group of the user's groups every block and replaces the plain order) */
static bool
gorder_grouped(const SiftColSegWriter *w, int c)
{
	return w->pagg.gorder_group > 0 && !sift_cgord_leaf_text(&w->schema->cols[c]) && (!w->pagg.gorder_group_auto || (w->setting >= SIFT_COL_FORMAT_PRESENCE && sift_cgord_leaf_time(&w->schema->cols[c])));
}

/* ... and the plain order too */
static bool
gorder_plain(const SiftColSegWriter *w, int c)
{
	return w->pagg.gorder_group == 0 || w->pagg.gorder_group_auto || sift_cgord_leaf_text(&w->schema->cols[c]);
}

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
		const SiftCPaggCol *gv = a->ord && gorder_grouped(w, c) ? &cols[w->pagg.gorder_group - 1] : NULL;	/* the group of each row */

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
		if (elem_point(w, c))
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
static void
add_preagg(SiftColSegWriter *w, const SiftColBatch *b, Size unit_bytes)
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
		SIFT_INVARIANT(!((w->format >= SIFT_COL_FORMAT_MINMAX && !(w->format == SIFT_COL_FORMAT_CALENDAR && !w->schema->kinds)) || w->set || w->run || w->hll),
					   "sift_col: a unit without its kinds in a segment of column format %d", w->run ? SIFT_COL_FORMAT_RUNS : w->set ? SIFT_COL_FORMAT_SETS : w->format);
		w->inexact = true;
		/* a schema without shredded leaves has no kinds to lose: the minor stays */
		if (w->format != SIFT_COL_FORMAT_CALENDAR || w->schema->kinds)
			w->format = sift_col_write_minor(w->schema, Min(w->format, SIFT_COL_FORMAT_TAGS), false);
	}
	Assert(b->nrows > 0 && b->schema->ncols == ncols);
	SIFT_INVARIANT(w->nsb == 0 || b->sb > w->sbdir[w->nsb - 1].id, "sift_col: superblock %u added after %u", b->sb,
				   w->nsb > 0 ? w->sbdir[w->nsb - 1].id : 0);
	if (w->nsb == w->capsb)
	{
		w->capsb *= 2;
		w->zones = repalloc_huge(w->zones, (Size) w->capsb * ncols * sizeof(SiftCZone));
		w->lagg = repalloc_huge(w->lagg, (Size) w->capsb * ncols * sizeof(SiftCLeafAgg));
		w->cubeoff = repalloc_huge(w->cubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		w->ecubeoff = repalloc_huge(w->ecubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		w->xcubeoff = repalloc_huge(w->xcubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		w->xecubeoff = repalloc_huge(w->xecubeoff, ((Size) w->capsb + 1) * sizeof(uint64));
		for (int c = 0; c < ncols; c++)
		{
			if (w->set && w->set[c].off)
				w->set[c].off = repalloc_huge(w->set[c].off, ((Size) w->capsb + 1) * sizeof(uint64));
			if (w->run && w->run[c].off)
				w->run[c].off = repalloc_huge(w->run[c].off, ((Size) w->capsb + 1) * sizeof(uint64));
			if (w->hll && w->hll[c].off)
				w->hll[c].off = repalloc_huge(w->hll[c].off, ((Size) w->capsb + 1) * sizeof(uint64));
		}
		w->pmask = repalloc_huge(w->pmask, (Size) w->capsb * SIFT_PAGEMASK_WORDS * sizeof(uint64));
		w->sbdir = repalloc_huge(w->sbdir, w->capsb * sizeof(SiftColSBRef));
		w->vunit = repalloc_huge(w->vunit, w->capsb * sizeof(VersUnit));
		memset(w->vunit + w->capsb / 2, 0, w->capsb / 2 * sizeof(VersUnit));
	}
	for (int c = 0; c < ncols; c++)
		w->inl[c] = w->dict[c] != NULL && sift_cbatch_str_inline(b, c, w->dict[c], SIFT_COL_DICT_MAX_BYTES);
	map_dicts(w, b, w->inl);
	count_rows(w, b);
	resetStringInfo(&w->buf);
	sift_cunit_encode(b, w->dictmap, w->inl, w->pol, w->format >= SIFT_COL_FORMAT_TAGS, w->keyed && w->setting >= SIFT_COL_FORMAT_PRESENCE, &w->buf, &start, w->zones + (Size) w->nsb * ncols);
	add_preagg(w, b, w->buf.len - start);
	if (b->keyed)
	{
		/* a key-cut unit has no page mask: its slot holds the first and last block of the unit (its head), which a group of units tests against the visibility map at once */
		memset(w->pmask + (Size) w->nsb * SIFT_PAGEMASK_WORDS, 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
		memcpy(w->pmask + (Size) w->nsb * SIFT_PAGEMASK_WORDS, w->buf.data + start + 4 * sizeof(uint32), 2 * sizeof(uint32));
	}
	else
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

void
sift_colw_set_index_rows(SiftColSegWriter *w, uint64 rows)
{
	w->index_rows = rows;
}

uint64
sift_colw_bytes(const SiftColSegWriter *w)
{
	return sift_sw_tell(w->sw);
}

Size
sift_colw_memory(const SiftColSegWriter *w)
{
	Size		m = (Size) w->nsb * (w->schema->ncols * (sizeof(SiftCZone) + sizeof(SiftCLeafAgg)) + sizeof(SiftColSBRef) + 2 * sizeof(uint64)) + w->cube.len + w->ecube.len + w->xcube.len + w->xecube.len;

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

/* The block of the rows kept for the global order of a leaf, sorted by key or (grouped) by group and then key; the rows with no group are not in the second */
static void
build_order(SiftColSegWriter *w, OrdBlock *ob, const PointAcc *a, bool grouped, int c)
{
	MemoryContext old = MemoryContextSwitchTo(w->mcxt);

	initStringInfo(&ob->tab);
	MemoryContextSwitchTo(old);
	if (grouped)
	{
		/* the arrays of the entries are the point directory's too, so the others are copied */
		uint32		nv = 0,
					nl = 0;
		int64	   *key = palloc((a->n + 1) * sizeof(int64)),
				   *grp = palloc((a->n + 1) * sizeof(int64)),
				   *ngrp = palloc((a->nn + 1) * sizeof(int64));
		uint64	   *tid = palloc((a->n + 1) * sizeof(uint64)),
				   *ntid = palloc((a->nn + 1) * sizeof(uint64));

		for (uint32 i = 0; i < a->n; i++)
			if (a->gok[i])
			{
				key[nv] = a->key[i];
				tid[nv] = a->tid[i];
				grp[nv++] = a->gk[i];
			}
		for (uint32 i = 0; i < a->nn; i++)
			if (a->ngok[i])
			{
				ntid[nl] = a->ntid[i];
				ngrp[nl++] = a->ngk[i];
			}
		ob->nent = nv + nl;
		ob->nvals = nv;
		if (ob->nent > 0)
			sift_cgord_build_grouped(key, tid, grp, nv, ntid, ngrp, nl, w->pagg.gorder_group - 1, &ob->tab);
		pfree(key);
		pfree(grp);
		pfree(ngrp);
		pfree(tid);
		pfree(ntid);
	}
	else
	{
		ob->nent = a->n + a->nn;
		ob->nvals = a->n;
		if (sift_cgord_leaf_text(&w->schema->cols[c]))
		{
			w->anytextord = true;
			sift_cgord_build_text(a->key, a->tid, a->n, a->ntid, a->nn, w->dict[c], w->pagg.gorder_coll[c], &ob->tab);
		}
		else
			sift_cgord_build(a->key, a->tid, a->n, a->ntid, a->nn, &ob->tab);
	}
	ob->nch = (ob->nent + sift_cgord_rows() - 1) / sift_cgord_rows();
	w->anyord = ob->nch > 0 || w->anyord;
}

#ifdef SIFT_TESTING
bool		sift_test_col_point_unpacked = false;
bool		sift_test_col_point_unrooted = false;
#endif

/* Whether the header and records of the point directories end the DICT section (column format minor 37; a testing build can turn it off to write them first) */
static bool
rooted_ok(const SiftColSegWriter *w)
{
#ifdef SIFT_TESTING
	if (sift_test_col_point_unrooted)
		return false;
#endif
	return w->setting >= SIFT_COL_FORMAT_PRESENCE;
}

/* Whether a point directory may be written as a packed table (the setting that allows the exact slot count allows it; a testing build can turn it off to write the slots) */
static bool
packed_ok(void)
{
#ifdef SIFT_TESTING
	return !sift_test_col_point_unpacked;
#else
	return true;
#endif
}

/* The blocks of the point directories, built when the last unit is in */
static void
build_points(SiftColSegWriter *w)
{
	for (int c = 0; w->pt && c < w->schema->ncols; c++)
	{
		PointAcc   *a = &w->pt[c];

		if (a->ord && !a->bad && a->n + a->nn > 0)
		{
			if (gorder_plain(w, c))
				build_order(w, &a->ob[0], a, false, c);
			if (gorder_grouped(w, c))
				build_order(w, &a->ob[gorder_plain(w, c)], a, true, c);
			w->anydual |= a->ob[1].nch > 0;
		}
		if (a->want && !a->bad && a->n > 0)
		{
			MemoryContext old = MemoryContextSwitchTo(w->mcxt);

			initStringInfo(&a->tab);
			MemoryContextSwitchTo(old);
			a->exact = w->setting >= SIFT_COL_FORMAT_PRESENCE;
			if (a->tids)
			{
				sift_cpoint_build_tids(a->key, a->tid, a->n, a->exact, a->exact && packed_ok(), &a->par, &a->tab, &a->nslots, &a->nkeys, &a->words);
				w->anytids = true;
			}
			else
			{
				sift_cpoint_build(a->key, a->row, a->n, elem_point(w, c) ? Max(SIFT_CPOINT_DENSE_ROWS, (uint32) w->rows / SIFT_CPOINT_DENSE_SHARE) : 0, a->exact, a->exact && packed_ok(), &a->par, &a->tab, &a->nslots, &a->nkeys, &a->words, &a->n);
				w->anytids |= a->nslots > 0 && elem_point(w, c);
			}
			w->anypoints |= a->nslots > 0;
			w->anypacked |= a->par.pbits > 0;
		}
		if (a->key)
		{
			pfree(a->key);
			pfree(a->row);
			if (a->tid)
				pfree(a->tid);
			a->key = NULL;
			a->row = NULL;
			a->tid = NULL;
		}
		if (a->ntid)
		{
			pfree(a->ntid);
			a->ntid = NULL;
		}
		if (a->gk)
		{
			pfree(a->gk);
			pfree(a->gok);
			a->gk = NULL;
			a->gok = NULL;
		}
		if (a->ngk)
		{
			pfree(a->ngk);
			pfree(a->ngok);
			a->ngk = NULL;
			a->ngok = NULL;
		}
	}
}

/*
 * The point directories after the dictionaries, in the DICT section: a header {u32 magic, u32 version, u32 nleaves, u32 0}, a record
 * {u32 leaf, u32 nslots, u32 rows, u32 keys, u32 words, u32 0, u64 offset in the section} for each leaf, the parameters of the packed tables, the blocks, and the
 * last 16 bytes of the section {u64 offset of the first block, u32 magic, u32 version}. From column format minor 37 (setting 15 or more) the blocks come first and
 * the parameters, the records and the header end the section, so that a lookup reads them and the footer from one page.
 */
static void
write_points(SiftColSegWriter *w, SiftSectionRef *sec)
{
	int			ncols = w->schema->ncols;
	uint64		block = sift_sw_tell(w->sw) - sec->off;
	uint32		ver = w->anyord ? SIFT_CPOINT_VERSION_ORD : w->anytids ? SIFT_CPOINT_VERSION : SIFT_CPOINT_VERSION_ROWS;
	uint32		hdr[4] = {SIFT_CPOINT_MAGIC, ver, 0, 0};
	uint32		npacked = 0;
	uint64		foot[2];
	uint32		ftail[2] = {SIFT_CPOINT_MAGIC, ver};
	uint64		at;
	bool		rooted = rooted_ok(w);
	StringInfoData recs,
				pars;

	if (!w->anypoints && !w->anyord)
		return;
	for (int c = 0; c < ncols; c++)
	{
		hdr[2] += w->pt[c].nslots > 0;
		hdr[3] += (w->pt[c].ob[0].nch > 0) + (w->pt[c].ob[1].nch > 0);
		npacked += w->pt[c].par.pbits > 0;
	}
	at = rooted ? block : block + sizeof(hdr) + (uint64) (hdr[2] + hdr[3]) * SIFT_CPOINT_RECLEN + (uint64) npacked * SIFT_CPOINT_PK_PAR;
	initStringInfo(&recs);
	initStringInfo(&pars);
	for (int c = 0; c < ncols; c++)
	{
		const PointAcc *a = &w->pt[c];
		uint32		rec[6] = {c, a->nslots, a->n, a->nkeys, a->words, (a->tids ? SIFT_CPOINT_MODE_TIDS : elem_point(w, c) ? SIFT_CPOINT_MODE_ZROWS : SIFT_CPOINT_MODE_ROWS) | (a->exact ? SIFT_CPOINT_EXACT : 0) | (a->par.pbits > 0 ? SIFT_CPOINT_PACKED : 0)};

		if (a->nslots == 0)
			continue;
		appendBinaryStringInfo(&recs, (char *) rec, sizeof(rec));
		appendBinaryStringInfo(&recs, (char *) &at, sizeof(at));
		at += a->tab.len;
	}
	/* the plain blocks by leaf, then the blocks sorted by group by leaf (the slot ncols + leaf of the reader) */
	for (int k = 0; k < 2 * ncols; k++)
	{
		const OrdBlock *o = &w->pt[k % ncols].ob[k / ncols];
		uint32		rec[6] = {k, o->nch, o->nent, o->nvals, (uint32) (o->tab.len / 8), SIFT_CPOINT_MODE_ORDER};

		if (o->nch == 0)
			continue;
		appendBinaryStringInfo(&recs, (char *) rec, sizeof(rec));
		appendBinaryStringInfo(&recs, (char *) &at, sizeof(at));
		at += o->tab.len;
	}
	for (int c = 0; c < ncols; c++)
		if (w->pt[c].par.pbits > 0)
			appendBinaryStringInfo(&pars, (char *) &w->pt[c].par, SIFT_CPOINT_PK_PAR);
	if (!rooted)
	{
		sift_sw_write(w->sw, hdr, sizeof(hdr));
		sift_sw_write(w->sw, recs.data, recs.len);
		sift_sw_write(w->sw, pars.data, pars.len);
	}
	for (int c = 0; c < ncols; c++)
		if (w->pt[c].nslots > 0)
			sift_sw_write(w->sw, w->pt[c].tab.data, w->pt[c].tab.len);
	for (int k = 0; k < 2 * ncols; k++)
		if (w->pt[k % ncols].ob[k / ncols].nch > 0)
			sift_sw_write(w->sw, w->pt[k % ncols].ob[k / ncols].tab.data, w->pt[k % ncols].ob[k / ncols].tab.len);
	if (rooted)
	{
		sift_sw_write(w->sw, pars.data, pars.len);
		sift_sw_write(w->sw, recs.data, recs.len);
		sift_sw_write(w->sw, hdr, sizeof(hdr));
	}
	pfree(recs.data);
	pfree(pars.data);
	foot[0] = block;
	memcpy(&foot[1], ftail, sizeof(ftail));
	sift_sw_write(w->sw, foot, sizeof(foot));
}

/*
 * Whether the dictionary of leaf c no longer fits the cache of parsed dictionaries (dict_cache_kb): its strings, parsed and scaled to the rows of the index (the
 * segments of an index are alike), pass the cache, or this segment's alone pass the half of it that a single dictionary may take. A dictionary that fits is
 * parsed once and kept; one that does not is read, and unwrapped, again by every test that needs it
 */
static bool
dict_uncached(const SiftColSegWriter *w, int c)
{
	Size		cache = sift_cdict_cache_budget(),
				parsed = sift_cdict_parsed_bytes(w->dict[c]->nbytes, w->dict[c]->n);
	double		scale = Max((double) w->index_rows / (double) Max(sift_colw_rows(w), 1), 1.0);

	return parsed > cache / 2 || (double) parsed * scale > (double) cache;
}

/*
 * Whether the dictionary of leaf c gets the trigram signatures (and so the blocks form, whatever the budget of the blocks): when it no longer fits the cache, and
 * it holds dict_tsig_min bytes of strings of 5 bytes or more in the segment. An index that fits gets none
 */
static bool
tsig_wanted(const SiftColSegWriter *w, int c)
{
	if (w->setting < SIFT_COL_FORMAT_PRESENCE || w->dict[c]->nbytes < (Size) sift_limit(dict_tsig_min, CDICT_TSIG_MIN_KB) * 1024 ||
		w->dict[c]->nbytes < (Size) 5 * w->dict[c]->n)
		return false;
	return dict_uncached(w, c);
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
	bool	   *asked = palloc0(ncols * sizeof(bool));
	const SiftCcPolicy *pol;
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
	for (int c = 0; c < ncols; c++)
		if (w->dict[c] != NULL)
		{
			w->dict[c]->scan = w->setting >= SIFT_COL_FORMAT_PRESENCE && dict_uncached(w, c);
			w->dict[c]->leaf1 = c + 1;
		}
	for (int c = 0; blocks && c < ncols; c++)
	{
		if (w->dict[c] == NULL || w->dict[c]->n < sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN))
			extra[c] = SIZE_MAX;
		else
			extra[c] = sift_cdict_blocks_extra(w->dict[c], w->pol ? &w->pol[c] : NULL, &compact[c]);
		asked[c] = w->dform && w->dform[c];
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
		if (extra[c] != SIZE_MAX && blocks && !asked[c])
		{
			if (extra[c] <= budget || tsig_wanted(w, c))
			{
				form = DICT_BLOCKS;
				budget -= Min(extra[c], budget);
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
					w->format = Max(w->format, w->dict[c]->lz4_blocks ? SIFT_COL_FORMAT_TSIG : SIFT_COL_FORMAT_DICTIDX);
				}
				else
					appendBinaryStringInfo(&blob, whole.data, whole.len);
				pfree(whole.data);
				pfree(alt.data);
				continue;
			}
		}
		/* a dictionary in blocks that no leaf asked to search keeps the hash alone (5 bytes a string): the first equality of a backend finds its code in a few pages, not by parsing the dictionary */
		if (form == DICT_BLOCKS && w->pagg.dict_hash && w->setting >= SIFT_COL_FORMAT_PRESENCE && w->dict[c]->n >= sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN))
		{
			w->dict[c]->hashed = true;
			w->format = Max(w->format, SIFT_COL_FORMAT_HASH);
		}
		if (form == DICT_BLOCKS)
			w->format = Max(w->format, SIFT_COL_FORMAT_DICTIDX);
		/* a dictionary of long strings read in blocks gets the trigram signature of each (a substring test then reads the blocks that may hold the part) */
		w->dict[c]->tsig = (form == DICT_BLOCKS || form == DICT_LOOKUP || form == DICT_LOOKUP_COUNT) && tsig_wanted(w, c);
		if (w->dict[c]->tsig)
			w->format = Max(w->format, SIFT_COL_FORMAT_TSIG);
		pol = w->pol ? &w->pol[c] : NULL;
		sift_cdict_serialize(w->dict[c], pol, form, &blob);
		if (w->dict[c]->lz4_blocks)
			w->format = Max(w->format, SIFT_COL_FORMAT_TSIG);
	}
	pfree(extra);
	pfree(compact);
	pfree(asked);
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
	/* what the merge of one group allocates is freed with the group: a segment of many units has thousands of groups */
	cx = AllocSetContextCreate(w->mcxt, "sift col superunits", ALLOCSET_DEFAULT_SIZES);
	ua = palloc(fan * sizeof(SiftCAgg));
	cdir = palloc((nsu + 1) * sizeof(uint64));
	initStringInfo(&cubes);
	old = MemoryContextSwitchTo(cx);
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
		MemoryContextReset(cx);
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
	pfree(ua);
	pfree(cdir);
	pfree(cubes.data);
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
			   *runs = NULL,
			   *hlls = NULL;
	int			nsets = 0,
				nruns = 0,
				nhlls = 0;
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
		at = start + bytes;
	}
	if (ver > 5 && w->anyexc)
	{
		at = ALIGN8(at);
		PAGG_XCUBEDIR(h) = at;
		PAGG_XCUBE_BYTES(h) = w->xcube.len;
		at = ALIGN8(at + (nsb + 1) * sizeof(uint64) + w->xcube.len);
		if (w->xecube.len > 0)
		{
			PAGG_XECUBEDIR(h) = at;
			PAGG_XECUBE_BYTES(h) = w->xecube.len;
			at = ALIGN8(at + (nsb + 1) * sizeof(uint64) + w->xecube.len);
		}
	}
	if (ver > 6 && w->anyhll)
	{
		Size		start,
					bytes;

		hlls = layout_blobs(w, w->hll, at, &start, &bytes, &nhlls);
		PAGG_HLL(h) = start;
		PAGG_HLL_BYTES(h) = bytes;
		at = start + bytes;
	}
	if (ver > 7 && w->ktab.len > 0)
	{
		PAGG_KTAB(h) = ALIGN8(at);
		PAGG_KTAB_BYTES(h) = w->ktab.len;
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
	if (ver > 5 && w->anyexc)
	{
		sw_pad8(w->sw);
		sift_sw_write(w->sw, w->xcubeoff, (Size) (nsb + 1) * sizeof(uint64));
		sift_sw_write(w->sw, w->xcube.data, w->xcube.len);
		if (w->xecube.len > 0)
		{
			sw_pad8(w->sw);
			sift_sw_write(w->sw, w->xecubeoff, (Size) (nsb + 1) * sizeof(uint64));
			sift_sw_write(w->sw, w->xecube.data, w->xecube.len);
		}
	}
	if (hlls)
		write_blobs(w, w->hll, HLL_MAGIC, hlls, nhlls);
	if (ver > 7 && w->ktab.len > 0)
	{
		sw_pad8(w->sw);
		sift_sw_write(w->sw, w->ktab.data, w->ktab.len);
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
	if (w->nsb >= sift_limit(lst_min_units, LST_MIN_UNITS))
	{
		uint32		ns = sift_lst_nsample(w->nsb);
		Size		len = sizeof(LstHeader) + ncols * sizeof(LstLeaf);
		LstHeader  *lh;

		for (int c = 0; c < ncols; c++)
			if (sift_lst_sampled(&w->schema->cols[c]))
				len += 2 * (Size) ns * sizeof(int64);
		w->lst = palloc0(len);
		w->lstlen = len;
		lh = (LstHeader *) w->lst;
		*lh = (LstHeader) {.magic = LST_MAGIC,.ncols = ncols,.nsb = w->nsb,.nsample = ns,.len = len};
	}
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
		if (w->lst != NULL)
		{
			LstHeader  *lh = (LstHeader *) w->lst;
			LstLeaf    *rec = (LstLeaf *) (w->lst + sizeof(LstHeader)) + c;
			ZoneArrays	za = {.min = (const int64 *) mn,.max = (const int64 *) mx,.sum = (const int64 *) sum,.nnull = nnull,.nvals = nvals,.flags = flags};
			int64	   *lo = NULL;

			for (uint32 u = 0; u < w->nsb; u++)
				za.any_ovf |= (flags[u] & SIFT_COLZ_OVERFLOW) != 0, za.any_inl |= (flags[u] & SIFT_COLZ_INLINE) != 0;
			if (sift_lst_sampled(def))
			{
				Size		sat = sizeof(LstHeader) + ncols * sizeof(LstLeaf);

				for (int k = 0; k < c; k++)
					if (sift_lst_sampled(&w->schema->cols[k]))
						sat += 2 * (Size) lh->nsample * sizeof(int64);
				lo = (int64 *) (w->lst + sat);
			}
			sift_lst_leaf(def, w->format, w->nsb, &za, rec, lo, lo != NULL ? lo + lh->nsample : NULL);
			if (lo != NULL)
				rec->sample_off = (uint8 *) lo - w->lst;
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
	if (w->anyrunl)
		w->format = Max(w->format, SIFT_COL_FORMAT_RUNL);
	if (w->anyruns)
		w->format = Max(w->format, w->anyheads ? SIFT_COL_FORMAT_HEADS : SIFT_COL_FORMAT_RUNS);
	if (w->anyexc)
		w->format = Max(w->format, SIFT_COL_FORMAT_EXCEPT);
	if (w->anyhll)
		w->format = Max(w->format, SIFT_COL_FORMAT_HLL);
	build_points(w);
	if (w->anypoints)
		w->format = Max(w->format, w->setting >= SIFT_COL_FORMAT_PRESENCE ? SIFT_COL_FORMAT_EXACTSLOT : w->anytids ? SIFT_COL_FORMAT_POINTTID : SIFT_COL_FORMAT_DICTIDX);
	if (w->anypacked)
		w->format = Max(w->format, SIFT_COL_FORMAT_PACKED);
	if ((w->anypoints || w->anyord) && rooted_ok(w))
		w->format = Max(w->format, SIFT_COL_FORMAT_ROOT);
	if (w->anyord)
		w->format = Max(w->format, sift_cgord_rows() == SIFT_CGORD_ROWS_FINE ? SIFT_COL_FORMAT_FINEORD : SIFT_COL_FORMAT_GORDER);
	if (w->anydual)
		w->format = Max(w->format, SIFT_COL_FORMAT_GROUPORD);
	if (w->anytextord)
		w->format = Max(w->format, SIFT_COL_FORMAT_TEXTORD);
	if (w->keyed)
		w->format = Max(w->format, w->setting >= SIFT_COL_FORMAT_PRESENCE ? SIFT_COL_FORMAT_TABFIRST : SIFT_COL_FORMAT_KEYED);
	if (w->schema->contain)
		w->format = Max(w->format, SIFT_COL_FORMAT_CONTAIN);
	if (w->anymeaslen)
		w->format = Max(w->format, SIFT_COL_FORMAT_MEASLEN);
	/* a dictionary of many strings is written with the order to be searched in place when the setting allows it, the leaf asks for it (spec.lookup) and the kinds are complete */
	if (w->setting >= SIFT_COL_FORMAT_DICTIDX && !w->inexact)
	{
		Size		order_bytes = 0,		/* the orders kept so far, for the cap of a sift_table */
					seg_bytes = sift_sw_tell(w->sw);	/* the segment before its dictionaries, then with the compact form of them */

		w->dform = palloc0(w->schema->ncols * sizeof(uint8));
		if (w->pagg.lookup_pct != 0)
			for (int c = 0; c < w->schema->ncols; c++)
				if (w->dict[c] != NULL)
					seg_bytes += w->dict[c]->nbytes + ((Size) w->dict[c]->n + 1) * sizeof(uint32);
		for (int c = 0; c < w->schema->ncols; c++)
			if (w->dict[c] != NULL && w->pagg.lookup[c] && w->dict[c]->n >= sift_limit(dict_lookup_min, CDICT_LOOKUP_MIN) &&
				(w->pagg.lookup_pct == 0 || (order_bytes + (Size) w->dict[c]->n * sizeof(uint32)) * 100 <= (Size) w->pagg.lookup_pct * seg_bytes))
			{
				order_bytes += (Size) w->dict[c]->n * sizeof(uint32);
				w->dform[c] = DICT_LOOKUP;
				w->format = Max(w->format, SIFT_COL_FORMAT_DICTIDX);
				if (w->setting >= SIFT_COL_FORMAT_PRESENCE)
				{
					w->dict[c]->hashed = true;
					w->format = Max(w->format, SIFT_COL_FORMAT_HASH);
				}
				if (w->cnt != NULL && w->pagg.count[c] && !w->cnt[c].bad && w->cnt[c].cap >= w->dict[c]->n)
				{
					w->dict[c]->rows = w->cnt[c].rows;
					w->format = Max(w->format, SIFT_COL_FORMAT_COUNT);
				}
			}
	}
	h.sections[SIFT_SEC_POSTINGS] = (SiftSectionRef) {0, sift_sw_tell(w->sw)};
	write_dicts(w, &h.sections[SIFT_SEC_DICT]);
	initStringInfo(&w->ktab);
	if (w->kt)
		/* 3% of the segment as written so far: the chunks and the dictionaries */
		sift_ktab_finish(w->kt, sift_sw_tell(w->sw), &w->ktab);
	if (w->ktab.len > 0)
		w->format = Max(w->format, SIFT_COL_FORMAT_KTAB);
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
	/* the leaf statistics follow the last section, where no reader of an older minor looks (the directory entry says they are there); 3% of the segment at most */
	if (w->lst != NULL && w->lstlen * sift_limit(lst_share_div, LST_SHARE_DIV) <= sift_sw_tell(w->sw))
	{
		sw_pad8(w->sw);
		sift_sw_write(w->sw, w->lst, w->lstlen);
	}
	else
		w->lst = NULL;
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
	if (w->keyed)
		entry->flags |= SIFT_SEG_KEYED;
	if (w->lst != NULL)
		entry->flags |= SIFT_SEG_LSTATS;
	entry->ndocs = w->rows;
	entry->bytes = blocks * BLCKSZ;
	pfree(schema.data);
}
