/*
 * col/autocut/size.c - the bytes a cut would make: the codec of the index, the leaves, the pre-aggregates
 * the units a cut yields and the rows a unit holds on a table that was never analyzed
 * the sort of a sample
 */
#include "postgres.h"
#include <math.h>
#include "access/heapam.h"
#include "utils/syscache.h"
#include "catalog/pg_statistic.h"
#include "col/am.h"
#include "core/cmp.h"
#include "col/autocut/autocut_int.h"

#define AUTOCUT_TRIAL_SBS	48		/* superblocks of the sample whose integer leaves are encoded to predict the size */

#define AUTOCUT_DIR_ROWS	256		/* rows of a unit of the cut whose directory is encoded from the sample, at least */

#define AUTOCUT_OVERHEAD_BYTES	0.17	/* what a row takes in the index besides its leaves and the directory of its unit: the headers of the segment and of the units, the page overhead and the free space of the last pages (0.13 + 0.04); the pre-aggregates, 0.03 to 0.59 a row and changed by the cut either way, are not predicted */

#define AUTOCUT_UNIT_BYTES	2000	/* what a unit takes besides its leaves, its directory and its pre-aggregates: the header, the zones, the key list, the page it ends on (288 units of 3 leaves took 0.7 MB more than the sample's leaves and directories) */

#define AUTOCUT_PAGG_UNIT	600		/* bytes a unit takes in pre-aggregates for each leaf besides its histograms (the exact sums, the counts: 1,975 bytes a unit of 3 leaves) */

#define AUTOCUT_PAGG_GROUP	2		/* ... a group of a histogram takes: its key and its count */

#define AUTOCUT_PAGG_MEASURE	7.5	/* ... and for each measure of the cube (8 at most): the non-NULL count, the sum, the smallest and the largest value (24 bytes a group of 3 leaves) */

#define AUTOCUT_PAGG_GROUPS	256		/* the groups a histogram of a unit holds at most (docs/COL-PREAGG.md) */

#define AUTOCUT_WIDTH_SHARE	0.7		/* the share of its width a value that is not repeated takes in the index (hexadecimal text of a hash: 45 bytes of 65) */

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

/* a NULL sorts last, as PostgreSQL's default: after every value (a row with INT64_MAX and a NULL are in the order of their rows) */
#define SORT_ROW_KEY(x) ((x)->nl[0] ? INT64_MAX : (x)->v[0])
#define SORT_ROW_KEY2(x) ((x)->nl[1] ? INT64_MAX : (x)->v[1])
#define SORT_ROW_TIE(x) ((x)->row)

static inline int
sort_row_cmp(const SortRow *x, const SortRow *y)
{
	if (SORT_ROW_KEY(x) != SORT_ROW_KEY(y))
		return SORT_ROW_KEY(x) < SORT_ROW_KEY(y) ? -1 : 1;
	if (SORT_ROW_KEY2(x) != SORT_ROW_KEY2(y))
		return SORT_ROW_KEY2(x) < SORT_ROW_KEY2(y) ? -1 : 1;
	return SIFT_CMP3(x->row, y->row);
}

#define SORT_ROW_LESS(x, y) (sort_row_cmp(x, y) < 0)

SIFT_DEFINE_SORT(sort_rows_cmp, SortRow, SORT_ROW_LESS)

/* a copy of the rows for the radix sort: none when it would pass the largest allocation (the comparison sort then) */
static void *
sort_row_copy(size_t bytes)
{
	return bytes <= MaxAllocSize ? palloc_extended(bytes, MCXT_ALLOC_NO_OOM) : NULL;
}

/* the sampled rows are added in the order of their rows, so the radix sort applies */
SIFT_DEFINE_RADIX_SORT(sort_rows, SortRow, SORT_ROW_KEY, SORT_ROW_KEY2, SORT_ROW_TIE, sort_row_copy, pfree, sort_rows_cmp)

