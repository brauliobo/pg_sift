/*
 * col/autocut.c - the default of the option key_cut: CREATE INDEX and REINDEX of an index without key_cut (or with key_cut = 'auto')
 * cut the units by a key when a sample of the heap says that zones cannot skip the units of that key. The key is the first leaf, in the order of the index,
 * that is key-shaped (sift_col_key_shaped); when none of those is cut by, the first date or timestamp leaf, for a range of its values (ranges_read). A leaf is
 * chosen when the default formula says the cut pays: the benefit of the lookups it saves (an equality on one of its values, or a range holding the rows of one unit,
 * reads the units its rows fill and not those whose zones hold the key) is at least the cost of the growth of the index and of the build; no other leaf of the index
 * follows the order of the table (key_cut would widen its zones). The size is predicted from the sample: the integer leaves of the sampled rows are encoded by the codec of the index in the
 * units of the superblock cut and in the units of the key cut, the addresses of the rows are priced by what they measured, and the other leaves are
 * priced as unchanged. When the key would be worth cutting by and the index would grow more, the build only says so, with the command. The choice is
 * written to the option as 'auto: col', so that a promotion, a merge and a REINDEX keep it; key_cut = 'off' keeps the units by superblock. An index with
 * point_tid_columns or order_columns is left alone: the first lists rows, the second says the workload sorts by a column without a key, which a key cut
 * makes slower. sift.key_cut_decision(index) runs the same rule on an existing index and says what it would decide, without changing anything.
 *
 * The sample is a fixed set of superblocks spread over the heap, at most a quarter of it. For a key taken at a random row of the sample, the other sampled superblocks that hold
 * the key give the superblocks it is in (each stands for 1/f superblocks, f the sampled share), and the other sampled rows of the key give its rows.
 */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/codec.h"
#include "col/enc.h"
#include "col/col.h"
#include "core/cmp.h"
#include "col/setsum.h"
#include "core/sql.h"
#include "core/cmp.h"

#define AUTOCUT_KEYS		32		/* keys taken from the sample, more than half of them must read that many */
#define AUTOCUT_SBS			256		/* superblocks sampled, at most */
#define AUTOCUT_MIN_SBS		32
#define AUTOCUT_QUICK_SBS	8		/* superblocks of the first look, which most tables end at: it prices the decision at a share of the heap, not at a pass over it */
#define AUTOCUT_ROWS		400000	/* rows sampled, at most, unless AUTOCUT_MIN_SBS superblocks hold more: the sample is a number of units, so that a table of narrow rows (25,000 to a unit) is sampled as one of wide rows */
#define AUTOCUT_TRIAL_SBS	48		/* superblocks of the sample whose integer leaves are encoded to predict the size */
#define AUTOCUT_OTHER_PCT	1		/* percent more units that an equality on another leaf may keep under the cut of a lane: the cut spreads its zones over all the units */
#define AUTOCUT_LEAVES		8		/* leaves read: the candidates and those that may compete with them */
#define AUTOCUT_DIR_ROWS	256		/* rows of a unit of the cut whose directory is encoded from the sample, at least */
#define AUTOCUT_OVERHEAD_BYTES	0.17	/* what a row takes in the index besides its leaves and the directory of its unit: the headers of the segment and of the units, the page overhead and the free space of the last pages (0.13 + 0.04); the pre-aggregates, 0.03 to 0.59 a row and changed by the cut either way, are not predicted */
#define AUTOCUT_LEAF_BYTES	1.0		/* what a row takes in a leaf that is not read from the sample */
#define AUTOCUT_KEY_BYTES	2.0		/* what a key takes in the key list of a unit */
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
#define AUTOCUT_SECOND_SLACK 1		/* percent the prediction may exceed the cut by the key alone for the date to be its second key: the sample does not see the runs and addresses a date order shrinks */
#define AUTOCUT_QUICK_SLACK	4		/* units of cost over the benefit that the first look still sends a time leaf to the sample: its prediction from a few superblocks is rough (the prediction for a range lane beside other leaves was 7 to 16 points of growth, 2 to 5 units of cost, over the sample's) */

typedef struct Cell
{
	int64		key;
	uint32		sb;
	uint32		n;
} Cell;

typedef struct Track
{
	int			c;				/* leaf of the schema */
	AttrNumber	hatt;
	uint8		type;
	bool		cand;			/* key-shaped: an equality on its values is tested */
	bool		time;			/* a date or timestamp that is not a key, or the start lane of a range or inet column: a range of its values is tested */
	const SiftColDef *shape;	/* the head of the range or network whose lane this is (col/shape.c); NULL for a column's own value */
	int			lane;			/* which lane of the head */
	bool		image;			/* the first lane of a range or network: the image of its head, taken as untracked, is dictionary-coded in the order of the rows, and a unit of the cut by the lane lists it in the order of the lane, so it is priced like the lane */
	int64	   *v;				/* the value of each sampled row, 0 for a NULL */
	bool	   *nl;
	Cell	   *cells;
	int64		ncells;
	int64		cap;
	double		lo[AUTOCUT_SBS];
	double		hi[AUTOCUT_SBS];
	bool		has[AUTOCUT_SBS];
} Track;

