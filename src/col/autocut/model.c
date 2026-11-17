/*
 * col/autocut/model.c - the instructions of a lookup with and without a cut
 * how scattered a leaf is, whether a b-tree already serves it, what a cut saves
 * measured on the sampled superblocks
 */
/* check-memory: allow-allocs the cells of a leaf are in a spool held to maintenance_work_mem; the arrays follow one superblock */
#include "postgres.h"
#include <math.h>
#include "access/htup_details.h"
#include "catalog/pg_statistic.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "col/point.h"
#include "core/limits.h"
#include "core/cmp.h"
#include "col/autocut/autocut_int.h"
#include "core/host.h"
#include "core/membudget.h"

#define AUTOCUT_KEYS		32		/* keys taken from the sample, more than half of them must read that many */

#define AUTOCUT_BLOOM_PASS	0.25	/* the share of the units without the key that the bloom filter of a key column passes (SIFT_CSET_BLOOM_MIN_BITS: about a quarter) */

/*
 * The default formula decides: B = R * log2(S) / log2(1.05), a gain of 5% being one unit, S the instructions of a lookup
 * (an equality or a range that holds the rows of one unit) without the cut over those with it, R 1 (0.5 for the lane of a range or a network); C = G / 3 + W / 5, G the
 * predicted growth of the index in percent (3% being one unit), W the instructions the cut adds to the build in percent (5% being one unit).
 */
#define AUTOCUT_FIXED_ROWS	64000	/* a lookup costs this many rows besides the rows it reads: opening the index, the snapshot and the directory of the segments, 0.5 M instructions (a day count of a cut index of 3,000,000 rows takes 0.67 to 0.81 M) */

#define AUTOCUT_ROW_INSTR	8.5		/* instructions to read and filter one row of a unit: 25.4 M for the 134 units of 3,000,000 rows of 56 bytes, 28.6 M for the 293 of 190 bytes */

#define AUTOCUT_MIN_SAVED	100000	/* instructions a lookup must save, at least (the default formula) */

#define AUTOCUT_BUILD_BASE	1250	/* instructions a row takes to build the index without the cut, besides the leaves */

#define AUTOCUT_BUILD_LEAF	520		/* ... for each leaf of the index */

#define AUTOCUT_BUILD_CUT	1150	/* ... that the cut adds to each row: the sort by the key (3,000,000 rows: +3.5 G instructions for 1 to 3 leaves, +35 to +67% of a build) */

/* the share of the sampled superblocks whose zone holds the middle of another one's: the units an equality on the leaf keeps (1: its zones prune nothing); m: the superblocks that have the leaf */
double
acut_zone_share(const Track *t, int n, int *m)
{
	double		lo[AUTOCUT_SBS],
				hi[AUTOCUT_SBS],
				point = 0;

	*m = 0;
	for (int i = 0; i < n; i++)
		if (t->has[i])
		{
			lo[*m] = t->lo[i];
			hi[(*m)++] = t->hi[i];
		}
	if (*m < 2)
		return 1;
	for (int i = 0; i < *m; i++)
	{
		double		mid = (lo[i] + hi[i]) / 2;

		for (int j = 0; j < *m; j++)
			point += lo[j] <= mid && mid <= hi[j];
	}
	return point / *m / *m;
}

/* whether a valid unique b-tree of the table has the column alone as its key: a lookup of the column is its, and the planner takes it before the scan */
bool
acut_unique_btree(Relation heap, AttrNumber att)
{
	bool		found = false;

	foreach_oid(ioid, RelationGetIndexList(heap))
	{
		HeapTuple	tp = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(ioid));
		Form_pg_index ix;

		if (!HeapTupleIsValid(tp))
			continue;
		ix = (Form_pg_index) GETSTRUCT(tp);
		found |= ix->indisunique && ix->indisvalid && ix->indnkeyatts == 1 && ix->indkey.values[0] == att && heap_attisnull(tp, Anum_pg_index_indpred, NULL);
		ReleaseSysCache(tp);
	}
	return found;
}

/* the column a CLUSTER of the table leads with (0: none): the order the table was given on purpose */
AttrNumber
acut_cluster_att(Relation heap)
{
	AttrNumber	att = 0;

	foreach_oid(ioid, RelationGetIndexList(heap))
	{
		HeapTuple	tp = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(ioid));

		if (!HeapTupleIsValid(tp))
			continue;
		if (((Form_pg_index) GETSTRUCT(tp))->indisclustered)
			att = ((Form_pg_index) GETSTRUCT(tp))->indkey.values[0];
		ReleaseSysCache(tp);
	}
	return att;
}

