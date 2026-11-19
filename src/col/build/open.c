/*
 * col/build/open.c - open units: the rows of a value of the key that fills units are encoded into the open segment as they arrive, with no temporary file
 *
 * A build with key_cut whose rows are not in the order of the key puts them in key order by ranges of the key in temporary files (col/build/spill.c). A value of the key that holds many
 * units of rows needs none of that: its rows are written as units of their own as soon as a unit of them is complete, one encode and one write, so the value lies in the units its rows
 * need and nothing of it goes through a file. The values the sample of the key says hold at least a few units each (as many as the memory of the open units holds) have an open unit each
 * (col/build/ranges.c: a range of one value); the rows of every other value go on as before, to the files of the key ranges or to the runs. The rows left in the open units when the scan
 * ends join the rows of the other values and are sorted with them, so no unit is written short.
 *
 * In the mode where the files of the key ranges are not allowed (zero_temp) every range is an open unit: a unit of a range holds several values of the key, sorted, and a value lies in all
 * the units of its range.
 *
 * A unit of one value that is complete waits in memory, as the packed rows it was filled with, until the units that wait take the memory of a segment: they are then encoded in the order of
 * their keys into a segment that is a run like any other (it records its cut, and a read finds a key in it by the zones). With no files a unit holds the rows of a range of several values,
 * and two units of a range overlap in the key, so those units are encoded as they are complete into a segment that records no cut (sift_colw_set_arrival), which a merge sorts.
 */
#include "postgres.h"
#include "col/build/build_int.h"
#include "col/rowfile.h"
#include "core/membudget.h"

#define OPEN_CAP_DIV		4	/* the open units hold a quarter of the memory of the open path, its writer a half, the encode of a unit the rest */
#define OPEN_WRITER_DIV	2
#define OPEN_WORK_DIV	4
#define OPEN_MIN_SHARE	0.99	/* the open units take this share of the rows at least: a read of a value also opens the runs of the others' files (3% of the rows in files: +41% blocks, 10%: +21 to 26%, 20%: +124 to 192%; none: +4 to 6%) */
#define OPEN_MIN_UNITS	8	/* with no files, a value of the key has an open unit when its rows fill this many units */
#define OPEN_KEPT_MIN_UNITS 32	/* a value of the key has an open unit with files only when its rows fill this many units: its last unit is short and a read of the value opens it, so a value of fewer units reads more than from the files (30 values of 20 units: +10%) */
#define OPEN_WRITER_RUNS	1.0	/* the open units are written in one run: a value lies in every run, and each is a segment a read opens (ten values, 300,000 rows: 2.9 runs +73% instructions, 1.4 +37%, 0.7 +1.4%) */
#define OPEN_SLOT_PACKED 1.75	/* a packed open unit holds this many times the bytes of a unit: the block it fills and the blocks it has compressed */
#define OPEN_WORK_UNITS	4	/* encoding a unit takes about this many raw units of memory: the rows decoded from the packed form, and the cut of the unit */

#ifdef SIFT_TESTING
int			sift_test_col_open = 0;	/* -1 none, 0 as the build decides, 1 the values that fill units whatever the others and the runs, 2 every range (no files) */
int			sift_test_col_open_kb = 0;
int			sift_test_col_open_single = -1;
int			sift_test_col_open_mutant = 0;	/* 1 a value joins the unit of another, 2 the tail units are lost, 3 a unit is written twice, 4 the cap of the open units is not looked at, 5 the units that wait are not counted out when they are written, 6 the units that wait are never written before the end */
char		sift_test_col_open_stats[256];
#endif

#define MUTANT_MIXED		1	/* a value joins the unit of another */
#define MUTANT_TAIL_LOST	2	/* the rows the open units leave for the last run are dropped */
#define MUTANT_TWICE		3	/* a unit is written twice */
#define MUTANT_CAP_IGNORED	4	/* the cap of the open units is not looked at */
#define MUTANT_STAGED_KEPT	5	/* the units that wait are not counted out when they are written */
#define MUTANT_NEVER_WRITTEN	6	/* the units that wait are written only at the end */

static bool
mutant(int which)
{
#ifdef SIFT_TESTING
	return sift_test_col_open_mutant == which;
#else
	return false;
#endif
}

