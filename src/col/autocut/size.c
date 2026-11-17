/*
 * col/autocut/size.c - the bytes a cut would make: the codec of the index, the leaves, the pre-aggregates
 * the units a cut yields and the rows a unit holds on a table that was never analyzed
 * the sort of a sample
 */
/* check-memory: allow-allocs the rows of the sample are in spools held to maintenance_work_mem; the arrays follow one superblock or one unit of a layout */
#include "postgres.h"
#include <math.h>
#include "access/heapam.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "catalog/pg_statistic.h"
#include "col/am.h"
#include "core/cmp.h"
#include "core/hash.h"
#include "col/autocut/autocut_int.h"
#include "core/membudget.h"

#define AUTOCUT_TRIAL_SBS	48		/* superblocks of the sample whose integer leaves are encoded to predict the size */

#define AUTOCUT_DIR_ROWS	256		/* rows of a unit of the cut whose directory is encoded from the sample, at least */

#define AUTOCUT_OVERHEAD_BYTES	0.17	/* what a row takes in the index besides its leaves and the directory of its unit: the headers of the segment and of the units, the page overhead and the free space of the last pages (0.13 + 0.04); the pre-aggregates, 0.03 to 0.59 a row and changed by the cut either way, are not predicted */

#define AUTOCUT_UNIT_BYTES	2000	/* what a unit takes besides its leaves, its directory and its pre-aggregates: the header, the zones, the key list, the page it ends on (288 units of 3 leaves took 0.7 MB more than the sample's leaves and directories) */

#define AUTOCUT_PAGG_UNIT	600		/* bytes a unit takes in pre-aggregates for each leaf besides its histograms (the exact sums, the counts: 1,975 bytes a unit of 3 leaves) */

#define AUTOCUT_PAGG_GROUP	2		/* ... a group of a histogram takes: its key and its count */

#define AUTOCUT_PAGG_MEASURE	7.5	/* ... and for each measure of the cube (8 at most): the non-NULL count, the sum, the smallest and the largest value (24 bytes a group of 3 leaves) */

#define AUTOCUT_PAGG_GROUPS	256		/* the groups a histogram of a unit holds at most (docs/COL-PREAGG.md) */

#define AUTOCUT_WIDTH_SHARE	0.7		/* the share of its width a value that is not repeated takes in the index (hexadecimal text of a hash: 45 bytes of 65) */

/* a record of the window with its row, to put the rows of a unit in the order of their rows */
typedef struct OrdRow
{
	uint32		row;
	uint32		at;
} OrdRow;

#define ORD_ROW_KEY(x) ((int64_t) (x)->row)
#define ORD_ROW_KEY2(x) 0
#define ORD_ROW_TIE(x) ((x)->at)
#define ORD_ROW_LESS(x, y) ((x)->row < (y)->row)

SIFT_DEFINE_SORT(ord_rows_cmp, OrdRow, ORD_ROW_LESS)

static void *
ord_row_copy(size_t bytes)
{
	return bytes <= MaxAllocSize ? palloc_extended(bytes, MCXT_ALLOC_NO_OOM) : NULL;
}

SIFT_DEFINE_RADIX_SORT(sort_ord_rows, OrdRow, ORD_ROW_KEY, ORD_ROW_KEY2, ORD_ROW_TIE, ord_row_copy, pfree, ord_rows_cmp)

/* the columns of the rows of one unit of a layout, in the order of their addresses */
typedef struct Unit
{
	uint32		n,
				cap;
	SiftKey    *tid;
	int64	   *v[AUTOCUT_LEAVES];
	bool	   *nl[AUTOCUT_LEAVES];
	int64	   *tmp;
	OrdRow	   *ord;			/* the rows of the unit as the window holds them, to be put in the order of their rows */
} Unit;

static void
unit_reserve(Unit *u, int ntr, uint32 n)
{
	if (n <= u->cap)
		return;
	if (u->cap > 0)
	{
		pfree(u->tid);
		pfree(u->tmp);
		pfree(u->ord);
		for (int t = 0; t < ntr; t++)
		{
			pfree(u->v[t]);
			pfree(u->nl[t]);
		}
	}
	u->cap = Max(n, Max(2 * u->cap, 1024));
	u->tid = palloc_array(SiftKey, u->cap);
	u->tmp = palloc_array(int64, u->cap);
	u->ord = palloc_array(OrdRow, u->cap);
	for (int t = 0; t < ntr; t++)
	{
		u->v[t] = palloc_array(int64, u->cap);
		u->nl[t] = palloc_array(bool, u->cap);
	}
}

