/*
 * col/autocut.c - the default of the option key_cut: CREATE INDEX and REINDEX of an index without key_cut (or with key_cut = 'auto')
 * cut the units by a key when a sample of the heap says that zones cannot skip the units of that key. The key is the first leaf, in the order of the index,
 * that is key-shaped (sift_col_key_shaped); when none of those is cut by, the first date or timestamp leaf, for a range of its values (ranges_read). A leaf is
 * chosen when an equality on one of its values reads at least AUTOCUT_RATIO times the units its rows fill
 * and at least AUTOCUT_MIN_UNITS, no other leaf of the index follows the order of the table (key_cut would widen its zones), and the index would not grow by
 * more than AUTOCUT_GROWTH_PCT. The size is predicted from the sample: the integer leaves of the sampled rows are encoded by the codec of the index in the
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
#include "col/build.h"
#include "col/codec.h"
#include "col/enc.h"
#include "col/col.h"
#include "col/setsum.h"
#include "core/sql.h"

#define AUTOCUT_MIN_UNITS	64		/* units an equality reads, at least */
#define AUTOCUT_RATIO		8		/* times the units its rows fill, at least */
#define AUTOCUT_KEYS		32		/* keys taken from the sample, more than half of them must read that many */
#define AUTOCUT_SBS			256		/* superblocks sampled, at most */
#define AUTOCUT_MIN_SBS		32
#define AUTOCUT_QUICK_SBS	8		/* superblocks of the first look, which most tables end at: it prices the decision at a share of the heap, not at a pass over it */
#define AUTOCUT_ROWS		400000	/* rows sampled, at most */
#define AUTOCUT_TRIAL_SBS	48		/* superblocks of the sample whose integer leaves are encoded to predict the size */
#define AUTOCUT_LEAVES		8		/* leaves read: the candidates and those that may compete with them */
#define AUTOCUT_DIR_ROWS	256		/* rows of a unit of the cut whose directory is encoded from the sample, at least */
#define AUTOCUT_LEAF_BYTES	1.0		/* what a row takes in a leaf that is not read from the sample */
#define AUTOCUT_KEY_BYTES	2.0		/* what a key takes in the key list of a unit */
#define AUTOCUT_GROWTH_PCT	3		/* the predicted growth of the index, at most (the 3.3 rule for a default) */
#define AUTOCUT_QUICK_SLACK	4		/* points over AUTOCUT_GROWTH_PCT that the first look still sends a time leaf to the sample: its prediction from a few superblocks is rough */

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
	bool		time;			/* a date or timestamp that is not a key: a range of its values is tested */
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

static int
i64_cmp(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return x < y ? -1 : x > y;
}

static int
dbl_cmp(const void *a, const void *b)
{
	double		x = *(const double *) a,
				y = *(const double *) b;

	return x < y ? -1 : x > y;
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

/* whether the zones of the sampled superblocks of a leaf leave most of them out of an equality (the verdict "scattered" of sift.col_clustering) */
static bool
scattered(const Track *t, int n)
{
	double		lo[AUTOCUT_SBS],
				hi[AUTOCUT_SBS],
				point = 0;
	int			m = 0;

	for (int i = 0; i < n; i++)
		if (t->has[i])
		{
			lo[m] = t->lo[i];
			hi[m++] = t->hi[i];
		}
	if (m < 2)
		return false;
	for (int i = 0; i < m; i++)
	{
		double		mid = (lo[i] + hi[i]) / 2;

		for (int j = 0; j < m; j++)
			point += lo[j] <= mid && mid <= hi[j];
	}
	point /= m;
	return !(point <= 2 || point / m <= 0.25);
}

/*
 * The sampled superblocks of a key candidate give the units an equality on its keys reads: the median over AUTOCUT_KEYS keys taken at random rows, and the
 * median of the units their rows fill. fired: more than half of the keys read AUTOCUT_MIN_UNITS and AUTOCUT_RATIO times the units their rows fill. keyed: the
 * index keeps a key list of the leaf.
 */
static double
units_read(Track *t, int nsampled, double nsb, double rows_per_unit, bool keyed, double min_units, double *ideal, bool *fired)
{
	double		f = nsampled / nsb,
				total = 0,
				step,
				at,
				reads[AUTOCUT_KEYS],
				fills[AUTOCUT_KEYS];
	int			nk = 0,
				nfire = 0;
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
			nfire += reads[nk] >= min_units && reads[nk] >= AUTOCUT_RATIO * fills[nk];
			nk++;
			at += step;
		}
		seen += rows;
		i = j;
	}
	if (nk == 0)
		return 0;
	*fired = nfire * 2 > nk;
	qsort(reads, nk, sizeof(double), dbl_cmp);
	qsort(fills, nk, sizeof(double), dbl_cmp);
	*ideal = fills[nk / 2];
	return reads[nk / 2];
}

