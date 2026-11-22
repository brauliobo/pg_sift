/*
 * col/v1/build/open.c - open units: a build with no room for the temporary files of the key ranges writes the rows as units as they arrive, with no file
 *
 * A build with key_cut whose rows are not in the order of the key puts them in key order by ranges of the key in temporary files (col/v1/build/spill.c), and is refused when those files
 * would not fit (temp_file_limit, the temporary tablespaces) and no btree on the key serves. Here every range of the key is an open unit instead: the rows of a range are packed in memory,
 * and a unit of a range that is complete is encoded into the open segment as it is (a unit holds several values of its range and two units of a range overlap in the key, so the segment
 * records no cut, sift_colw_set_arrival, and a merge sorts it). The segment ends when its writer holds its budget. A value of the key lies in all the units of its range, so a read of
 * the index before a merge opens every segment; a build with room for the files takes the files, which read as the key ranges' index does.
 */
#include "postgres.h"
#include "col/v1/build/build_int.h"
#include "col/v1/rowfile.h"
#include "core/memtrack.h"

#define OPEN_CAP_DIV		4	/* the open units hold a quarter of the memory of the open path, its writer a half, the encode of a unit the rest */
#define OPEN_WRITER_DIV	2
#define OPEN_WORK_DIV	4
#define OPEN_MIN_UNITS	8	/* a value of the key gets a range of its own when its rows fill this many units */
#define OPEN_SLOT_PACKED 1.75	/* a packed open unit holds this many times the bytes of a unit: the block it fills and the blocks it has compressed */
#define OPEN_WORK_UNITS	4	/* encoding a unit takes about this many raw units of memory: the rows decoded from the packed form, and the cut of the unit */

#ifdef SIFT_TESTING
int			sift_test_col_open = 0;	/* -1 none, 0 as the build decides, 1 every range is an open unit whatever room the files have */
int			sift_test_col_open_kb = 0;
int			sift_test_col_open_single = -1;
int			sift_test_col_open_mutant = 0;	/* 1 a value joins the unit of another, 2 the rows left when the scan ends are lost, 3 a unit is written twice, 4 the cap of the open units is not looked at */
char		sift_test_col_open_stats[256];
#endif

#define MUTANT_MIXED		1	/* a value joins the unit of another */
#define MUTANT_TAIL_LOST	2	/* the rows the open units hold when the scan ends are dropped */
#define MUTANT_TWICE		3	/* a unit is written twice */
#define MUTANT_CAP_IGNORED	4	/* the cap of the open units is not looked at */

static bool
mutant(int which)
{
#ifdef SIFT_TESTING
	return sift_test_col_open_mutant == which;
#else
	return false;
#endif
}