/*
 * whether the zones of the sampled superblocks of a leaf leave most of them out of an equality (the verdict "scattered" of sift.col_clustering). A leaf with
 * values in fewer than two of the sampled superblocks (all NULL) has no zones that a cut could widen.
 */
bool
acut_scattered(const Track *t, int n)
{
	int			m;
	double		share = acut_zone_share(t, n, &m);

	return m < 2 || !(share * m <= 2 || share <= 0.25);
}

/* the instructions of a lookup that reads `reads` units over those of one that reads the `ideal` units its rows fill */
double
acut_cut_speedup(double reads, double ideal, double rows_per_unit)
{
	return (AUTOCUT_FIXED_ROWS + reads * rows_per_unit) / (AUTOCUT_FIXED_ROWS + ideal * rows_per_unit);
}

/* whether a cut saves the default formula's minimum, S >= 1.05 and AUTOCUT_MIN_SAVED instructions a lookup */
static bool
cut_pays_a_lookup(double reads, double ideal, double rows_per_unit)
{
	return acut_cut_speedup(reads, ideal, rows_per_unit) >= 1.05 && (reads - ideal) * rows_per_unit * AUTOCUT_ROW_INSTR >= AUTOCUT_MIN_SAVED;
}

double
acut_cut_benefit(double speedup, double relevance)
{
	return relevance * log2(Max(speedup, 1.0)) / log2(1.05);
}

double
acut_cut_cost(double growth_pct, int nleaves)
{
	return growth_pct / 3 + 100.0 * AUTOCUT_BUILD_CUT / (AUTOCUT_BUILD_BASE + (double) AUTOCUT_BUILD_LEAF * nleaves) / 5;
}

/* a distinct value of a leaf in one sampled superblock, with the rows that hold it */
typedef struct Cell
{
	int64		key;
	uint32		sb;
	uint32		n;
} Cell;

#define CELL_KEY(x) ((x)->key)
#define CELL_KEY2(x) 0
#define CELL_TIE(x) ((x)->sb)

static inline int
cell_cmp(const Cell *x, const Cell *y)
{
	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return SIFT_CMP3(x->sb, y->sb);
}

#define CELL_LESS(x, y) (cell_cmp(x, y) < 0)

SIFT_DEFINE_SORT(sort_cells_cmp, Cell, CELL_LESS)

/* a copy of the cells for the radix sort: none when it would pass the largest allocation (the comparison sort then) */
static void *
cell_copy(size_t bytes)
{
	void	   *copy = bytes <= MaxAllocSize ? palloc_extended(bytes, MCXT_ALLOC_NO_OOM) : NULL;

	SIFT_MEM_CHECKPOINT();
	return copy;
}

/* the rows are added in the order of the superblocks, so the radix sort applies */
SIFT_DEFINE_RADIX_SORT(sort_cells, Cell, CELL_KEY, CELL_KEY2, CELL_TIE, cell_copy, pfree, sort_cells_cmp)

/*
 * The cells of a leaf in the order of the values, those of one value in the order of the superblocks. The non-NULL rows of the sample are put in a buffer as a cell each, and
 * sorted: a buffer that holds them all is the order, read as it is; a smaller one is sorted a run at a time (a run ends at a superblock), each run written as cells (the rows of
 * one value in one superblock counted) to a spool, and the runs are merged.
 */
typedef struct Cells
{
	AcutSpool	sp;
	uint64		bound[AUTOCUT_SBS + 1];
	AcutMerge	merge;
	Cell	   *buf;
	uint32		fill,
				at;
	uint64		ncells;			/* the distinct (value, superblock) pairs */
	const Cell *next;
	uint64		groups;
	uint32		cap;			/* the rows the buffer holds, taken from the budget while it is */
	bool		runs;
	AcutBudget *budget;
} Cells;

#define CELL_HELD	(2 * sizeof(Cell))	/* what a row in the buffer takes of the budget: its cell and the sort's copy */

/* all the cells of one value */
typedef struct CellGroup
{
	int64		key;
	double		rows;
	int			ncell;
	uint32		sb[AUTOCUT_SBS];
	uint32		n[AUTOCUT_SBS];
} CellGroup;