/*
 * What a row takes in a leaf the sample does not read: its values dictionary-coded, the distinct ones (of the width the statistics of the column give, a share of it when they do not repeat)
 * once and a code for each row; AUTOCUT_LEAF_BYTES without statistics, for a leaf inside a document or one that is not a column.
 */
double
acut_leaf_bytes(Relation heap, const SiftColDef *d, AttrNumber hatt)
{
	HeapTuple	tp;
	double		bytes = AUTOCUT_LEAF_BYTES;

	if (hatt == 0 || d->parent >= 0 || (d->path != NULL && d->path[0] != '\0') || sift_col_def_shape(d) != 0 || heap->rd_rel->reltuples <= 0)
		return bytes;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(heap)), Int16GetDatum(hatt), BoolGetDatum(false));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_statistic st = (Form_pg_statistic) GETSTRUCT(tp);
		double		nd = st->stadistinct >= 0 ? st->stadistinct : -st->stadistinct * heap->rd_rel->reltuples;

		if (st->stawidth > 0 && nd >= 1)
			bytes = Max(AUTOCUT_LEAF_BYTES, Min(nd / heap->rd_rel->reltuples, 1.0) * st->stawidth * AUTOCUT_WIDTH_SHARE + log2(Max(nd, 2)) / 8);
		ReleaseSysCache(tp);
	}
	return bytes;
}

#define PAGG_SLOT_BITS	10
#define PAGG_SLOTS		(1 << PAGG_SLOT_BITS)	/* four times the groups of a histogram: a probe ends at a free slot */

/* the bytes of the pre-aggregates of a unit of n rows (docs/COL-PREAGG.md): a base for each leaf, and for each integer leaf with at most AUTOCUT_PAGG_GROUPS values in the unit a histogram of its groups with the measures of every leaf */
static double
pagg_unit_bytes(const Track *tr, int ntr, const uint32 *rows, uint32 n, int nleaves)
{
	double		bytes = (double) AUTOCUT_PAGG_UNIT * nleaves,
				per_group = AUTOCUT_PAGG_GROUP + AUTOCUT_PAGG_MEASURE * Min(nleaves, 8);

	for (int t = 0; t < ntr; t++)
	{
		uint32		groups = 0;
		int64		seen[PAGG_SLOTS];
		bool		used[PAGG_SLOTS] = {false};

		if (tr[t].shape != NULL)
			continue;
		/* the values of the unit, counted until there are more than a histogram holds: a column of many values ends at the first AUTOCUT_PAGG_GROUPS + 1 of them */
		for (uint32 i = 0; i < n && groups <= AUTOCUT_PAGG_GROUPS; i++)
		{
			int64		x = tr[t].nl[rows[i]] ? PG_INT64_MIN : tr[t].v[rows[i]];
			uint32		h = (uint32) (((uint64) x * UINT64CONST(0x9E3779B97F4A7C15)) >> (64 - PAGG_SLOT_BITS));

			while (used[h] && seen[h] != x)
				h = (h + 1) & (PAGG_SLOTS - 1);
			if (!used[h])
			{
				used[h] = true;
				seen[h] = x;
				groups++;
			}
		}
		if (groups <= AUTOCUT_PAGG_GROUPS)
			bytes += groups * per_group;
	}
	return bytes;
}

/*
 * The units of the rows of a sample in the order of the key, as boundaries cuts[0..n]: unit rows each, as the cut of a key-cut segment makes them (col/order.c cut_at); with a second
 * leaf (small > 0: the rows of a unit of a big key in the sample) a key of at least 2 * small rows is cut into units of small rows, the last one ending with the key, and the
 * unit before it ends where the key starts.
 */