typedef struct OpenStaged
{
	int			range;
	uint64		seq;			/* the order the unit was complete in: units of one range stay in the order of their rows */
	uint32		nrows;
	MemoryContext cx;
	SiftRowFile *pk;
	Size		bytes;
} OpenStaged;

struct ColOpen
{
	ColRanges	g;
	bool		zero_temp;		/* every range is an open unit, none goes to a file */
	double		est_runs;		/* the runs the open units are written in: their packed bytes over the writer's budget */
	uint32		unit_rows;
	Size		raw_unit,
				packed_unit;	/* the bytes a unit of rows takes in an open unit, raw and packed */
	Size		mem;			/* the memory of the open units, the open segment's writer and the encode of a unit: the build's budget less what the files of the key ranges keep */
	Size		cap;			/* the part of it the rows of the open units may hold */
	Size		wbudget;		/* the part the writer of the open segment may hold before the segment ends */
	Size		total,
				peak,
				wpeak,
				upeak,
				xpeak;
	Size		block;
	StringInfoData scratch;		/* the blocks of the packed open units are compressed into it */
	SiftColRow	row;
	SiftRowFile **pk;			/* the rows of the open unit of range i, packed as the spool packs them */
	uint32	   *nrows;
	MemoryContext *cx;
	Size	   *bytes;
	MemoryContext pcx;			/* the open units: a child for each range */
	MemoryContext ucx;			/* the cut of one unit */
	MemoryContext xcx;			/* the batch a packed unit is decoded into */
	SiftColSegWriter *w;		/* with no files: the open segment */
	MemoryContext wcxt;
	OpenStaged *stage;			/* the complete units that wait for their segment */
	int			nstage;
	int			capstage;
	Size		staged;
	uint64		seq;
	int			nsingle;
	uint64		open_rows;		/* the rows the sample says the open units take */
	List	   *left;			/* the rows the open units hold when the scan ends, in batches for the last run */
	MemoryContext lcx;
	int64	   *tail;			/* the sample without the keys of the open units */
	uint32		ntail;
	uint64		rows,
				units,
				leftover_rows,
				evictions;
};

static bool
slot_open(const ColOpen *o, int i)
{
	return o->cx[i] != NULL;
}

static int
open_first_single(const ColOpen *o)
{
	int			i = 0;

	while (!o->g.single[i])
		i++;
	return i;
}

static bool
slot_wanted(const ColOpen *o, int i)
{
	return o->zero_temp || o->g.single[i];
}

static void
open_new(const ColBuild *b, ColOpen *o, int i)
{
	MemoryContext old;

	o->cx[i] = AllocSetContextCreate(o->pcx, "sift_col build open unit", ALLOCSET_SMALL_SIZES);
	o->nrows[i] = 0;
	old = MemoryContextSwitchTo(o->cx[i]);
	o->pk[i] = sift_rowfile_create(b->r->schema, &o->scratch, SIFT_ROWS_MEMORY, o->block, false);
	MemoryContextSwitchTo(old);
}

/* The rows of a packed open unit (sealed) as a batch in the context xcx, which the caller resets */
static SiftColBatch *
open_unpack(const ColBuild *b, ColOpen *o, SiftRowFile *pk)
{
	SiftColBatch *x = sift_cbatch_create(o->xcx, b->r->schema);
	MemoryContext old;
	uint32		xmin,
				cmin;

	x->keyed = x->unsorted = true;
	old = MemoryContextSwitchTo(o->xcx);
	while (sift_rowfile_get(pk, &o->row, &xmin, &cmin))
		sift_cbatch_add(x, &o->row);
	MemoryContextSwitchTo(old);
	return x;
}

static void
open_end_segment(ColBuild *b, SiftColSegWriter *w, MemoryContext wcxt)
{
	colb_writer_end(b, w, wcxt);
	b->lvl[b->nsegs - 1] = 0;
}