/* the sorted rows of the buffer as cells, in place: the rows of one value in one superblock counted; returns the cells */
static uint32
cells_compact(Cell *buf, uint32 fill)
{
	uint32		n = 0;

	for (uint32 i = 0; i < fill; i++)
		if (n > 0 && buf[n - 1].key == buf[i].key && buf[n - 1].sb == buf[i].sb)
			buf[n - 1].n++;
		else
			buf[n++] = buf[i];
	return n;
}

/* the buffer sorted, as cells */
static void
cells_sort(Cells *c)
{
	sort_cells(c->buf, c->fill);
	c->fill = cells_compact(c->buf, c->fill);
}

static void
cells_write_run(Cells *c)
{
	cells_sort(c);
	for (uint32 i = 0; i < c->fill; i++)
		acut_spool_add(&c->sp, &c->buf[i]);
	c->fill = 0;
	SIFT_MEM_CHECKPOINT();
}

static const Cell *cells_next(Cells *c);

static void
cells_begin(Samp *s, const Track *t, Cells *c)
{
	AcutCur		cur;
	uint32		most = 0,
				nr = 0;

	*c = (Cells) {.budget = &s->budget};
	acut_spool_init(&c->sp, &s->budget, sizeof(Cell));
	for (int u = 0; u < s->nsb; u++)
		most = Max(most, s->start[u + 1] - s->start[u]);
	/* a run holds the rows of as many superblocks as half of what the budget has left takes, at least one */
	c->cap = (uint32) Min((int64) t->nonnull, Max((int64) most, Max(s->budget.left, 0) / 2 / (int64) CELL_HELD));
	s->budget.left -= c->cap * (int64) CELL_HELD;
	c->buf = palloc_array(Cell, c->cap);
	acut_cur_init(&cur, &s->rows, 0, s->n);
	for (int u = 0; u < s->nsb; u++)
	{
		if (c->fill > 0 && c->fill + s->start[u + 1] - s->start[u] > c->cap)
		{
			c->bound[nr++] = c->sp.n;
			cells_write_run(c);
		}
		for (uint32 i = s->start[u]; i < s->start[u + 1]; i++)
		{
			const void *rec = acut_cur_next(&cur);

			if (!ROW_NL(rec, s->ntr, t->idx))
				c->buf[c->fill++] = (Cell) {ROW_V(rec, t->idx), u, 1};
		}
	}
	acut_cur_free(&cur);
	c->runs = nr > 0;
	if (!c->runs)
	{
		cells_sort(c);
		c->ncells = c->fill;
	}
	else
	{
		c->bound[nr++] = c->sp.n;
		cells_write_run(c);
		c->bound[nr] = c->sp.n;
		pfree(c->buf);
		c->buf = NULL;
		s->budget.left += c->cap * (int64) CELL_HELD;
		c->ncells = c->sp.n;
		acut_merge_begin(&c->merge, &c->sp, c->bound, nr, 1);
	}
	c->next = cells_next(c);
	SIFT_MEM_CHECKPOINT();
}

static const Cell *
cells_next(Cells *c)
{
	if (c->runs)
		return acut_merge_next(&c->merge);
	return c->at < c->fill ? &c->buf[c->at++] : NULL;
}

static void
cells_end(Cells *c)
{
	if (c->runs)
		acut_merge_end(&c->merge);
	else
	{
		pfree(c->buf);
		c->budget->left += c->cap * (int64) CELL_HELD;
	}
	acut_spool_free(&c->sp);
}

static bool
cells_group(Cells *c, CellGroup *g)
{
	if (c->next == NULL)
		return false;
	if ((++c->groups & 1023) == 0)
		SIFT_MEM_CHECKPOINT();
	g->key = c->next->key;
	g->rows = 0;
	g->ncell = 0;
	do
	{
		SIFT_INVARIANT(g->ncell < AUTOCUT_SBS, "sift: a value has more cells than the %d superblocks of the sample", AUTOCUT_SBS);
		g->sb[g->ncell] = c->next->sb;
		g->n[g->ncell++] = c->next->n;
		g->rows += c->next->n;
		c->next = cells_next(c);
	} while (c->next != NULL && c->next->key == g->key);
	return true;
}