/*
 * The same for the values of a time leaf, which do not repeat: AUTOCUT_KEYS ranges at places spread over the sorted values of the sample, each holding the
 * rows of one unit (a day, a week of a table that takes it). A unit is read when its zone meets the range, so the units read are the sampled superblocks
 * that meet it (the first stands for itself, the others for 1/f each), and the units its rows fill are those of the rows with a value in it.
 */
static double
ranges_read(const Track *t, uint32 nrows, int nsampled, double nsb, double min_units, double *ideal, bool *fired)
{
	double		f = nsampled / nsb,
				reads[AUTOCUT_KEYS],
				fills[AUTOCUT_KEYS];
	int64	   *s = palloc_array(int64, nrows);
	uint32		n = 0,
				half;
	int			nfire = 0;

	*fired = false;
	for (uint32 i = 0; i < nrows; i++)
		if (!t->nl[i])
			s[n++] = t->v[i];
	if (n < AUTOCUT_KEYS)
	{
		pfree(s);
		return 0;
	}
	qsort(s, n, sizeof(int64), i64_cmp);
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
		nfire += reads[k] >= min_units && reads[k] >= AUTOCUT_RATIO * fills[k];
	}
	pfree(s);
	*fired = nfire * 2 > AUTOCUT_KEYS;
	qsort(reads, AUTOCUT_KEYS, sizeof(double), dbl_cmp);
	qsort(fills, AUTOCUT_KEYS, sizeof(double), dbl_cmp);
	*ideal = fills[AUTOCUT_KEYS / 2];
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

static int
row_cmp(const void *a, const void *b, void *arg)
{
	const Track *k = arg;
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	if (k->nl[x] != k->nl[y])
		return k->nl[x] ? 1 : -1;
	if (!k->nl[x] && k->v[x] != k->v[y])
		return k->v[x] < k->v[y] ? -1 : 1;
	return x < y ? -1 : x > y;
}

static int
u64_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

static int
u32_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return x < y ? -1 : x > y;
}

/*
 * The bytes of the integer leaves and of the row directories of the rows of at most AUTOCUT_TRIAL_SBS of the sampled superblocks (every k-th) in the units of the
 * superblock cut (key < 0) or of the cut by the leaf key, per sampled row. The integer leaves are encoded by the codec in units of as many rows as a superblock
 * holds. The row directory is encoded as enc.c writes it: a unit of the cut holds the rows of the whole heap in a range of the key and lists their addresses,
 * and the sample holds a share of the heap, so a unit is the sampled rows in the range, n / nsb of them. When that is fewer than AUTOCUT_DIR_ROWS, the unit is
 * that many rows and the gaps between its blocks, then smaller than the whole heap gives, are widened by the bits of the difference.
 */