typedef struct Samp
{
	uint32		n;				/* sampled rows */
	uint32		start[AUTOCUT_SBS + 1];	/* the rows of sampled superblock s are start[s] to start[s + 1] */
	int			nsb;
	SiftKey	   *tid;			/* the address of each sampled row, the sampled superblocks side by side (superblock s starts at block s * SIFT_SB_PAGES) */
} Samp;

static int
cell_cmp(const void *a, const void *b)
{
	const Cell *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->sb < y->sb ? -1 : x->sb > y->sb;
}

static bool
value_of(uint8 type, Datum d, int64 *v)
{
	switch (type)
	{
		case SIFT_COLT_INT2:
			*v = DatumGetInt16(d);
			return true;
		case SIFT_COLT_INT4:
		case SIFT_COLT_DATE:
			*v = DatumGetInt32(d);
			return true;
		case SIFT_COLT_INT8:
		case SIFT_COLT_TIMESTAMP:
		case SIFT_COLT_TIMESTAMPTZ:
			*v = DatumGetInt64(d);
			return true;
	}
	return false;
}

/* the name the option key_cut gives leaf c: the column, or its lane (r#1) */
static const char *
cut_name(Relation index, const SiftColSchema *sc, int c)
{
	return sc->cols[c].lane > 0 ? sc->cols[c].path : NameStr(TupleDescAttr(RelationGetDescr(index), sc->cols[c].attno - 1)->attname);
}

static bool
cand_type(uint8 type)
{
	return type == SIFT_COLT_INT2 || type == SIFT_COLT_INT4 || type == SIFT_COLT_INT8 || type == SIFT_COLT_DATE;
}

static bool
time_type(uint8 type)
{
	return type == SIFT_COLT_DATE || type == SIFT_COLT_TIMESTAMP || type == SIFT_COLT_TIMESTAMPTZ;
}

/* the share of the sampled superblocks whose zone holds the middle of another one's: the units an equality on the leaf keeps (1: its zones prune nothing); m: the superblocks that have the leaf */
static double
zone_share(const Track *t, int n, int *m)
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

/* whether the zones of the sampled superblocks of a leaf leave most of them out of an equality (the verdict "scattered" of sift.col_clustering) */
static bool
scattered(const Track *t, int n)
{
	int			m;
	double		share = zone_share(t, n, &m);

	return m >= 2 && !(share * m <= 2 || share <= 0.25);
}

/* the instructions of a lookup that reads `reads` units over those of one that reads the `ideal` units its rows fill */
static double
cut_speedup(double reads, double ideal, double rows_per_unit)
{
	return (AUTOCUT_FIXED_ROWS + reads * rows_per_unit) / (AUTOCUT_FIXED_ROWS + ideal * rows_per_unit);
}

/* whether a cut saves the default formula's minimum, S >= 1.05 and AUTOCUT_MIN_SAVED instructions a lookup */
static bool
cut_pays_a_lookup(double reads, double ideal, double rows_per_unit)
{
	return cut_speedup(reads, ideal, rows_per_unit) >= 1.05 && (reads - ideal) * rows_per_unit * AUTOCUT_ROW_INSTR >= AUTOCUT_MIN_SAVED;
}

static double
cut_benefit(double speedup, double relevance)
{
	return relevance * log2(Max(speedup, 1.0)) / log2(1.05);
}

static double
cut_cost(double growth_pct, int nleaves)
{
	return growth_pct / 3 + 100.0 * AUTOCUT_BUILD_CUT / (AUTOCUT_BUILD_BASE + (double) AUTOCUT_BUILD_LEAF * nleaves) / 5;
}

/*
 * The sampled superblocks of a key candidate give the units an equality on its keys reads: the median over AUTOCUT_KEYS keys taken at random rows, and the
 * median of the units their rows fill. fired: the cut would save a lookup what the default formula asks (cut_pays_a_lookup). keyed: the index keeps a key list of
 * the leaf.
 */