/* A unit of n rows, in buf or sealed in pk, is encoded into the segment of w */
static void
open_write(ColBuild *b, ColOpen *o, SiftColSegWriter *w, MemoryContext wcxt, SiftRowFile *pk, uint32 n)
{
	SiftColBatch *buf = open_unpack(b, o, pk);
	MemoryContext old;

	o->units++;
	old = MemoryContextSwitchTo(wcxt);
	b->unit_base += sift_colk_write(w, &buf, 1, b->r->schema, b->r->cut, b->r->ncut, Max(o->unit_rows, n), o->ucx, b->unit_base);
	if (mutant(MUTANT_TWICE))
		b->unit_base += sift_colk_write(w, &buf, 1, b->r->schema, b->r->cut, b->r->ncut, Max(o->unit_rows, n), o->ucx, b->unit_base);
	MemoryContextSwitchTo(old);
	o->wpeak = Max(o->wpeak, MemoryContextMemAllocated(wcxt, true));
	o->upeak = Max(o->upeak, MemoryContextMemAllocated(o->ucx, true));
	o->xpeak = Max(o->xpeak, MemoryContextMemAllocated(o->xcx, true));
	MemoryContextReset(o->ucx);
	MemoryContextReset(o->xcx);
	sift_colb_peak_note();
}

/* The slot's rows are complete: its last block is sealed, so that what it holds from now on is what it takes */
static void
open_seal(ColOpen *o, int i)
{
	sift_rowfile_seal(o->pk[i]);
	o->total += MemoryContextMemAllocated(o->cx[i], false) - o->bytes[i];
	o->bytes[i] = MemoryContextMemAllocated(o->cx[i], false);
}

static void
open_release(ColOpen *o, int i)
{
	MemoryContextDelete(o->cx[i]);
	o->total -= o->bytes[i];
	o->bytes[i] = 0;
	o->pk[i] = NULL;
	o->cx[i] = NULL;
	o->nrows[i] = 0;
}

/* The rows of range i that make no unit are kept for the last run, which sorts them with the rows of the other values */
static void
open_leave(ColBuild *b, ColOpen *o, int i)
{
	SiftColBatch *keep = sift_cbatch_create(o->lcx, b->r->schema);
	SiftColBatch *rows;

	open_seal(o, i);
	rows = open_unpack(b, o, o->pk[i]);
	keep->keyed = keep->unsorted = true;
	for (uint32 r = 0; r < rows->nrows; r++)
		sift_cbatch_add_from(keep, rows, r);
	sift_cbatch_trim(keep);
	o->left = lappend(o->left, keep);
	o->leftover_rows += keep->nrows;
	MemoryContextReset(o->xcx);
}

/* With no files: the unit of range i goes to the open segment as it is, and the segment ends when its writer holds its budget */
static void
open_append(ColBuild *b, ColOpen *o, int i)
{
	open_seal(o, i);
	if (o->w == NULL)
	{
		o->w = colb_writer_begin(b, &o->wcxt);
		sift_colw_set_arrival(o->w);
	}
	open_write(b, o, o->w, o->wcxt, o->pk[i], o->nrows[i]);
	open_release(o, i);
	if (sift_colw_full(o->w, o->wbudget))
	{
		open_end_segment(b, o->w, o->wcxt);
		o->w = NULL;
		colb_make_room(b);
	}
}

static int
stage_cmp(const void *a, const void *b)
{
	const OpenStaged *x = a,
			   *y = b;

	return x->range != y->range ? (x->range < y->range ? -1 : 1) : (x->seq < y->seq ? -1 : x->seq > y->seq);
}

/* The units that wait are encoded in the order of their keys into a segment of their own: a run like those of the files of the key ranges */
static void
open_flush(ColBuild *b, ColOpen *o)
{
	MemoryContext wcxt;
	SiftColSegWriter *w;

	if (o->nstage == 0)
		return;
	qsort(o->stage, o->nstage, sizeof(OpenStaged), stage_cmp);
	b->aim = b->spill_aim;
	w = colb_writer_begin(b, &wcxt);
	for (int k = 0; k < o->nstage; k++)
	{
		OpenStaged *u = &o->stage[k];

		open_write(b, o, w, wcxt, u->pk, u->nrows);
		MemoryContextDelete(u->cx);
	}
	open_end_segment(b, w, wcxt);
	o->nstage = 0;
	if (!mutant(MUTANT_STAGED_KEPT))
		o->staged = 0;
	colb_make_room(b);
}