static uint32
cut_units(const Track *k0, const uint32 *sel, uint32 nsel, uint32 unit, double small, uint32 *cuts)
{
	uint32	   *kend = palloc_array(uint32, nsel + 1),
			   *kbeg = palloc_array(uint32, nsel + 1);
	uint32		n = 0,
				pos = 0;
	bool		split = false;

	for (uint32 i = 0; i < nsel; i++)
		kbeg[i] = i > 0 && k0->nl[sel[i]] == k0->nl[sel[i - 1]] && k0->v[sel[i]] == k0->v[sel[i - 1]] ? kbeg[i - 1] : i;
	for (uint32 i = nsel; i-- > 0;)
		kend[i] = i + 1 < nsel && kbeg[i + 1] == kbeg[i] ? kend[i + 1] : i + 1;
	cuts[0] = 0;
	while (pos < nsel)
	{
		bool		big = small > 0 && kend[pos] - pos >= 2 * small;
		uint32		end;

		if (big || (split && small > 0))
		{
			split = big;
			end = big ? pos + (uint32) ceil(small) : kend[pos];
		}
		else
		{
			split = false;
			end = Min(pos + unit, nsel);
			if (small > 0 && end < nsel && kbeg[end] < end && kbeg[end] > pos && kend[kbeg[end]] - kbeg[end] >= 2 * small)
				end = kbeg[end];
		}
		cuts[++n] = end;
		pos = end;
	}
	pfree(kend);
	pfree(kbeg);
	return n;
}

/*
 * The bytes of the integer leaves and of the row directories of the rows of at most AUTOCUT_TRIAL_SBS of the sampled superblocks (every k-th) in the units of the
 * superblock cut (key < 0) or of the cut by the leaf key, per sampled row. The integer leaves are encoded by the codec in units of as many rows as a superblock
 * holds. The row directory is encoded as enc.c writes it: a unit of the cut holds the rows of the whole heap in a range of the key and lists their addresses,
 * and the sample holds a share of the heap, so a unit is the sampled rows in the range, n / nsb of them. When that is fewer than AUTOCUT_DIR_ROWS, the unit is
 * that many rows and the gaps between its blocks, then smaller than the whole heap gives, are widened by the bits of the difference.
 * units: also the headers and the pre-aggregates of the units, which the value returned then holds; plain, when given, receives the bytes without them from the same pass.
 */