static double
units_read(Track *t, int nsampled, double nsb, double rows_per_unit, bool keyed, double *ideal, bool *fired)
{
	double		f = nsampled / nsb,
				total = 0,
				step,
				at,
				reads[AUTOCUT_KEYS],
				fills[AUTOCUT_KEYS];
	int			nk = 0;
	int64		i = 0;

	*fired = false;
	if (t->ncells == 0)
		return 0;
	qsort(t->cells, t->ncells, sizeof(Cell), cell_cmp);
	for (int64 k = 0; k < t->ncells; k++)
		total += t->cells[k].n;
	step = total / AUTOCUT_KEYS;
	at = step / 2;
	for (double seen = 0; i < t->ncells && nk < AUTOCUT_KEYS;)
	{
		int64		j = i;
		double		rows = 0;

		while (j < t->ncells && t->cells[j].key == t->cells[i].key)
			rows += t->cells[j++].n;
		/* the keys whose rows hold the points at, at + step, ..: a key of many rows is taken as often as its share of the rows */
		while (nk < AUTOCUT_KEYS && at < seen + rows)
		{
			double		pos = at - seen,
						cum = 0,
						mine,
						others = j - i - 1;
			int64		e = i;

			while (e < j - 1 && cum + t->cells[e].n <= pos)
				cum += t->cells[e++].n;
			mine = t->cells[e].n;
			/* without a key list a unit is read when its zone holds the key */
			if (!keyed)
			{
				others = 0;
				for (int u = 0; u < nsampled; u++)
					others += u != t->cells[e].sb && t->has[u] && t->lo[u] <= t->cells[e].key && t->cells[e].key <= t->hi[u];
			}
			reads[nk] = Min(nsb, 1 + others / f);
			fills[nk] = Max(1, (mine + (rows - mine) / f) / rows_per_unit);
			nk++;
			at += step;
		}
		seen += rows;
		i = j;
	}
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
 * that meet it (the first stands for itself, the others for 1/f each), and the units its rows fill are those of the rows with a value in it.
 */
static double
ranges_read(const Track *t, uint32 nrows, int nsampled, double nsb, double rows_per_unit, double *ideal, bool *fired)
{
	double		f = nsampled / nsb,
				reads[AUTOCUT_KEYS],
				fills[AUTOCUT_KEYS];
	int64	   *s = palloc_array(int64, nrows);
	uint32		n = 0,
				half;

	*fired = false;
	for (uint32 i = 0; i < nrows; i++)
		if (!t->nl[i])
			s[n++] = t->v[i];
	if (n < AUTOCUT_KEYS)
	{
		pfree(s);
		return 0;
	}
	sift_sort_i64(s, n);
	half = Max(1, n / nsb / 2);
	for (int k = 0; k < AUTOCUT_KEYS; k++)
	{
		uint32		mid = (uint64) n * (2 * k + 1) / (2 * AUTOCUT_KEYS),
					from = mid > half ? mid - half : 0,
					to = Min(n - 1, mid + half);
		int64		lo = s[from],
					hi = s[to];
		double		hits = 0;

		while (from > 0 && s[from - 1] == lo)
			from--;
		while (to + 1 < n && s[to + 1] == hi)
			to++;
		for (int u = 0; u < nsampled; u++)
			hits += t->has[u] && t->lo[u] <= hi && lo <= t->hi[u];
		reads[k] = Min(nsb, hits > 1 ? 1 + (hits - 1) / f : 1);
		fills[k] = Max(1, (double) (to - from + 1) / n * nsb);
	}
	pfree(s);
	qsort(reads, AUTOCUT_KEYS, sizeof(double), sift_cmp_f64);
	qsort(fills, AUTOCUT_KEYS, sizeof(double), sift_cmp_f64);
	*ideal = fills[AUTOCUT_KEYS / 2];
	*fired = cut_pays_a_lookup(reads[AUTOCUT_KEYS / 2], *ideal, rows_per_unit);
	return reads[AUTOCUT_KEYS / 2];
}

/* the bytes the codec of the index writes for the values of the rows (a NULL takes the value before it, as in a unit) */
static double
encoded_bytes(const Track *t, const uint32 *rows, uint32 n, StringInfo buf, int64 *tmp)
{
	SiftCcPolicy pol;
	int64		prev = 0;

	for (uint32 i = 0; i < n; i++)
	{
		uint32		r = rows[i];

		tmp[i] = t->nl[r] ? prev : t->v[r];
		prev = tmp[i];
	}
	resetStringInfo(buf);
	sift_cc_policy_default(&pol);
	sift_cc_encode(buf, SIFT_CC_INT, tmp, n, NULL, &pol, NULL);
	return buf->len;
}

/* a sampled row with the values of the (at most two) leaves a cut sorts by, so that the sort compares them in line */
typedef struct SortRow
{
	int64		v[2];
	uint32		row;
	bool		nl[2];
} SortRow;

static inline int
sort_row_cmp(const SortRow *x, const SortRow *y)
{
	for (int i = 0; i < 2; i++)
	{
		if (x->nl[i] != y->nl[i])
			return x->nl[i] ? 1 : -1;
		if (!x->nl[i] && x->v[i] != y->v[i])
			return x->v[i] < y->v[i] ? -1 : 1;
	}
	return SIFT_CMP3(x->row, y->row);
}

#define SORT_ROW_LESS(x, y) (sort_row_cmp(x, y) < 0)
SIFT_DEFINE_SORT(sort_rows, SortRow, SORT_ROW_LESS)

/*
 * The bytes of the integer leaves and of the row directories of the rows of at most AUTOCUT_TRIAL_SBS of the sampled superblocks (every k-th) in the units of the
 * superblock cut (key < 0) or of the cut by the leaf key, per sampled row. The integer leaves are encoded by the codec in units of as many rows as a superblock
 * holds. The row directory is encoded as enc.c writes it: a unit of the cut holds the rows of the whole heap in a range of the key and lists their addresses,
 * and the sample holds a share of the heap, so a unit is the sampled rows in the range, n / nsb of them. When that is fewer than AUTOCUT_DIR_ROWS, the unit is
 * that many rows and the gaps between its blocks, then smaller than the whole heap gives, are widened by the bits of the difference.
 */
static double
layout_bytes(const Track *tr, int ntr, const Samp *s, int key, int key2, double nsb, double heap_pages, double rows_per_unit)
{
	const Track *order[2] = {key >= 0 ? &tr[key] : NULL, key2 >= 0 ? &tr[key2] : NULL};
	StringInfoData buf;
	int64	   *tmp = palloc_array(int64, s->n + 1);
	uint32	   *rows = palloc_array(uint32, s->n + 1);
	uint32	   *sel = palloc_array(uint32, s->n + 1);
	SiftKey    *tid = palloc_array(SiftKey, s->n + 1);	/* by sampled row: its address with the taken superblocks side by side */
	SiftKey    *keys = palloc_array(SiftKey, s->n + 1);
	uint32		nsel = 0,
				stride = (s->nsb + AUTOCUT_TRIAL_SBS - 1) / AUTOCUT_TRIAL_SBS,
				unit,
				pages;
	int			nused = 0;
	double		bytes = 0,
				narrower;

	for (int u = 0; u < s->nsb; u += stride, nused++)
	{
		uint32		from = nsel;

		for (uint32 i = s->start[u]; i < s->start[u + 1]; i++)
		{
			sel[nsel++] = i;
			tid[i] = s->tid[i] - (((SiftKey) (u - nused) * SIFT_SB_PAGES) << 16);
		}
		if (key < 0)
			bytes += sift_cunit_directory_bytes(tid + s->start[u], nsel - from, false, nused, &pages);
	}
	unit = key >= 0 ? Max(nsel / nsb, AUTOCUT_DIR_ROWS) : Max(1, nsel / nused);	/* a unit of the cut holds the rows of the whole heap in a range of the key, the sampled share of them */
	narrower = Max(0, log2(nsb / nused));	/* the units of the sample are sorted slices of nused superblocks: a unit of the whole heap holds a key range this much narrower, so its gaps take this many bits fewer */
	initStringInfo(&buf);
	if (key >= 0)
	{
		uint32		dir_rows = unit;
		double		wider = Max(0, log2(heap_pages / rows_per_unit / ((double) nused * SIFT_SB_PAGES / dir_rows)));

		SortRow    *by = palloc_array(SortRow, nsel);

		for (uint32 i = 0; i < nsel; i++)
		{
			by[i] = (SortRow) {.row = sel[i]};
			for (int k = 0; k < 2 && order[k] != NULL; k++)
			{
				by[i].nl[k] = order[k]->nl[sel[i]];
				by[i].v[k] = order[k]->v[sel[i]];
			}
		}
		sort_rows(by, nsel);
		for (uint32 i = 0; i < nsel; i++)
			sel[i] = by[i].row;
		pfree(by);
		for (uint32 from = 0; from < nsel; from += dir_rows)
		{
			uint32		n = Min(dir_rows, nsel - from);

			for (uint32 i = 0; i < n; i++)
				keys[i] = tid[sel[from + i]];
			sift_sort_u64(keys, n);
			bytes += sift_cunit_directory_bytes(keys, n, true, 0, &pages) + wider * pages / 8;
		}
	}
	for (int t = 0; t < ntr; t++)
	{
		if (key < 0)
			for (int u = 0; u < s->nsb; u += stride)
			{
				for (uint32 i = s->start[u]; i < s->start[u + 1]; i++)
					rows[i - s->start[u]] = i;
				bytes += (1 + tr[t].image) * encoded_bytes(&tr[t], rows, s->start[u + 1] - s->start[u], &buf, tmp);
			}
		else
			for (uint32 from = 0; from < nsel; from += unit)
			{
				uint32		n = Min(unit, nsel - from);
				double		b;

				memcpy(rows, sel + from, n * sizeof(uint32));
				sift_sort_u32(rows, n);	/* the rows of a unit are in TID order */
				b = encoded_bytes(&tr[t], rows, n, &buf, tmp);
				if (&tr[t] == order[0] && tr[t].shape != NULL)
					b = Max(n / 8.0, b - n * narrower / 8);
				bytes += (1 + tr[t].image) * b;
			}
	}
	pfree(buf.data);
	pfree(tmp);
	pfree(rows);
	pfree(sel);
	pfree(tid);
	pfree(keys);
	return bytes / Max(nsel, 1) + AUTOCUT_OVERHEAD_BYTES;
}

typedef struct CutVerdict
{
	SiftAutoCut res;
	bool		decided;		/* the options let a build decide */
	bool		maybe;			/* the first look found a key that may be worth cutting by: the full sample decides */
	const char *reason;
	char		leaf[NAMEDATALEN];
	char		second[NAMEDATALEN];	/* the date or timestamp leaf the rows of a key are sorted by, or empty */
	double		units;
	double		sampled;
	double		reads;
	double		ideal;
	double		growth;
	double		benefit;		/* the default formula: B and C of the cut (cut_benefit, cut_cost) */
	double		cost;
	bool		range;			/* the leaf is a time leaf: a range of its values is read, not an equality */
} CutVerdict;

/* The rows of the superblock in the middle of the heap: what a unit holds for a table that was never analyzed, and what the build aims a unit at then */
static double
mid_rows(Relation heap, BlockNumber nblocks, double nsb)
{
	BlockNumber start = (BlockNumber) (nsb / 2) * SIFT_SB_PAGES;
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	TableScanDesc scan = table_beginscan_strat(heap, GetActiveSnapshot(), 0, NULL, true, false);
	double		n = 0;

	heap_setscanlimits(scan, start, Min(SIFT_SB_PAGES, nblocks - start));
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		n++;
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	return Max(n, 1);
}

/* The rule above for the index: no write, the verdict says what a build would do. dry: the options of the index do not stop it. */
static void
decide_stage(Relation heap, Relation index, bool dry, CutVerdict *v, bool quick)
{
	const SiftColRel *rel;
	const SiftColSchema *sc;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col auto key_cut", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	Track	   *tr;
	Samp	   *samp;
	int			ntr = 0,
				ncand = 0,
				untracked = 0,
				want,
				full,
				first = -1,
				nfire = 0,
				fire[AUTOCUT_LEAVES];
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	double		nsb = ceil((double) nblocks / SIFT_SB_PAGES),
				rows_per_unit,
				def_row = 0,
				kc_row = 0;
	double		reads[AUTOCUT_LEAVES],
				fills[AUTOCUT_LEAVES];
	bool		blocked[AUTOCUT_LEAVES] = {false};
	int			first_open;
	uint32		cap;
	int64	   *tmp;
	TupleTableSlot *slot;
	const char *leaf = NULL;

	*v = (CutVerdict) {.res = SIFT_AUTOCUT_NONE, .units = nsb};
	if (sift_col_format_minor < SIFT_COL_FORMAT_PRESENCE)
		v->reason = "sift.col_format_minor is below 15";
	else if (!dry && !sift_col_options_key_cut_auto(index))
		v->reason = "key_cut is set";
	else if (sift_col_options_point_tid_columns(index)[0] != '\0')
		v->reason = "point_tid_columns is set";
	else if (sift_col_options_order_columns(index)[0] != '\0')
		v->reason = "order_columns is set";
	else if (!ActiveSnapshotSet())
		v->reason = "no snapshot";
	if (v->reason != NULL)
		goto done;
	v->decided = true;
	rel = sift_col_rel_private(index, cx);	/* the loop reads catalogs, which reload the cached state */
	sc = rel->schema;
	tr = palloc0(sizeof(Track) * AUTOCUT_LEAVES);
	for (int c = 0; c < sc->ncols; c++)
	{
		const SiftColDef *d = &sc->cols[c];
		AttrNumber	hatt = index->rd_index->indkey.values[d->attno - 1];
		int64		x;

		if (ntr < AUTOCUT_LEAVES && d->kind == SIFT_COLK_SCALAR && d->store == SIFT_COLS_INT && d->lane == 0 && d->parent < 0 && (d->path == NULL || d->path[0] == '\0') &&
			hatt != 0 && value_of(d->type, 0, &x))
		{
			tr[ntr] = (Track) {.c = c, .hatt = hatt, .type = d->type, .cand = cand_type(d->type) && sift_col_key_shaped(heap, index, sc, d)};
			tr[ntr].time = !tr[ntr].cand && time_type(d->type);
			ncand += tr[ntr].cand || tr[ntr].time;
			ntr++;
		}
		else if (ntr < AUTOCUT_LEAVES && d->lane > 0 && sift_col_def_shape(&sc->cols[c - d->lane]) != 0 && !(sift_col_def_shape(&sc->cols[c - d->lane]) == SIFT_COL_SHAPE_INET && d->lane > 1) && hatt != 0)
		{
			/* the lanes of a range or a network are integer leaves of the index whose size the cut changes; the first (the start of the set, the node of a network) has values that
			 * do not repeat, and a range of them is read like a range of dates */
			tr[ntr] = (Track) {.c = c, .hatt = hatt, .type = SIFT_COLT_INT8, .time = d->lane == 1, .image = d->lane == 1, .shape = &sc->cols[c - d->lane], .lane = d->lane};
			ncand += d->lane == 1;
			untracked -= d->lane == 1;
			ntr++;
		}
		else
			untracked++;
	}
	rows_per_unit = heap->rd_rel->reltuples > 0 ? heap->rd_rel->reltuples / nsb : mid_rows(heap, nblocks, nsb);
	/* a quarter of the heap at most: the sample costs what a build of the share costs; and at least AUTOCUT_MIN_SBS units whatever their rows */
	full = (int) Min(Min(Min(AUTOCUT_SBS, nsb), Max(ceil(nsb / 4), AUTOCUT_MIN_SBS)), Max(AUTOCUT_ROWS / Max(rows_per_unit, 1), AUTOCUT_MIN_SBS));
	/* a table of fewer units is sampled whole: the formula, not a count of units, says whether so few units pay for a cut */
	cap = (uint32) Min(Max((double) AUTOCUT_ROWS, 1.25 * full * Max(rows_per_unit, 1)), (double) full * SIFT_COL_UNIT_MAX_ROWS);
	if (nblocks == 0)
	{
		v->reason = "the table is empty";
		goto done;
	}
	if (ncand == 0)
	{
		v->reason = "no key-shaped column (a foreign key or *_id, not unique, int2, int4, int8 or date) and no date, timestamp, range or inet column";
		goto done;
	}
	want = quick ? Min(full, AUTOCUT_QUICK_SBS) : full;
	samp = palloc0(sizeof(Samp));
	samp->tid = palloc_array(SiftKey, cap + SIFT_COL_UNIT_MAX_ROWS);
	for (int t = 0; t < ntr; t++)
	{
		tr[t].v = palloc_array(int64, cap + SIFT_COL_UNIT_MAX_ROWS);
		tr[t].nl = palloc_array(bool, cap + SIFT_COL_UNIT_MAX_ROWS);
	}
	slot = table_slot_create(heap, NULL);
	for (int s = 0; s < want; s++)
	{
		/* one superblock from each stretch of the heap, at a place that varies with the stretch and not with the run: the same table gives the same sample */
		BlockNumber start = (BlockNumber) ((s + ((uint32) s * 2654435761u % 1024) / 1024.0) * nsb / want) * SIFT_SB_PAGES;
		TableScanDesc scan;
		uint32		from = samp->n,
					nrow = 0;

		if (start >= nblocks || samp->n > cap)
			continue;
		scan = table_beginscan_strat(heap, GetActiveSnapshot(), 0, NULL, true, false);
		heap_setscanlimits(scan, start, Min(SIFT_SB_PAGES, nblocks - start));
		while (nrow++ < SIFT_COL_UNIT_MAX_ROWS && table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			samp->tid[samp->n] = ((SiftKey) (samp->nsb * SIFT_SB_PAGES + ItemPointerGetBlockNumber(&slot->tts_tid) - start) << 16) | ItemPointerGetOffsetNumber(&slot->tts_tid);
			for (int t = 0; t < ntr; t++)
			{
				bool		isnull;
				Datum		dat = slot_getattr(slot, tr[t].hatt, &isnull);

				tr[t].v[samp->n] = 0;
				tr[t].nl[samp->n] = isnull;
				if (!isnull && tr[t].shape != NULL)
				{
					SiftColValue lane[SIFT_COL_MAX_LANES];

					sift_col_datum_values(tr[t].shape, dat, false, lane);
					tr[t].nl[samp->n] = lane[tr[t].lane].isnull;	/* a bound that does not fit a lane is a NULL there */
					tr[t].v[samp->n] = lane[tr[t].lane].v.i;
				}
				else if (!isnull)
					value_of(tr[t].type, dat, &tr[t].v[samp->n]);
			}
			samp->n++;
		}
		table_endscan(scan);
		if (samp->n == from)
			continue;
		samp->start[samp->nsb] = from;
		samp->start[++samp->nsb] = samp->n;
	}
	ExecDropSingleTupleTableSlot(slot);
	v->sampled = samp->nsb;
	if (samp->nsb < Min(AUTOCUT_MIN_SBS, want))
	{
		v->reason = "the sample holds fewer than 32 superblocks";
		goto done;
	}
	tmp = palloc_array(int64, SIFT_COL_UNIT_MAX_ROWS);
	for (int t = 0; t < ntr; t++)
	{
		Track	   *k = &tr[t];

		for (int u = 0; u < samp->nsb; u++)
		{
			int			n = 0;

			for (uint32 i = samp->start[u]; i < samp->start[u + 1]; i++)
				if (!k->nl[i])
					tmp[n++] = k->v[i];
			if (n == 0)
				continue;
			sift_sort_i64(tmp, n);
			k->has[u] = true;
			k->lo[u] = (double) tmp[0];
			k->hi[u] = (double) tmp[n - 1];
			for (int i = 0, j; k->cand && i < n; i = j)
			{
				for (j = i; j < n && tmp[j] == tmp[i];)
					j++;
				if (k->ncells == k->cap)
				{
					k->cap = k->cap ? k->cap * 2 : 1024;
					k->cells = k->cells ? repalloc(k->cells, k->cap * sizeof(Cell)) : palloc(k->cap * sizeof(Cell));
				}
				k->cells[k->ncells++] = (Cell) {tmp[i], u, j - i};
			}
		}
	}
	/* what a row takes in the index now: the integer leaves as the codec writes them, the others as 1 byte, and the directory of the unit */
	def_row = layout_bytes(tr, ntr, samp, -1, -1, nsb, nblocks, rows_per_unit) + untracked * AUTOCUT_LEAF_BYTES;
	/* the keys first, in the order of the index, then the time leaves: a range of a time leaf is the lookup of the rows of a key that has no key */
	for (int pass = 0; pass < 2; pass++)
		for (int t = 0; t < ntr; t++)
		{
			double		fill = 0,
						u;
			bool		fired;

			if (!(pass == 0 ? tr[t].cand : tr[t].time))
				continue;
			/* the first look asks a quarter of the units: a sample of a few superblocks puts one hit at 1/f units, so it cannot tell 16 from 64, but it can tell a key that sits in few */
			if (pass == 0)
				/* the key list of a leaf is kept where it fits the share of the bytes of a unit (SIFT_CSET_PRES_PCT) */
				u = units_read(&tr[t], samp->nsb, nsb, rows_per_unit,
							   AUTOCUT_KEY_BYTES * tr[t].ncells / samp->nsb <= SIFT_CSET_PRES_PCT / 100.0 * rows_per_unit * def_row,
							   &fill, &fired);
			else
				u = ranges_read(&tr[t], samp->n, samp->nsb, nsb, rows_per_unit, &fill, &fired);
			if (first < 0)
			{
				first = t;
				v->reads = u;
				v->ideal = fill;
				v->range = pass == 1;
			}
			if (fired)
			{
				fire[nfire] = t;
				reads[nfire] = u;
				fills[nfire++] = fill;
			}
		}
	leaf = cut_name(index, sc, tr[nfire > 0 ? fire[0] : first].c);
	strlcpy(v->leaf, leaf, sizeof(v->leaf));
	if (nfire == 0)
	{
		v->reason = "an equality on the key reads few units, or not many times those its rows fill";
		goto done;
	}
	/* a leaf that follows the order of the table would lose its zones to the cut: the first look says so as well as the sample does, and ends there. The cut of a lane serves
	 * a test on the lane only, so any other leaf whose zones skip units (its equality keeps less than all of them) would keep all of them under it */
	for (int i = 0; i < nfire; i++)
		for (int t = 0; t < ntr && !blocked[i]; t++)
		{
			int			m;
			const char *what = NULL;

			if (t == fire[i])
				continue;
			if (!scattered(&tr[t], samp->nsb))
				what = "follows the order of the table, and the cut would widen its zones";
			else if (tr[fire[i]].shape && zone_share(&tr[t], samp->nsb, &m) * (100 + AUTOCUT_OTHER_PCT) < 100)
				what = "has zones that skip units, and the cut by a lane would keep them all";
			if (what == NULL)
				continue;
			blocked[i] = true;
			if (i == 0)
				v->reason = psprintf("the leaf \"%s\" %s", cut_name(index, sc, tr[t].c), what);
		}
	for (first_open = 0; first_open < nfire && blocked[first_open];)
		first_open++;
	if (first_open == nfire)
		goto done;
	/* a time leaf is cut by or left alone, never advised, so the first look prices its cut and ends there when the cost is well over the benefit */
	if (quick)
	{
		for (int i = first_open; i < nfire; i++)
			if (!blocked[i] && tr[fire[i]].time)
			{
				double		g = 100.0 * (layout_bytes(tr, ntr, samp, fire[i], -1, nsb, nblocks, rows_per_unit) + untracked * AUTOCUT_LEAF_BYTES - def_row) / def_row;

				if (cut_benefit(cut_speedup(reads[i], fills[i], rows_per_unit), tr[fire[i]].shape ? 0.5 : 1.0) + AUTOCUT_QUICK_SLACK < cut_cost(g, ntr + untracked))
				{
					blocked[i] = true;
					if (i == 0)
						v->reason = "a range of the leaf reads many units, but the cost of the cut is above its benefit";
				}
			}
		for (first_open = 0; first_open < nfire && blocked[first_open];)
			first_open++;
		if (first_open == nfire)
			goto done;
		v->maybe = true;
		goto done;
	}
	/* the first that fires and whose benefit is at least its cost is cut by; when every one that fires costs more than it gains, the first is advised */
	for (int i = 0; i < nfire && v->res != SIFT_AUTOCUT_CUT; i++)
	{
		int			pick = fire[i],
					sec;
		double		growth,
					benefit,
					cost;
		bool		pays;

		if (blocked[i])
			continue;
		kc_row = layout_bytes(tr, ntr, samp, pick, -1, nsb, nblocks, rows_per_unit) + untracked * AUTOCUT_LEAF_BYTES;
		/*
		 * The rows of a key are sorted by the first date or timestamp leaf, scattered over the table like every other leaf (or the cut would be blocked): a key
		 * with more rows than a unit is split by date, and a window of dates skips the units outside it. Taken when the sample predicts an index not larger by more than AUTOCUT_SECOND_SLACK.
		 */
		sec = -1;
		for (int t = 0; t < ntr && !tr[pick].time && sec < 0; t++)
			if (t != pick && (time_type(tr[t].type) || (tr[t].shape != NULL && tr[t].lane == 1)))
			{
				double		row2 = layout_bytes(tr, ntr, samp, pick, t, nsb, nblocks, rows_per_unit) + untracked * AUTOCUT_LEAF_BYTES;

				if (row2 <= kc_row * (1 + AUTOCUT_SECOND_SLACK / 100.0))
				{
					sec = t;
					kc_row = row2;
				}
			}
		growth = 100.0 * (kc_row - def_row) / def_row;
		benefit = cut_benefit(cut_speedup(reads[i], fills[i], rows_per_unit), tr[pick].shape ? 0.5 : 1.0);
		cost = cut_cost(growth, ntr + untracked);
		pays = benefit >= cost;
		if (tr[pick].time && !pays)
		{
			if (v->res == SIFT_AUTOCUT_NONE && i == first_open)
				v->reason = "a range of the leaf reads many units, but the cost of the cut is above its benefit";
			continue;
		}
		if (pays || v->res == SIFT_AUTOCUT_NONE)
		{
			strlcpy(v->leaf, cut_name(index, sc, tr[pick].c), sizeof(v->leaf));
			v->reads = reads[i];
			v->ideal = fills[i];
			v->range = tr[pick].time;
			v->growth = growth;
			v->benefit = benefit;
			v->cost = cost;
			v->second[0] = '\0';
			if (sec >= 0)
				strlcpy(v->second, cut_name(index, sc, tr[sec].c), sizeof(v->second));
			v->reason = NULL;
			v->res = pays ? SIFT_AUTOCUT_CUT : SIFT_AUTOCUT_ADVISED;
		}
	}
	if (v->res == SIFT_AUTOCUT_CUT)
		v->reason = v->range ? "a range of the leaf reads many units and the benefit of the cut is at least its cost" : "an equality on the key reads many units and the benefit of the cut is at least its cost";
	else if (v->res == SIFT_AUTOCUT_ADVISED)
		v->reason = v->range ? "a range of the leaf reads many units, but the cost of the cut is above its benefit" : "an equality on the key reads many units, but the cost of the cut is above its benefit";
done:
	MemoryContextSwitchTo(old);
	if (v->reason != NULL)
		v->reason = pstrdup(v->reason);
	MemoryContextDelete(cx);
}

/*
 * The first look reads at most AUTOCUT_QUICK_SBS superblocks and ends at the usual answer, none, for a table whose keys sit in few of them; only a key that reads
 * a quarter of the units the rule asks for goes on to the sample of the rule. dry (sift.key_cut_decision) always reads the full sample, for its numbers.
 */
static void
decide(Relation heap, Relation index, bool dry, CutVerdict *v)
{
	if (!dry)
	{
		decide_stage(heap, index, dry, v, true);
		if (!v->maybe)
			return;
	}
	decide_stage(heap, index, dry, v, false);
}

/*
 * Before a build of an index with no key_cut of its own: reads the sample and writes 'auto: col' to the option when the rule above holds (and 'auto' when
 * an earlier build chose a key and this one does not). Returns what it did.
 */
SiftAutoCut
sift_col_auto_cut(Relation heap, Relation index)
{
	CutVerdict	v;

	decide(heap, index, false, &v);
	if (v.res == SIFT_AUTOCUT_CUT)
	{
		StringInfoData opt;

		initStringInfo(&opt);
		appendStringInfo(&opt, "auto: %s%s%s", v.leaf, v.second[0] ? ", " : "", v.second);
		sift_col_set_option(index, "key_cut", opt.data);
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": key_cut = '%s' was chosen: %s \"%s\" would read about %.0f of %.0f units, where %.0f hold its rows (predicted index size %+.1f%%, benefit %.0f against cost %.0f)",
						RelationGetRelationName(index), opt.data + 6, v.range ? "a range holding the rows of one unit of" : "an equality on", v.leaf, v.reads, v.units, v.ideal, v.growth, v.benefit, v.cost),
				 errdetail("A sample of %.0f superblocks shows the values of \"%s\" spread over the units, and no other leaf of the index follows the order of the table. The units are cut by the leaf, so %s reads the few units of its range; the addresses of the rows of a unit are in the index.%s%s",
						   v.sampled, v.leaf, v.range ? "a range" : "an equality",
						   v.range ? " Reading all the rows of one key of another leaf takes more units." : "",
						   v.second[0] ? psprintf(" The rows of a key are sorted by \"%s\" (the index is predicted to be no larger), so a key with more rows than a unit is split by it and a range of its values skips the units outside.", v.second) : ""),
				 errhint("ALTER INDEX %s SET (key_cut = 'off') and REINDEX keep the units by block range.", quote_identifier(RelationGetRelationName(index)))));
	}
	else
	{
		if (v.decided && sift_col_options_key_cut(index)[0] != '\0')
			sift_col_set_option(index, "key_cut", "auto");
		if (v.res == SIFT_AUTOCUT_ADVISED)
		{
			const char *keys = v.second[0] ? psprintf("%s, %s", v.leaf, v.second) : v.leaf;

			ereport(NOTICE,
					(errmsg("sift_col index \"%s\": %s \"%s\" reads about %.0f of %.0f units, where %.0f hold its rows; key_cut = '%s' would cut that, and the index would grow by %.1f%%",
							RelationGetRelationName(index), v.range ? "a range holding the rows of one unit of" : "an equality on", v.leaf, v.reads, v.units, v.ideal, keys, v.growth),
					 errdetail("The predicted size comes from a sample of %.0f superblocks; an automatic cut is chosen only when its benefit is at least its cost (here %.0f against %.0f: a gain of 5%% in a lookup is one unit of benefit, 3%% of growth and 5%% of build instructions one of cost). No other leaf follows the order of the table.",
							   v.sampled, v.benefit, v.cost),
					 errhint("CREATE INDEX .. WITH (key_cut = '%s'), or ALTER INDEX %s SET (key_cut = '%s') and REINDEX, if the lookups of this key matter more than the size.",
							 keys, quote_identifier(RelationGetRelationName(index)), keys)));
		}
	}
	return v.res;
}