/* The unit of range i, complete or evicted, waits for its segment */
static void
open_stage(ColBuild *b, ColOpen *o, int i)
{
	OpenStaged *u;

	open_seal(o, i);
	if (o->nstage == o->capstage)
	{
		o->capstage = Max(o->capstage * 2, 16);
		o->stage = o->stage == NULL ? MemoryContextAlloc(b->cxt, o->capstage * sizeof(OpenStaged)) : repalloc(o->stage, o->capstage * sizeof(OpenStaged));
	}
	u = &o->stage[o->nstage++];
	*u = (OpenStaged) {.range = i, .seq = o->seq++, .nrows = o->nrows[i], .cx = o->cx[i], .pk = o->pk[i], .bytes = o->bytes[i]};
	o->staged += u->bytes;
	o->total -= u->bytes;
	o->bytes[i] = 0;
	o->pk[i] = NULL;
	o->cx[i] = NULL;
	o->nrows[i] = 0;
	if (o->staged > o->wbudget && !mutant(MUTANT_NEVER_WRITTEN))
		open_flush(b, o);
}

/* The unit of range i is done: it waits for its segment, or with no files it goes to the open segment */
static void
open_done(ColBuild *b, ColOpen *o, int i)
{
	if (o->zero_temp)
		open_append(b, o, i);
	else
		open_stage(b, o, i);
}

static int
open_victim(const ColOpen *o)
{
	int			v = -1;

	for (int i = 0; i < o->g.n; i++)
		if (slot_open(o, i) && (v < 0 || o->bytes[i] > o->bytes[v]))
			v = i;
	return v;
}

/* A row joined the open unit of range i: a unit that is complete is done, and while the open units hold more than their memory the largest is done too, short */
static void
open_settle(ColBuild *b, ColOpen *o, int i)
{
	Size		now = MemoryContextMemAllocated(o->cx[i], false);

	o->total += now - o->bytes[i];
	o->bytes[i] = now;
	o->peak = Max(o->peak, o->total);
	o->rows++;
	if (o->nrows[i] >= o->unit_rows)
		open_done(b, o, i);
	while (o->total > (mutant(MUTANT_CAP_IGNORED) ? 8 * o->cap : o->cap))
	{
		o->evictions++;
		open_done(b, o, open_victim(o));
	}
}

/* The row of the scan, if its value of the key has an open unit: true, and the row is in it */
bool
colb_open_take(ColBuild *b, const SiftColRow *row)
{
	ColOpen    *o = b->open;
	int			i = colb_range_of_value(&o->g, &row->val[o->g.leaf]);
	MemoryContext old;

	if (!slot_wanted(o, i))
		return false;
	if (mutant(MUTANT_MIXED) && (o->rows & 1))
		i = open_first_single(o);
	SIFT_MEM_SCOPE(col_build_open, o->pcx);
	if (!slot_open(o, i))
		open_new(b, o, i);
	old = MemoryContextSwitchTo(o->cx[i]);
	sift_rowfile_put(o->pk[i], row, 0, 0);
	MemoryContextSwitchTo(old);
	o->nrows[i]++;
	open_settle(b, o, i);
	SIFT_MEM_CHECKPOINT();
	return true;
}

/* The rows of the batches the first run holds that have no open unit stay held; the others go to their open units */
void
colb_open_split_held(ColBuild *b)
{
	ColOpen    *o = b->open;
	ListCell   *lc;
	SiftColRow	row = {0};

	b->held_rows = 0;
	foreach(lc, b->held)
	{
		const SiftColBatch *batch = lfirst(lc);
		SiftColBatch *rest = sift_cbatch_create(b->heldcxt, b->r->schema);

		rest->keyed = rest->unsorted = true;
		for (uint32 r = 0; r < batch->nrows; r++)
		{
			if (slot_wanted(o, colb_range_of_row(&o->g, batch, r)))
			{
				sift_cbatch_row(batch, r, &row);
				(void) colb_open_take(b, &row);
			}
			else
				sift_cbatch_add_from(rest, batch, r);
		}
		sift_cbatch_release((SiftColBatch *) batch);
		sift_cbatch_trim(rest);
		lfirst(lc) = rest;
		b->held_rows += rest->nrows;
	}
	sift_cbatch_row_free(&row);
}

#ifdef SIFT_TESTING
static void
open_say(const ColOpen *o)
{
	snprintf(sift_test_col_open_stats, sizeof(sift_test_col_open_stats), "ranges=%d single=%d est_runs=%.2f est_rows=%llu rows=%llu units=%llu leftover=%llu evictions=%llu raw_kb=%zu pk_kb=%zu mem_kb=%zu peak_kb=%zu w_kb=%zu u_kb=%zu x_kb=%zu",
			 o->g.n, o->nsingle, o->est_runs, (unsigned long long) o->open_rows, (unsigned long long) o->rows, (unsigned long long) o->units, (unsigned long long) o->leftover_rows,
			 (unsigned long long) o->evictions, o->raw_unit / 1024, o->packed_unit / 1024, o->mem / 1024, o->peak / 1024, o->wpeak / 1024, o->upeak / 1024, o->xpeak / 1024);
}
#endif