struct ColOpen
{
	ColRanges	g;
	uint32		unit_rows;
	Size		raw_unit,
				packed_unit;	/* the bytes a unit of rows takes in an open unit, raw and packed */
	Size		mem;			/* the memory of the open units, the open segment's writer and the encode of a unit: the build's budget */
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
	SiftColSegWriter *w;		/* the open segment */
	MemoryContext wcxt;
	int			nsingle;
	uint64		rows,
				units,
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

/* The unit of range i goes to the open segment as it is, and the segment ends when its writer holds its budget */
static void
open_append(ColBuild *b, ColOpen *o, int i)
{
	open_seal(o, i);
	if (o->w == NULL)
	{
		o->w = colb_writer_begin(b, &o->wcxt, o->wbudget);
		sift_colw_set_arrival(o->w);
	}
	open_write(b, o, o->w, o->wcxt, o->pk[i], o->nrows[i]);
	open_release(o, i);
	if (sift_colw_full(o->w))
	{
		open_end_segment(b, o->w, o->wcxt);
		o->w = NULL;
		colb_make_room(b);
	}
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
		open_append(b, o, i);
	while (o->total > (mutant(MUTANT_CAP_IGNORED) ? 8 * o->cap : o->cap))
	{
		o->evictions++;
		open_append(b, o, open_victim(o));
	}
}

/* The row of the scan goes to the open unit of its range */
void
colb_open_take(ColBuild *b, const SiftColRow *row)
{
	ColOpen    *o = b->open;
	int			i = colb_range_of_value(&o->g, &row->val[o->g.leaf]);
	MemoryContext old;

	if (mutant(MUTANT_MIXED) && (o->rows & 1))
		i = open_first_single(o);
	SiftMemTracker mt_col_build_open;

	sift_mem_init_sub(&mt_col_build_open, "col_build_open", o->pcx, false, sift_colb_budget(1));
	if (!slot_open(o, i))
		open_new(b, o, i);
	old = MemoryContextSwitchTo(o->cx[i]);
	sift_rowfile_put(o->pk[i], row, 0, 0);
	MemoryContextSwitchTo(old);
	o->nrows[i]++;
	open_settle(b, o, i);
	sift_mem_assert_within(&mt_col_build_open);
}

/* The rows of the batches the first run holds go to their open units */
void
colb_open_take_held(ColBuild *b)
{
	ListCell   *lc;
	SiftColRow	row = {0};

	foreach(lc, b->held)
	{
		SiftColBatch *batch = lfirst(lc);

		for (uint32 r = 0; r < batch->nrows; r++)
		{
			sift_cbatch_row(batch, r, &row);
			colb_open_take(b, &row);
		}
		sift_cbatch_release(batch);
	}
	sift_cbatch_row_free(&row);
	list_free(b->held);
	b->held = NIL;
	b->held_rows = 0;
	MemoryContextReset(b->heldcxt);
}

#ifdef SIFT_TESTING
static void
open_say(const ColOpen *o)
{
	snprintf(sift_test_col_open_stats, sizeof(sift_test_col_open_stats), "ranges=%d single=%d rows=%llu units=%llu evictions=%llu raw_kb=%zu pk_kb=%zu mem_kb=%zu peak_kb=%zu w_kb=%zu u_kb=%zu x_kb=%zu",
			 o->g.n, o->nsingle, (unsigned long long) o->rows, (unsigned long long) o->units, (unsigned long long) o->evictions,
			 o->raw_unit / 1024, o->packed_unit / 1024, o->mem / 1024, o->peak / 1024, o->wpeak / 1024, o->upeak / 1024, o->xpeak / 1024);
}
#endif

/* The scan is over: the rows the open units hold are units, short, and the open segment ends */
void
colb_open_finish(ColBuild *b)
{
	ColOpen    *o = b->open;

	for (int i = 0; i < o->g.n; i++)
	{
		if (!slot_open(o, i))
			continue;
		if (o->nrows[i] == 0 || mutant(MUTANT_TAIL_LOST))
			open_release(o, i);
		else
			open_append(b, o, i);
	}
	if (o->w != NULL)
		open_end_segment(b, o->w, o->wcxt);
#ifdef SIFT_TESTING
	open_say(o);
#endif
	MemoryContextDelete(o->pcx);
	MemoryContextDelete(o->ucx);
	MemoryContextDelete(o->xcx);
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

/* Every range is an open unit whatever room the files have (testing) */
bool
colb_open_forced(void)
{
#ifdef SIFT_TESTING
	return sift_test_col_open == 1;
#else
	return false;
#endif
}

/*
 * The open units of the build: a range of one value for each of the heaviest values of the key (as many as the memory holds, packed) and quantile ranges for the others, the build's budget as
 * their memory. NULL when that memory does not hold the encode of a unit. raw_unit and packed_unit are the bytes a unit of rows takes decoded and packed, nranges the ranges to cut.
 */
ColOpen *
colb_open_plan(ColBuild *b, int leaf, uint32 unit_rows, double raw_unit, double packed_unit, int nranges)
{
	int			max_single;
	MemoryContext old;
	ColOpen    *o;

	if (b->ksample == NULL || b->entries == 0)
		return NULL;
	o = MemoryContextAllocZero(b->cxt, sizeof(ColOpen));
	o->unit_rows = unit_rows;
	o->mem = b->budget;
	o->raw_unit = (Size) raw_unit;
	o->packed_unit = (Size) packed_unit;
	o->cap = o->mem / OPEN_CAP_DIV;
#ifdef SIFT_TESTING
	if (sift_test_col_open_kb > 0)
		o->cap = (Size) sift_test_col_open_kb * 1024;
#endif
	max_single = Max((int) (o->cap / (OPEN_SLOT_PACKED * packed_unit)), 1);
#ifdef SIFT_TESTING
	if (sift_test_col_open_single >= 0)
		max_single = sift_test_col_open_single;
#endif
	o->wbudget = o->mem / OPEN_WRITER_DIV;
	o->nsingle = colb_ranges_isolate(b, &o->g, leaf, nranges, (uint64) unit_rows * OPEN_MIN_UNITS, max_single / 2);
	if (o->mem / OPEN_WORK_DIV < OPEN_WORK_UNITS * raw_unit)
		return NULL;
	o->pk = MemoryContextAllocZero(b->cxt, o->g.n * sizeof(SiftRowFile *));
	o->nrows = MemoryContextAllocZero(b->cxt, o->g.n * sizeof(uint32));
	o->cx = MemoryContextAllocZero(b->cxt, o->g.n * sizeof(MemoryContext));
	o->bytes = MemoryContextAllocZero(b->cxt, o->g.n * sizeof(Size));
	o->pcx = AllocSetContextCreate(b->cxt, "sift_col build open units", ALLOCSET_SMALL_SIZES);
	o->ucx = AllocSetContextCreate(b->cxt, "sift_col build unit cut", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(b->cxt);
	initStringInfo(&o->scratch);
	MemoryContextSwitchTo(old);
	o->block = Min(Max(o->cap / ((Size) o->g.n * 2), SIFT_ROWFILE_BLOCK_MIN), SIFT_ROWFILE_BLOCK_MAX);
	o->row.val = MemoryContextAlloc(b->cxt, b->r->schema->ncols * sizeof(SiftColValue));
	o->row.elem = MemoryContextAllocZero(b->cxt, (b->r->schema->ncols + 1) * sizeof(SiftColValue *));
	o->xcx = AllocSetContextCreate(b->cxt, "sift_col build open unit decoded", ALLOCSET_SMALL_SIZES);
	return o;
}