static double
layout_bytes(const Track *tr, int ntr, const Samp *s, int key, double nsb, double heap_pages, double rows_per_unit)
{
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
	double		bytes = 0;

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
	unit = Max(1, nsel / nused);
	initStringInfo(&buf);
	if (key >= 0)
	{
		uint32		dir_rows = Max(nsel / nsb, AUTOCUT_DIR_ROWS);
		double		wider = Max(0, log2(heap_pages / rows_per_unit / ((double) nused * SIFT_SB_PAGES / dir_rows)));

		qsort_arg(sel, nsel, sizeof(uint32), row_cmp, (void *) &tr[key]);
		for (uint32 from = 0; from < nsel; from += dir_rows)
		{
			uint32		n = Min(dir_rows, nsel - from);

			for (uint32 i = 0; i < n; i++)
				keys[i] = tid[sel[from + i]];
			qsort(keys, n, sizeof(SiftKey), u64_cmp);
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
				bytes += encoded_bytes(&tr[t], rows, s->start[u + 1] - s->start[u], &buf, tmp);
			}
		else
			for (uint32 from = 0; from < nsel; from += unit)
			{
				uint32		n = Min(unit, nsel - from);

				memcpy(rows, sel + from, n * sizeof(uint32));
				qsort(rows, n, sizeof(uint32), u32_cmp);	/* the rows of a unit are in TID order */
				bytes += encoded_bytes(&tr[t], rows, n, &buf, tmp);
			}
	}
	pfree(buf.data);
	pfree(tmp);
	pfree(rows);
	pfree(sel);
	pfree(tid);
	pfree(keys);
	return bytes / Max(nsel, 1);
}