/*
 * The sampled superblocks of a key candidate give the units an equality on its keys reads: the median over AUTOCUT_KEYS keys taken at random rows, and the
 * median of the units their rows fill. fired: the cut would save a lookup what the default formula asks (cut_pays_a_lookup). keyed_room: the bytes the index keeps a
 * key list of the leaf in (the list takes AUTOCUT_KEY_BYTES for each cell of a superblock); kept: it keeps the keys of the leaf in some form (a bloom filter where no list
 * fits), not key_columns = 'off'. The cells are read once, in the order of the values; the rows of the most common value are left in t->top.
 */
double
acut_units_read(Track *t, Samp *s, double nsb, double rows_per_unit, double keyed_room, bool kept, double *ideal, bool *fired)
{
	int			nsampled = s->nsb;
	double		f = nsampled / nsb,
				step = (double) t->nonnull / AUTOCUT_KEYS,
				at = step / 2,
				seen = 0,
				reads[AUTOCUT_KEYS],
				fills[AUTOCUT_KEYS];
	int			nk = 0;
	bool		keyed;
	Cells		cells;
	CellGroup  *g;
	MemoryContext cx;
	MemoryContext old;

	*fired = false;
	t->top = 0;
	if (t->nonnull == 0)
		return 0;
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col key_cut cells", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cx);
	SIFT_MEM_SCOPE(col_key_cut_cells, cx);
	g = palloc(sizeof(CellGroup));
	cells = (Cells) {0};
	cells_begin(s, t, &cells);
	keyed = AUTOCUT_KEY_BYTES * (double) cells.ncells / nsampled <= keyed_room;
	while (cells_group(&cells, g))
	{
		/* the keys whose rows hold the points at, at + step, ..: a key of many rows is taken as often as its share of the rows */
		while (nk < AUTOCUT_KEYS && at < seen + g->rows)
		{
			double		pos = at - seen,
						cum = 0,
						mine,
						zone,
						others = g->ncell - 1;
			int			e = 0;

			while (e < g->ncell - 1 && cum + g->n[e] <= pos)
				cum += g->n[e++];
			mine = g->n[e];
			/* a unit is read when its zone holds the key, and then only when its key list holds it or its bloom filter passes it */
			zone = 0;
			for (int u = 0; u < nsampled; u++)
				zone += u != g->sb[e] && t->has[u] && t->lo[u] <= g->key && g->key <= t->hi[u];
			if (keyed)
				zone = others;
			else if (kept)
				zone = others + AUTOCUT_BLOOM_PASS * Max(zone - others, 0);
			reads[nk] = Min(nsb, 1 + zone / f);
			fills[nk] = Max(1, (mine + (g->rows - mine) / f) / rows_per_unit);
			nk++;
			at += step;
		}
		seen += g->rows;
		t->top = Max(t->top, (uint64) g->rows);
	}
	cells_end(&cells);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	if (nk == 0)
		return 0;
	qsort(reads, nk, sizeof(double), sift_cmp_f64);
	qsort(fills, nk, sizeof(double), sift_cmp_f64);
	*ideal = fills[nk / 2];
	*fired = cut_pays_a_lookup(reads[nk / 2], *ideal, rows_per_unit);
	return reads[nk / 2];
}

/*
 * The same for the values of a time leaf, which do not repeat: AUTOCUT_KEYS ranges at places spread over the sorted values of the sample, each holding the
 * rows of one unit (a day, a week of a table that takes it). A unit is read when its zone meets the range, so the units read are the sampled superblocks
 * that meet it (the first stands for itself, the others for 1/f each), and the units its rows fill are those of the rows with a value in it. The ranges are
 * found in one pass over the cells: a range ends at the values it starts and ends at, and takes every row of them.
 */