/* row i of the unit: the record of the sample, its address moved by shift (the superblocks of the layout side by side) */
static void
unit_put(Unit *u, int ntr, uint32 i, const void *rec, SiftKey shift)
{
	u->tid[i] = ROW_TID(rec) + shift;
	for (int t = 0; t < ntr; t++)
	{
		u->v[t][i] = ROW_V(rec, t);
		u->nl[t][i] = ROW_NL(rec, ntr, t);
	}
}

/* the bytes the codec of the index writes for the values of the leaf in the rows of the unit (a NULL takes the value before it, as in a unit) */
static double
encoded_bytes(Unit *u, int t, StringInfo buf)
{
	SiftCcPolicy pol;
	int64		prev = 0;

	for (uint32 i = 0; i < u->n; i++)
	{
		u->tmp[i] = u->nl[t][i] ? prev : u->v[t][i];
		prev = u->tmp[i];
	}
	resetStringInfo(buf);
	sift_cc_policy_default(&pol);
	sift_cc_encode(buf, SIFT_CC_INT, u->tmp, u->n, NULL, &pol, NULL);
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
	void	   *copy = bytes <= MaxAllocSize ? palloc_extended(bytes, MCXT_ALLOC_NO_OOM) : NULL;

	SIFT_MEM_CHECKPOINT();
	return copy;
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

/* the bytes of the pre-aggregates of a unit (docs/COL-PREAGG.md): a base for each leaf, and for each integer leaf with at most AUTOCUT_PAGG_GROUPS values in the unit a histogram of its groups with the measures of every leaf */
static double
pagg_unit_bytes(const Track *tr, int ntr, const Unit *u, int nleaves)
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
		for (uint32 i = 0; i < u->n && groups <= AUTOCUT_PAGG_GROUPS; i++)
		{
			int64		x = u->nl[t][i] ? PG_INT64_MIN : u->v[t][i];
			uint32		h = (uint32) (sift_fib64((uint64) x) >> (64 - PAGG_SLOT_BITS));

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

/* what a pass over the units of a layout adds up */
typedef struct Layout
{
	const Track *tr;
	int			ntr,
				nleaves;
	const Track *order[2];
	bool		units;
	uint32		unit;
	double		rows_per_unit,
				narrower,
				wider;
	double	   *term;			/* for each unit ntr + 2 numbers: its directory, its leaves and its pre-aggregates */
	uint32		ncut,
				capcut;
	StringInfoData buf;
	Unit		u;
} Layout;

/*
 * The bytes of the unit in the columns of L->u: the row directory, the integer leaves, and with units the pre-aggregates. They are added up once all the units are in
 * (layout_sum), in the order the prediction was calibrated in: all the directories, then the leaves one by one, then the pre-aggregates, each in the order of the units.
 */
static void
unit_add(Layout *L, bool keyed, uint32 rank)
{
	Unit	   *u = &L->u;
	uint32		pages;
	double	   *term;

	if (L->ncut == L->capcut)
	{
		L->capcut = Max(2 * L->capcut, 64);
		L->term = L->term ? repalloc_array(L->term, double, (size_t) L->capcut * (L->ntr + 2)) : palloc_array(double, (size_t) L->capcut * (L->ntr + 2));
	}
	term = L->term + (size_t) L->ncut++ * (L->ntr + 2);
	if (keyed)
		term[0] = sift_cunit_directory_bytes(u->tid, u->n, true, 0, &pages) + L->wider * pages / 8;
	else
		term[0] = sift_cunit_directory_bytes(u->tid, u->n, false, rank, &pages);
	for (int t = 0; t < L->ntr; t++)
	{
		double		b = encoded_bytes(u, t, &L->buf);

		if (keyed && &L->tr[t] == L->order[0] && L->tr[t].shape != NULL)
			b = Max(u->n / 8.0, b - u->n * L->narrower / 8);
		term[1 + t] = (1 + L->tr[t].image) * b;
	}
	term[1 + L->ntr] = L->units ? (keyed ? Min(1.0, (double) L->unit / L->rows_per_unit) : 1.0) * pagg_unit_bytes(L->tr, L->ntr, u, L->nleaves) : 0;
	SIFT_MEM_CHECKPOINT();
}

/* the directories and the leaves of the units added up, the number the layout without units returns */
static double
layout_sum(const Layout *L)
{
	double		bytes = 0;

	for (uint32 u = 0; u < L->ncut; u++)
		bytes += L->term[(size_t) u * (L->ntr + 2)];
	for (int t = 0; t < L->ntr; t++)
		for (uint32 u = 0; u < L->ncut; u++)
			bytes += L->term[(size_t) u * (L->ntr + 2) + 1 + t];
	return bytes;
}

/* the cut by the superblock: each sampled superblock of the trial is a unit */
static void
layout_superblocks(Layout *L, Samp *s, uint32 stride)
{
	AcutCur	cur;
	uint32		rank = 0;

	acut_cur_init(&cur, &s->rows, 0, 0);
	for (int u = 0; u < s->nsb; u += stride, rank++)
	{
		uint32		n = s->start[u + 1] - s->start[u];

		unit_reserve(&L->u, L->ntr, n);
		acut_cur_seek(&cur, s->start[u], s->start[u + 1]);
		for (uint32 i = 0; i < n; i++)
			unit_put(&L->u, L->ntr, i, acut_cur_next(&cur), ((SiftKey) rank * SIFT_SB_PAGES) << 16);
		L->u.n = n;
		unit_add(L, false, rank);
	}
	acut_cur_free(&cur);
}

/* a record of a sorted run: the keys it is sorted by and its place in the sample, then the record of the sample (its address moved to the layout) */
typedef struct RunHead
{
	int64		k0;
	int64		k1;
	uint32		row;
	uint32		flags;			/* 1: the first key is NULL */
} RunHead;

static int
run_cmp(const void *a, const void *b)
{
	const RunHead *x = a,
			   *y = b;

	if (x->k0 != y->k0)
		return x->k0 < y->k0 ? -1 : 1;
	if (x->k1 != y->k1)
		return x->k1 < y->k1 ? -1 : 1;
	return SIFT_CMP3(x->row, y->row);
}

/* the records of the runs in the order of the keys, with the window of them the cut looks ahead in */
typedef struct Feed
{
	AcutMerge	merge;
	uint32		recsz;
	const char *sb;				/* the sorted buffer the records are read from, record j at by[j].row, when the rows fit in one */
	const SortRow *by;
	char	   *win;
	uint32		head,
				tail,
				cap;
	bool		eof;
} Feed;

/* record i past the head of the window, read from the runs when the window ends before it; NULL past the last */
static const char *
feed_at(Feed *f, uint32 i)
{
	while (f->head + i >= f->tail)
	{
		const void *rec = f->eof ? NULL : acut_merge_next(&f->merge);

		if (rec == NULL)
		{
			f->eof = true;
			return NULL;
		}
		if (f->tail == f->cap && f->head > 0)
		{
			memmove(f->win, f->win + (size_t) f->head * f->recsz, (size_t) (f->tail - f->head) * f->recsz);
			f->tail -= f->head;
			f->head = 0;
		}
		if (f->tail == f->cap)
		{
			f->cap = f->cap ? f->cap * 2 : 1024;
			f->win = f->win ? repalloc_huge(f->win, (size_t) f->cap * f->recsz) : palloc_extended(f->recsz * (size_t) f->cap, MCXT_ALLOC_HUGE);
		}
		memcpy(f->win + (size_t) f->tail * f->recsz, rec, f->recsz);
		f->tail++;
	}
	if (f->sb != NULL)
		return f->sb + (size_t) f->by[f->head + i].row * f->recsz;
	return f->win + (size_t) (f->head + i) * f->recsz;
}

static bool
feed_same_key(const RunHead *a, const RunHead *b)
{
	return a->k0 == b->k0 && (a->flags & 1) == (b->flags & 1);
}

/* the rows from record `from` on that have the key of that record, up to `most` of them */
static uint32
feed_run(Feed *f, uint32 from, uint32 most)
{
	RunHead		first = *(const RunHead *) feed_at(f, from);
	uint32		n = 0;

	while (n < most)
	{
		const RunHead *x = (const RunHead *) feed_at(f, from + n);

		if (x == NULL || !feed_same_key(&first, x))
			break;
		n++;
	}
	return n;
}

/* the first n records of the window as the unit of L, in the order of their rows */
static void
feed_take(Layout *L, Feed *f, uint32 n)
{
	Unit	   *u = &L->u;

	unit_reserve(u, L->ntr, n);
	for (uint32 i = 0; i < n; i++)
		u->ord[i] = (OrdRow) {((const RunHead *) feed_at(f, i))->row, i};
	sort_ord_rows(u->ord, n);
	for (uint32 j = 0; j < n; j++)
		unit_put(u, L->ntr, j, feed_at(f, u->ord[j].at) + sizeof(RunHead), 0);
	u->n = n;
	f->head += n;
}

/* the rows of the buffer in the order of the keys, appended to the runs as the next run */
static void
write_run(AcutSpool *runs, const char *sb, SortRow *by, uint32 fill)
{
	sort_rows(by, fill);
	for (uint32 i = 0; i < fill; i++)
		acut_spool_add(runs, sb + (size_t) by[i].row * runs->recsz);
	SIFT_MEM_CHECKPOINT();
}

/* the records of superblock u (rank in the trial) appended to the buffer from fill on, with the keys they sort by */
static uint32
load_records(Layout *L, Samp *s, AcutCur *cur, int u, uint32 rank, uint32 fill, char *sb, SortRow *by)
{
	uint32		n = s->start[u + 1] - s->start[u];
	uint64		shift = ((uint64) rank * SIFT_SB_PAGES) << 16;

	acut_cur_seek(cur, s->start[u], s->start[u + 1]);
	for (uint32 i = 0; i < n; i++, fill++)
	{
		RunHead    *h = (RunHead *) (sb + (size_t) fill * (s->recsz + sizeof(RunHead)));
		char	   *rec = (char *) (h + 1);

		memcpy(rec, acut_cur_next(cur), s->recsz);
		((uint64 *) rec)[0] += shift;
		by[fill] = (SortRow) {.row = fill};
		for (int k = 0; k < 2 && L->order[k] != NULL; k++)
		{
			by[fill].nl[k] = ROW_NL(rec, s->ntr, L->order[k]->idx);
			by[fill].v[k] = ROW_V(rec, L->order[k]->idx);
		}
		*h = (RunHead) {.k0 = SORT_ROW_KEY(&by[fill]), .k1 = SORT_ROW_KEY2(&by[fill]), .row = s->start[u] + i, .flags = by[fill].nl[0]};
	}
	return fill;
}

/*
 * The cut by the key: the sampled rows of the trial are sorted by the keys (a NULL last, as PostgreSQL's default) a buffer at a time, the buffer as large as the budget allows,
 * and written as runs that are merged into the order of the whole sample (a sample that fits one buffer is read from it as it is); its units are cut as the cut of a key-cut
 * segment makes them (col/order.c cut_at): unit rows each, as the units of the table; with a second leaf (small > 0: the rows of a unit of a big key in the sample) a key of at
 * least 2 * small rows is cut into units of small rows, the last one ending with the key, and the unit before it ends where the key starts. A unit is the rows between two cuts,
 * so the window of the feed holds a unit and the rows the cut looks ahead.
 */
static void
layout_keyed(Layout *L, Samp *s, uint32 stride, double small)
{
	AcutCur		cur;
	AcutSpool	runs;
	uint64		bound[AUTOCUT_SBS + 1];
	uint32		recsz = s->recsz + sizeof(RunHead),
				most = 0,
				nsel = 0,
				nr = 0,
				fill = 0,
				cap,
				rank = 0,
				L2 = small > 0 ? (uint32) ceil(2 * small) : 0;
	int64		held = recsz + 2 * sizeof(SortRow);
	char	   *sb;
	SortRow    *by;
	Feed		f = {.recsz = recsz};
	bool		split = false;

	for (int u = 0; u < s->nsb; u += stride)
	{
		most = Max(most, s->start[u + 1] - s->start[u]);
		nsel += s->start[u + 1] - s->start[u];
	}
	/* a run holds the rows of as many superblocks as half of what the budget has left takes (a row held takes the record, its place in the sort and the sort's copy), at least one; when all the rows fit, the buffer sorted is the only run and is read as it is */
	cap = (uint32) Min((int64) nsel, Max((int64) most, Max(s->budget.left, 0) / 2 / held));
	s->budget.left -= cap * held;
	sb = palloc((size_t) cap * recsz);
	by = palloc_array(SortRow, cap);
	acut_spool_init(&runs, &s->budget, recsz);
	acut_cur_init(&cur, &s->rows, 0, 0);
	for (int u = 0; u < s->nsb; u += stride, rank++)
	{
		if (fill > 0 && fill + s->start[u + 1] - s->start[u] > cap)
		{
			bound[nr++] = runs.n;
			write_run(&runs, sb, by, fill);
			fill = 0;
		}
		fill = load_records(L, s, &cur, u, rank, fill, sb, by);
	}
	acut_cur_free(&cur);
	if (nr == 0)
	{
		sort_rows(by, fill);
		f.sb = sb;
		f.by = by;
		f.tail = fill;
		f.eof = true;
	}
	else
	{
		bound[nr++] = runs.n;
		write_run(&runs, sb, by, fill);
		bound[nr] = runs.n;
		pfree(by);
		pfree(sb);
		s->budget.left += cap * held;
		acut_merge_begin(&f.merge, &runs, bound, nr, run_cmp);
	}
	while (feed_at(&f, 0) != NULL)
	{
		uint32		rem = small > 0 ? feed_run(&f, 0, L2) : 0,
					end;
		bool		big = small > 0 && rem >= L2;

		if (big || (split && small > 0))
		{
			split = big;
			end = big ? (uint32) ceil(small) : rem;
		}
		else
		{
			split = false;
			end = L->unit;
			if (feed_at(&f, end - 1) == NULL)
				end = f.tail - f.head;
			else if (small > 0 && feed_at(&f, end) != NULL)
			{
				RunHead		at = *(const RunHead *) feed_at(&f, end);
				uint32		b = end;

				while (b > 0 && feed_same_key((const RunHead *) feed_at(&f, b - 1), &at))
					b--;
				if (b < end && b > 0 && feed_run(&f, b, L2) >= L2)
					end = b;
			}
		}
		feed_take(L, &f, end);
		unit_add(L, true, 0);
	}
	if (f.sb != NULL)
	{
		pfree(by);
		pfree(sb);
		s->budget.left += cap * held;
	}
	else
	{
		if (f.win != NULL)
			pfree(f.win);
		acut_merge_end(&f.merge);
	}
	acut_spool_free(&runs);
}

/*
 * The bytes of the integer leaves and of the row directories of the rows of at most AUTOCUT_TRIAL_SBS of the sampled superblocks (every k-th) in the units of the
 * superblock cut (key < 0) or of the cut by the leaf key, per sampled row. The integer leaves are encoded by the codec in units of as many rows as a superblock
 * holds. The row directory is encoded as enc.c writes it: a unit of the cut holds the rows of the whole heap in a range of the key and lists their addresses,
 * and the sample holds a share of the heap, so a unit is the sampled rows in the range, n / nsb of them. When that is fewer than AUTOCUT_DIR_ROWS, the unit is
 * that many rows and the gaps between its blocks, then smaller than the whole heap gives, are widened by the bits of the difference.
 * units: also the headers and the pre-aggregates of the units, which the value returned then holds; plain, when given, receives the bytes without them from the same pass.
 * The rows are read from the spool of the sample unit by unit, so the memory of a layout does not follow the rows of the sample.
 */
double
acut_layout_bytes(const Track *tr, int ntr, int nleaves, Samp *s, int key, int key2, bool units, double *plain, double nsb, double heap_pages, double rows_per_unit)
{
	Layout		L = {.tr = tr, .ntr = ntr, .nleaves = nleaves, .units = units, .rows_per_unit = rows_per_unit,
		.order = {key >= 0 ? &tr[key] : NULL, key2 >= 0 ? &tr[key2] : NULL}};
	uint32		stride = (s->nsb + AUTOCUT_TRIAL_SBS - 1) / AUTOCUT_TRIAL_SBS,
				nsel = 0,
				nused = 0;
	double		bytes;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_col key_cut layout", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);

	SIFT_MEM_SCOPE(col_key_cut_layout, cx);
	for (int u = 0; u < s->nsb; u += stride, nused++)
		nsel += s->start[u + 1] - s->start[u];
	L.narrower = Max(0, log2(nsb / nused));
	initStringInfo(&L.buf);
	if (key < 0)
		layout_superblocks(&L, s, stride);
	else
	{
		L.unit = (uint32) Max(nsel / nsb, AUTOCUT_DIR_ROWS);
		L.wider = Max(0, log2(heap_pages / rows_per_unit / ((double) nused * SIFT_SB_PAGES / L.unit)));
		layout_keyed(&L, s, stride,
					 key2 >= 0 && rows_per_unit >= 2.0 * SIFT_COLK_SECOND_ROWS ? Max(SIFT_COLK_SECOND_ROWS * (double) nsel / nsb / rows_per_unit, AUTOCUT_DIR_ROWS / 4) : 0);
	}
	bytes = layout_sum(&L);
	if (plain != NULL)
		*plain = bytes / Max(nsel, 1) + AUTOCUT_OVERHEAD_BYTES;
	if (units)
	{
		bytes += AUTOCUT_UNIT_BYTES * (key < 0 ? nused : L.ncut * Min(1.0, (double) L.unit / rows_per_unit));
		for (uint32 u = 0; u < L.ncut; u++)
			bytes += L.term[(size_t) u * (ntr + 2) + 1 + ntr];
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
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