/* The scan is over: the rows of the open units that make no unit are kept for the last run (with no files they are units, short), the units that wait are written */
void
colb_open_finish(ColBuild *b)
{
	ColOpen    *o = b->open;

	for (int i = 0; i < o->g.n; i++)
	{
		if (!slot_open(o, i))
			continue;
		if (o->nrows[i] == 0)
			open_release(o, i);
		else if (o->zero_temp)
			open_append(b, o, i);
		else
		{
			if (!mutant(MUTANT_TAIL_LOST))
				open_leave(b, o, i);
			open_release(o, i);
		}
	}
	open_flush(b, o);
	if (o->w != NULL)
		open_end_segment(b, o->w, o->wcxt);
#ifdef SIFT_TESTING
	open_say(o);
#endif
	MemoryContextDelete(o->pcx);
	MemoryContextDelete(o->ucx);
	MemoryContextDelete(o->xcx);
}

/* The last run takes the rows the open units left, after the files of the key ranges have been read back */
void
colb_open_adopt(ColBuild *b)
{
	ColOpen    *o = b->open;
	ListCell   *lc;

	foreach(lc, o->left)
	{
		SiftColBatch *x = lfirst(lc);

		b->held = lappend(b->held, x);
		b->held_rows += x->nrows;
	}
	b->open = NULL;
}

/* Whether the build may have open units: a key_cut build by one process that reads the heap (sift.col_build_source = heap turns them off) */
bool
colb_open_allowed(const ColBuild *b)
{
	if (b->shared || b->scope_leaf >= 0 || OidIsValid(b->btree) || sift_col_build_source == SIFT_BUILD_SOURCE_HEAP)
		return false;
#ifdef SIFT_TESTING
	return sift_test_col_open >= 0;
#else
	return true;
#endif
}

/* The values of the key that have open units, said when the build starts */
void
colb_open_notice(const ColBuild *b, const ColOpen *o)
{
	ereport(NOTICE, (errmsg("sift_col index \"%s\": %d values of the key fill units, their rows need no temporary file",
							sift_col_final_name(b->index), o->nsingle),
					 errhint("SET sift.col_build_source = heap reads the heap, as releases before 5.1 did.")));
}

/* Every range is an open unit whatever room the files have (testing) */
bool
colb_open_forced_zero_temp(void)
{
#ifdef SIFT_TESTING
	return sift_test_col_open == 2;
#else
	return false;
#endif
}

bool
colb_open_zero_temp(const ColOpen *o)
{
	return o->zero_temp;
}

/* What the sample leaves for the files of the key ranges: its keys that have no open unit, and the rows they stand for */
void
colb_open_tail(const ColOpen *o, const ColBuild *b, const int64 **sample, uint32 *n, uint64 *entries)
{
	*sample = o->tail;
	*n = o->ntail;
	*entries = b->entries - Min(o->open_rows, b->entries);
}

#ifdef SIFT_TESTING
#define open_forced()	(sift_test_col_open == 1)
#else
#define open_forced()	false
#endif

/* The files of the key ranges keep what the open path does not take: a quarter of the build's memory, and as much more as the share of the rows that the open units take */
static void
open_share(const ColBuild *b, ColOpen *o)
{
	double		f = Min((double) o->open_rows / Max(b->entries, 1), 1.0);

	o->mem = (Size) ((double) b->budget * (0.25 + f / 2));
	o->cap = o->mem / OPEN_CAP_DIV;
#ifdef SIFT_TESTING
	if (sift_test_col_open_kb > 0)
		o->cap = (Size) sift_test_col_open_kb * 1024;
#endif
	o->wbudget = o->mem / OPEN_WRITER_DIV;
}

Size
colb_open_mem(const ColOpen *o)
{
	return o->mem;
}

/*
 * The open units of the build, from the sample of the key: the values of the key that hold at least OPEN_KEPT_MIN_UNITS units of rows (as many of them as the memory of the open units holds,
 * packed) that go in at most OPEN_WRITER_RUNS runs, or with no files every range. NULL when no value qualifies, when a read of them would open more runs, or when the memory of the path does not hold the encode of a unit. mem is the memory the build gives the path, raw_unit
 * and packed_unit the bytes a unit of rows takes decoded and packed.
 */