double
acut_layout_bytes(const Track *tr, int ntr, int nleaves, const Samp *s, int key, int key2, bool units, double *plain, double nsb, double heap_pages, double rows_per_unit)
{
	const Track *order[2] = {key >= 0 ? &tr[key] : NULL, key2 >= 0 ? &tr[key2] : NULL};
	StringInfoData buf;
	int64	   *tmp = palloc_array(int64, s->n + 1);
	uint32	   *rows = palloc_array(uint32, s->n + 1);
	uint32	   *sel = palloc_array(uint32, s->n + 1);
	uint32	   *tidrows = key >= 0 ? palloc_array(uint32, s->n + 1) : NULL;
	uint32	   *asc = key >= 0 ? palloc_array(uint32, s->n + 1) : NULL;	/* the sampled rows in the order of their rows */
	SiftKey    *tid = palloc_array(SiftKey, s->n + 1);	/* by sampled row: its address with the taken superblocks side by side */
	SiftKey    *keys = palloc_array(SiftKey, s->n + 1);
	uint32	   *cuts = palloc_array(uint32, s->n + 2);
	uint32		ncut = 0,
				nsel = 0,
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
	unit = key >= 0 ? (uint32) Max(nsel / nsb, AUTOCUT_DIR_ROWS) : Max(1, nsel / nused);	/* a unit of the cut holds the rows of the whole heap in a range of the key, the sampled share of them */
	narrower = Max(0, log2(nsb / nused));	/* the units of the sample are sorted slices of nused superblocks: a unit of the whole heap holds a key range this much narrower, so its gaps take this many bits fewer */
	initStringInfo(&buf);
	if (key >= 0)
	{
		SortRow    *by = palloc_array(SortRow, nsel);
		double		wider,
					small = key2 >= 0 && rows_per_unit >= 2.0 * SIFT_COLK_SECOND_ROWS ? Max(SIFT_COLK_SECOND_ROWS * (double) nsel / nsb / rows_per_unit, AUTOCUT_DIR_ROWS / 4) : 0;

		for (uint32 i = 0; i < nsel; i++)
		{
			by[i] = (SortRow) {.row = sel[i]};
			for (int k = 0; k < 2 && order[k] != NULL; k++)
			{
				by[i].nl[k] = order[k]->nl[sel[i]];
				by[i].v[k] = order[k]->v[sel[i]];
			}
		}
		memcpy(asc, sel, nsel * sizeof(uint32));
		sort_rows(by, nsel);
		for (uint32 i = 0; i < nsel; i++)
			sel[i] = by[i].row;
		pfree(by);
		ncut = cut_units(order[0], sel, nsel, unit, small, cuts);
		/* the rows of each unit in the order of their rows (their addresses), whatever the leaf: a pass over the rows in that order, each to the next place of its unit */
		{
			uint32	   *unit_of = palloc_array(uint32, s->n + 1);
			uint32	   *next = palloc_array(uint32, ncut + 1);

			for (uint32 u = 0; u < ncut; u++)
			{
				next[u] = cuts[u];
				for (uint32 k = cuts[u]; k < cuts[u + 1]; k++)
					unit_of[sel[k]] = u;
			}
			for (uint32 i = 0; i < nsel; i++)
				tidrows[next[unit_of[asc[i]]]++] = asc[i];
			pfree(unit_of);
			pfree(next);
		}
		wider = Max(0, log2(heap_pages / rows_per_unit / ((double) nused * SIFT_SB_PAGES / unit)));
		for (uint32 u = 0; u < ncut; u++)
		{
			uint32		from = cuts[u],
						n = cuts[u + 1] - from;

			for (uint32 i = 0; i < n; i++)
				keys[i] = tid[tidrows[from + i]];	/* the rows of a unit in the order of their rows are in the order of their addresses */
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
			for (uint32 u = 0; u < ncut; u++)
			{
				uint32		from = cuts[u],
							n = cuts[u + 1] - from;
				double		b;

				b = encoded_bytes(&tr[t], tidrows + from, n, &buf, tmp);
				if (&tr[t] == order[0] && tr[t].shape != NULL)
					b = Max(n / 8.0, b - n * narrower / 8);
				bytes += (1 + tr[t].image) * b;
			}
	}
	if (plain != NULL)
		*plain = bytes / Max(nsel, 1) + AUTOCUT_OVERHEAD_BYTES;
	/* the pre-aggregates and the header of the units: the units of a cut are a share of a unit of the table, so their constants are that share too */
	if (units)
		bytes += AUTOCUT_UNIT_BYTES * (key < 0 ? nused : ncut * Min(1.0, (double) unit / rows_per_unit));
	if (units && key < 0)
		for (int u = 0; u < s->nsb; u += stride)
		{
			for (uint32 i = s->start[u]; i < s->start[u + 1]; i++)
				rows[i - s->start[u]] = i;
			bytes += pagg_unit_bytes(tr, ntr, rows, s->start[u + 1] - s->start[u], nleaves);
		}
	else if (units)
		for (uint32 u = 0; u < ncut; u++)
		{
			uint32		n = cuts[u + 1] - cuts[u];

			memcpy(rows, sel + cuts[u], n * sizeof(uint32));
			bytes += Min(1.0, (double) unit / rows_per_unit) * pagg_unit_bytes(tr, ntr, rows, n, nleaves);
		}
	pfree(buf.data);
	pfree(tmp);
	pfree(rows);
	pfree(sel);
	if (tidrows != NULL)
	{
		pfree(tidrows);
		pfree(asc);
	}
	pfree(tid);
	pfree(keys);
	pfree(cuts);
	return bytes / Max(nsel, 1) + AUTOCUT_OVERHEAD_BYTES;
}

/* The rows of the superblock in the middle of the heap: what a unit holds for a table that was never analyzed, and what the build aims a unit at then */
double
acut_mid_rows(Relation heap, BlockNumber nblocks, double nsb)
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