typedef struct CutVerdict
{
	SiftAutoCut res;
	bool		decided;		/* the options let a build decide */
	bool		maybe;			/* the first look found a key that may be worth cutting by: the full sample decides */
	const char *reason;
	char		leaf[NAMEDATALEN];
	double		units;
	double		sampled;
	double		reads;
	double		ideal;
	double		growth;
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
	else if (nsb < AUTOCUT_MIN_UNITS)
		v->reason = "the table has fewer than 64 units";
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
		else
			untracked++;
	}
	rows_per_unit = heap->rd_rel->reltuples > 0 ? heap->rd_rel->reltuples / nsb : mid_rows(heap, nblocks, nsb);
	full = (int) Min(Min(Min(AUTOCUT_SBS, nsb), Max(ceil(nsb / 4), AUTOCUT_MIN_SBS)), AUTOCUT_ROWS / Max(rows_per_unit, 1));	/* a quarter of the heap at most: the sample costs what a build of the share costs */
	if (ncand == 0)
	{
		v->reason = "no key-shaped column (a foreign key or *_id, not unique, int2, int4, int8 or date) and no date or timestamp column";
		goto done;
	}
	if (full < AUTOCUT_MIN_SBS)
	{
		v->reason = "the units are too large for a sample";
		goto done;
	}
	want = quick ? Min(full, AUTOCUT_QUICK_SBS) : full;
	samp = palloc0(sizeof(Samp));
	samp->tid = palloc_array(SiftKey, AUTOCUT_ROWS + SIFT_COL_UNIT_MAX_ROWS);
	for (int t = 0; t < ntr; t++)
	{
		tr[t].v = palloc_array(int64, AUTOCUT_ROWS + SIFT_COL_UNIT_MAX_ROWS);
		tr[t].nl = palloc_array(bool, AUTOCUT_ROWS + SIFT_COL_UNIT_MAX_ROWS);
	}
	slot = table_slot_create(heap, NULL);
	for (int s = 0; s < want; s++)
	{
		/* one superblock from each stretch of the heap, at a place that varies with the stretch and not with the run: the same table gives the same sample */
		BlockNumber start = (BlockNumber) ((s + ((uint32) s * 2654435761u % 1024) / 1024.0) * nsb / want) * SIFT_SB_PAGES;
		TableScanDesc scan;
		uint32		from = samp->n,
					nrow = 0;

		if (start >= nblocks || samp->n + SIFT_COL_UNIT_MAX_ROWS > AUTOCUT_ROWS + SIFT_COL_UNIT_MAX_ROWS)
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
				if (!isnull)
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
			qsort(tmp, n, sizeof(int64), i64_cmp);
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
	def_row = layout_bytes(tr, ntr, samp, -1, nsb, nblocks, rows_per_unit) + untracked * AUTOCUT_LEAF_BYTES;
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
							   quick ? AUTOCUT_MIN_UNITS / 4 : AUTOCUT_MIN_UNITS, &fill, &fired);
			else
				u = ranges_read(&tr[t], samp->n, samp->nsb, nsb, quick ? AUTOCUT_MIN_UNITS / 4 : AUTOCUT_MIN_UNITS, &fill, &fired);
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
	leaf = NameStr(TupleDescAttr(RelationGetDescr(index), sc->cols[tr[nfire > 0 ? fire[0] : first].c].attno - 1)->attname);
	strlcpy(v->leaf, leaf, sizeof(v->leaf));
	if (nfire == 0)
	{
		v->reason = "an equality on the key reads few units, or not many times those its rows fill";
		goto done;
	}
	/* a leaf that follows the order of the table would lose its zones to the cut: the first look says so as well as the sample does, and ends there */
	for (int i = 0; i < nfire; i++)
		for (int t = 0; t < ntr && !blocked[i]; t++)
			if (t != fire[i] && !scattered(&tr[t], samp->nsb))
			{
				blocked[i] = true;
				if (i == 0)
					v->reason = psprintf("the leaf \"%s\" follows the order of the table, and the cut would widen its zones",
										 NameStr(TupleDescAttr(RelationGetDescr(index), sc->cols[tr[t].c].attno - 1)->attname));
			}
	for (first_open = 0; first_open < nfire && blocked[first_open];)
		first_open++;
	if (first_open == nfire)
		goto done;
	/* a time leaf is cut by or left alone, never advised, so the first look prices its cut and ends there when it grows the index well over the limit */
	if (quick)
	{
		for (int i = first_open; i < nfire; i++)
			if (!blocked[i] && tr[fire[i]].time &&
				100.0 * (layout_bytes(tr, ntr, samp, fire[i], nsb, nblocks, rows_per_unit) + untracked * AUTOCUT_LEAF_BYTES - def_row) / def_row > AUTOCUT_GROWTH_PCT + AUTOCUT_QUICK_SLACK)
			{
				blocked[i] = true;
				if (i == 0)
					v->reason = "a range of the leaf reads many units, but the index would grow by more than 3%";
			}
		for (first_open = 0; first_open < nfire && blocked[first_open];)
			first_open++;
		if (first_open == nfire)
			goto done;
		v->maybe = true;
		goto done;
	}
	/* the first that fires and does not grow the index is cut by; when every one that fires would grow it, the first is advised */
	for (int i = 0; i < nfire && v->res != SIFT_AUTOCUT_CUT; i++)
	{
		int			pick = fire[i];
		double		growth;

		if (blocked[i])
			continue;
		kc_row = layout_bytes(tr, ntr, samp, pick, nsb, nblocks, rows_per_unit) + untracked * AUTOCUT_LEAF_BYTES;
		growth = 100.0 * (kc_row - def_row) / def_row;
		if (tr[pick].time && growth > AUTOCUT_GROWTH_PCT)
		{
			if (v->res == SIFT_AUTOCUT_NONE && i == first_open)
				v->reason = "a range of the leaf reads many units, but the index would grow by more than 3%";
			continue;
		}
		if (growth <= AUTOCUT_GROWTH_PCT || v->res == SIFT_AUTOCUT_NONE)
		{
			strlcpy(v->leaf, NameStr(TupleDescAttr(RelationGetDescr(index), sc->cols[tr[pick].c].attno - 1)->attname), sizeof(v->leaf));
			v->reads = reads[i];
			v->ideal = fills[i];
			v->range = tr[pick].time;
			v->growth = growth;
			v->reason = NULL;
			v->res = growth <= AUTOCUT_GROWTH_PCT ? SIFT_AUTOCUT_CUT : SIFT_AUTOCUT_ADVISED;
		}
	}
	if (v->res == SIFT_AUTOCUT_CUT)
		v->reason = v->range ? "a range of the leaf reads many units and the index does not grow" : "an equality on the key reads many units and the index does not grow";
	else if (v->res == SIFT_AUTOCUT_ADVISED)
		v->reason = v->range ? "a range of the leaf reads many units, but the index would grow by more than 3%" : "an equality on the key reads many units, but the index would grow by more than 3%";
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
sift_col_auto_cut(Relation heap, Relation index, IndexInfo *info)
{
	CutVerdict	v;

	decide(heap, index, false, &v);
	if (v.res == SIFT_AUTOCUT_CUT)
	{
		StringInfoData opt;

		initStringInfo(&opt);
		appendStringInfo(&opt, "auto: %s", v.leaf);
		sift_col_set_option(index, "key_cut", opt.data);
		if (!sift_colb_plan_runs(heap, index, info, true))
		{
			/* the rows of the table make more runs than the directory holds with this maintenance_work_mem: a cut that cannot be built is not chosen */
			sift_col_set_option(index, "key_cut", "auto");
			ereport(NOTICE,
					(errmsg("sift_col index \"%s\": key_cut = '%s' is not chosen: the build would need more segments than the directory holds with maintenance_work_mem = %d kB",
							RelationGetRelationName(index), v.leaf, maintenance_work_mem),
					 errhint("CREATE INDEX .. WITH (key_cut = '%s') says the maintenance_work_mem it needs.", v.leaf)));
			return SIFT_AUTOCUT_ADVISED;
		}
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": key_cut = '%s' was chosen: %s \"%s\" would read about %.0f of %.0f units, where %.0f hold its rows (predicted index size %+.1f%%)",
						RelationGetRelationName(index), v.leaf, v.range ? "a range holding the rows of one unit of" : "an equality on", v.leaf, v.reads, v.units, v.ideal, v.growth),
				 errdetail("A sample of %.0f superblocks shows the values of \"%s\" spread over the units, and no other leaf of the index follows the order of the table. The units are cut by the leaf, so %s reads the few units of its range; the addresses of the rows of a unit are in the index.%s",
						   v.sampled, v.leaf, v.range ? "a range" : "an equality",
						   v.range ? " Reading all the rows of one key of another leaf takes more units." : ""),
				 errhint("ALTER INDEX %s SET (key_cut = 'off') and REINDEX keep the units by block range.", quote_identifier(RelationGetRelationName(index)))));
	}
	else
	{
		if (v.decided && sift_col_options_key_cut(index)[0] != '\0')
			sift_col_set_option(index, "key_cut", "auto");
		if (v.res == SIFT_AUTOCUT_ADVISED)
			ereport(NOTICE,
					(errmsg("sift_col index \"%s\": %s \"%s\" reads about %.0f of %.0f units, where %.0f hold its rows; key_cut = '%s' would cut that, and the index would grow by %.1f%%",
							RelationGetRelationName(index), v.range ? "a range holding the rows of one unit of" : "an equality on", v.leaf, v.reads, v.units, v.ideal, v.leaf, v.growth),
					 errdetail("The predicted size comes from a sample of %.0f superblocks; an automatic cut is chosen only when the index does not grow by more than %d%%. No other leaf follows the order of the table.",
							   v.sampled, AUTOCUT_GROWTH_PCT),
					 errhint("CREATE INDEX .. WITH (key_cut = '%s'), or ALTER INDEX %s SET (key_cut = '%s') and REINDEX, if the lookups of this key matter more than the size.",
							 v.leaf, quote_identifier(RelationGetRelationName(index)), v.leaf)));
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