ColOpen *
colb_open_plan(ColBuild *b, int leaf, uint32 unit_rows, Size mem, double raw_unit, double packed_unit, int nranges, bool zero_temp)
{
	int			max_single;
	MemoryContext old;
	ColOpen    *o;

	if (b->ksample == NULL || b->entries == 0)
		return NULL;
	o = MemoryContextAllocZero(b->cxt, sizeof(ColOpen));
	o->zero_temp = zero_temp;
	o->unit_rows = unit_rows;
	o->mem = mem;
	o->raw_unit = (Size) raw_unit;
	o->packed_unit = (Size) packed_unit;
	o->cap = mem / OPEN_CAP_DIV;
#ifdef SIFT_TESTING
	if (sift_test_col_open_kb > 0)
		o->cap = (Size) sift_test_col_open_kb * 1024;
#endif
	max_single = Max((int) (o->cap / (OPEN_SLOT_PACKED * packed_unit)), 1);
#ifdef SIFT_TESTING
	if (sift_test_col_open_single >= 0)
		max_single = sift_test_col_open_single;
#endif
	o->wbudget = mem / OPEN_WRITER_DIV;
	o->nsingle = colb_ranges_isolate(b, &o->g, leaf, o->zero_temp ? nranges : 1, (uint64) unit_rows * (o->zero_temp ? OPEN_MIN_UNITS : OPEN_KEPT_MIN_UNITS), o->zero_temp ? max_single / 2 : max_single);
	if (o->nsingle == 0 && !o->zero_temp)
		return NULL;
	o->tail = MemoryContextAlloc(b->cxt, b->nksample * sizeof(int64));
	for (uint32 i = 0; i < b->nksample; i++)
		if (slot_wanted(o, colb_range_of_key(&o->g, b->ksample[i])))
			o->open_rows++;
		else
			o->tail[o->ntail++] = b->ksample[i];
	o->open_rows = (uint64) ((double) b->entries * o->open_rows / b->nksample);
	if (!o->zero_temp && !open_forced() && o->open_rows < OPEN_MIN_SHARE * b->entries)
		return NULL;
	if (!o->zero_temp)
		open_share(b, o);
	o->est_runs = (double) o->open_rows * (packed_unit - SIFT_ROWFILE_BLOCK_MAX) / unit_rows / Max(o->wbudget, 1);
	if (!o->zero_temp && o->est_runs > OPEN_WRITER_RUNS && !open_forced())
		return NULL;
	if (o->mem / OPEN_WORK_DIV < OPEN_WORK_UNITS * raw_unit)
		return NULL;
	o->pk = MemoryContextAllocZero(b->cxt, o->g.n * sizeof(SiftRowFile *));
	o->nrows = MemoryContextAllocZero(b->cxt, o->g.n * sizeof(uint32));
	o->cx = MemoryContextAllocZero(b->cxt, o->g.n * sizeof(MemoryContext));
	o->bytes = MemoryContextAllocZero(b->cxt, o->g.n * sizeof(Size));
	o->pcx = AllocSetContextCreate(b->cxt, "sift_col build open units", ALLOCSET_SMALL_SIZES);
	o->lcx = AllocSetContextCreate(b->cxt, "sift_col build open leftovers", ALLOCSET_DEFAULT_SIZES);
	o->ucx = AllocSetContextCreate(b->cxt, "sift_col build unit cut", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(b->cxt);
	initStringInfo(&o->scratch);
	MemoryContextSwitchTo(old);
	o->block = Min(Max(o->cap / ((Size) Max(o->zero_temp ? o->g.n : o->nsingle, 1) * 2), SIFT_ROWFILE_BLOCK_MIN), SIFT_ROWFILE_BLOCK_MAX);
	o->row.val = MemoryContextAlloc(b->cxt, b->r->schema->ncols * sizeof(SiftColValue));
	o->row.elem = MemoryContextAllocZero(b->cxt, (b->r->schema->ncols + 1) * sizeof(SiftColValue *));
	o->xcx = AllocSetContextCreate(b->cxt, "sift_col build open unit decoded", ALLOCSET_SMALL_SIZES);
	return o;
}