double
acut_ranges_read(const Track *t, Samp *s, double nsb, double rows_per_unit, double *ideal, bool *fired)
{
	int			nsampled = s->nsb;
	double		f = nsampled / nsb,
				reads[AUTOCUT_KEYS],
				fills[AUTOCUT_KEYS];
	uint32		n = t->nonnull,
				half;
	uint64		from_at[AUTOCUT_KEYS],
				to_at[AUTOCUT_KEYS],
				from[AUTOCUT_KEYS],
				to[AUTOCUT_KEYS],
				rank = 0;
	int64		lo[AUTOCUT_KEYS],
				hi[AUTOCUT_KEYS];
	int			kf = 0,
				kt = 0;
	Cells		cells;
	CellGroup  *g;
	MemoryContext cx;
	MemoryContext old;

	*fired = false;
	if (n < AUTOCUT_KEYS)
		return 0;
	half = (uint32) Max(1, n / nsb / 2);
	for (int k = 0; k < AUTOCUT_KEYS; k++)
	{
		uint64		mid = (uint64) n * (2 * k + 1) / (2 * AUTOCUT_KEYS);

		from_at[k] = mid > half ? mid - half : 0;
		to_at[k] = Min(n - 1, mid + half);
	}
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col key_cut cells", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cx);
	SIFT_MEM_SCOPE(col_key_cut_cells, cx);
	g = palloc(sizeof(CellGroup));
	cells = (Cells) {0};
	cells_begin(s, t, &cells);
	while (cells_group(&cells, g))
	{
		for (; kf < AUTOCUT_KEYS && from_at[kf] < rank + g->rows; kf++)
		{
			lo[kf] = g->key;
			from[kf] = rank;
		}
		for (; kt < AUTOCUT_KEYS && to_at[kt] < rank + g->rows; kt++)
		{
			hi[kt] = g->key;
			to[kt] = rank + (uint64) g->rows - 1;
		}
		rank += (uint64) g->rows;
	}
	cells_end(&cells);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	for (int k = 0; k < AUTOCUT_KEYS; k++)
	{
		double		hits = 0;

		for (int u = 0; u < nsampled; u++)
			hits += t->has[u] && t->lo[u] <= hi[k] && lo[k] <= t->hi[u];
		reads[k] = Min(nsb, hits > 1 ? 1 + (hits - 1) / f : 1);
		fills[k] = Max(1, (double) (to[k] - from[k] + 1) / n * nsb);
	}
	qsort(reads, AUTOCUT_KEYS, sizeof(double), sift_cmp_f64);
	qsort(fills, AUTOCUT_KEYS, sizeof(double), sift_cmp_f64);
	*ideal = fills[AUTOCUT_KEYS / 2];
	*fired = cut_pays_a_lookup(reads[AUTOCUT_KEYS / 2], *ideal, rows_per_unit);
	return reads[AUTOCUT_KEYS / 2];
}

/*
 * What the cut takes from the lookups of a leaf whose zones it would widen: an equality or a window on the leaf reads the units that hold its values (`held`, the
 * units with a row that has one) and not the `kept` (a share of them, at least one) it reads now. The loss is that speedup priced like a benefit and weighed by the share of the rows that have a value,
 * the rows a lookup of the leaf is about: a leaf that follows the table and is filled loses all it has, one filled in 0.2% of the rows almost nothing, one all NULL nothing.
 */
double
acut_zone_loss(const Track *t, uint32 nrows, double nsb, double rows_per_unit, double kept_share)
{
	double		filled = t->nonnull,
				held,
				kept;

	filled /= Max(nrows, 1);
	held = Min(nsb, filled * nsb * rows_per_unit);
	kept = Min(held, Max(1.0, kept_share * held));
	return filled * acut_cut_benefit(acut_cut_speedup(held, kept, rows_per_unit), 1.0);
}

/* The rows a segment may hold: those whose arrays (24 bytes a row, 48 at the end of the segment) fit array_bytes, and the writer's SIFT_CPOINT_BUILD_ROWS */
double
acut_segment_rows(void)
{
	return Min((double) SIFT_CPOINT_BUILD_ROWS, (double) sift_limit(array_bytes, SIFT_ARRAY_UNLIMITED) / 48);
}

/*
 * The share of the rows that the most common value of the leaf holds: the first of the most common values of the statistics of the column when it has them
 * (none: no value holds more than the rarest of the list), else the most common value of the sample (t->top, left by acut_units_read).
 */
double
acut_hot_share(Relation heap, const Track *t, uint32 nsampled)
{
	HeapTuple	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(t->hatt), BoolGetDatum(false));
	AttStatsSlot sslot;

	if (HeapTupleIsValid(tup))
	{
		double		share = 0.0;

		if (get_attstatsslot(&sslot, tup, STATISTIC_KIND_MCV, InvalidOid, ATTSTATSSLOT_NUMBERS))
		{
			share = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
		ReleaseSysCache(tup);
		return share;
	}
	return (double) t->top / Max(nsampled, 1);
}