PG_FUNCTION_INFO_V1(sift_col_key_cut_decision);

/* sift.key_cut_decision(index): what CREATE INDEX or REINDEX of the index would decide for key_cut now, with the measures behind it; changes nothing */
Datum
sift_col_key_cut_decision(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Relation	index = index_open(relid, AccessShareLock);
	Relation	heap;
	CutVerdict	v;
	Datum		vals[8];
	bool		nulls[8] = {false};

	if (!sift_col_is_index(index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(index))));
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	decide(heap, index, true, &v);
	vals[0] = v.leaf[0] ? CStringGetTextDatum(v.leaf) : (Datum) 0;
	nulls[0] = v.leaf[0] == '\0';
	vals[1] = Float8GetDatum(v.units);
	vals[2] = Float8GetDatum(v.sampled);
	vals[3] = Float8GetDatum(v.reads);
	vals[4] = Float8GetDatum(v.ideal);
	vals[5] = Float8GetDatum(v.growth);
	nulls[5] = v.res == SIFT_AUTOCUT_NONE;
	vals[6] = CStringGetTextDatum(v.res == SIFT_AUTOCUT_CUT ? "cut" : v.res == SIFT_AUTOCUT_ADVISED ? "advise" : "none");
	vals[7] = CStringGetTextDatum(v.reason);
	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(sift_result_row(fcinfo, vals, nulls));
}
