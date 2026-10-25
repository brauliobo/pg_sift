/*
 * col/autocut.c - the default of the option key_cut: CREATE INDEX and REINDEX of an index without key_cut (or with key_cut = 'auto')
 * cut the units by a key when a sample of the heap says that zones cannot skip the units of that key. The key is the first leaf, in the order of the index,
 * that is key-shaped (sift_col_key_shaped); it is chosen when an equality on one of its values reads at least AUTOCUT_RATIO times the units its rows fill
 * and at least AUTOCUT_MIN_UNITS, no other leaf of the index follows the order of the table (key_cut would widen its zones), and the index would not grow by
 * more than AUTOCUT_GROWTH_PCT. The size is predicted from the sample: the integer leaves of the sampled rows are encoded by the codec of the index in the
 * units of the superblock cut and in the units of the key cut, the addresses of the rows are priced by what they measured, and the other leaves are
 * priced as unchanged. When the key would be worth cutting by and the index would grow more, the build only says so, with the command. The choice is
 * written to the option as 'auto: col', so that a promotion, a merge and a REINDEX keep it; key_cut = 'off' keeps the units by superblock. An index with
 * point_tid_columns or order_columns is left alone: the first lists rows, the second says the workload sorts by a column without a key, which a key cut
 * makes slower. sift.key_cut_decision(index) runs the same rule on an existing index and says what it would decide, without changing anything.
 *
 * The sample is a fixed set of superblocks spread over the heap. For a key taken at a random row of the sample, the other sampled superblocks that hold
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
#include "col/col.h"
#include "col/setsum.h"

#define AUTOCUT_MIN_UNITS	64		/* units an equality reads, at least */
#define AUTOCUT_RATIO		8		/* times the units its rows fill, at least */
#define AUTOCUT_KEYS		32		/* keys taken from the sample, more than half of them must read that many */
#define AUTOCUT_SBS			256		/* superblocks sampled, at most */
#define AUTOCUT_MIN_SBS		32
#define AUTOCUT_QUICK_SBS	8		/* superblocks of the first look, which most tables end at: it prices the decision at a share of the heap, not at a pass over it */
#define AUTOCUT_ROWS		400000	/* rows sampled, at most */
#define AUTOCUT_TRIAL_SBS	48		/* superblocks of the sample whose integer leaves are encoded to predict the size */
#define AUTOCUT_LEAVES		8		/* leaves read: the candidates and those that may compete with them */
#define AUTOCUT_TID_BYTES	0.65	/* what the address of a row takes in a unit, at most */
#define AUTOCUT_LEAF_BYTES	1.0		/* what a row takes in a leaf that is not read from the sample */
#define AUTOCUT_DIR_BYTES	0.5		/* what the directory of a unit takes for a row */
#define AUTOCUT_KEY_BYTES	2.0		/* what a key takes in the key list of a unit */
#define AUTOCUT_GROWTH_PCT	3		/* the predicted growth of the index, at most (the 3.3 rule for a default) */

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
	bool		cand;
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
u32_cmp(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a,
				y = *(const uint32 *) b;

	return x < y ? -1 : x > y;
}

/*
 * The bytes of the integer leaves of the rows of at most AUTOCUT_TRIAL_SBS of the sampled superblocks (every k-th) in the units of the superblock cut
 * (key < 0) or of the cut by the leaf key, in units of as many rows, per sampled row.
 */
static double
layout_bytes(const Track *tr, int ntr, const Samp *s, int key)
{
	StringInfoData buf;
	int64	   *tmp = palloc_array(int64, s->n + 1);
	uint32	   *rows = palloc_array(uint32, s->n + 1);
	uint32	   *sel = palloc_array(uint32, s->n + 1);
	uint32		nsel = 0,
				stride = (s->nsb + AUTOCUT_TRIAL_SBS - 1) / AUTOCUT_TRIAL_SBS,
				unit;
	int			nused = 0;
	double		bytes = 0;

	for (int u = 0; u < s->nsb; u += stride, nused++)
		for (uint32 i = s->start[u]; i < s->start[u + 1]; i++)
			sel[nsel++] = i;
	unit = Max(1, nsel / nused);
	initStringInfo(&buf);
	if (key >= 0)
		qsort_arg(sel, nsel, sizeof(uint32), row_cmp, (void *) &tr[key]);
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
} CutVerdict;

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
				pick = -1,
				first = -1;
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	double		nsb = ceil((double) nblocks / SIFT_SB_PAGES),
				rows_per_unit,
				def_row = 0,
				kc_row = 0;
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
			ncand += tr[ntr++].cand;
		}
		else
			untracked++;
	}
	rows_per_unit = heap->rd_rel->reltuples > 0 ? heap->rd_rel->reltuples / nsb : (double) SIFT_SB_PAGES * MaxHeapTuplesPerPage / 4;
	full = (int) Min(Min(AUTOCUT_SBS, nsb), AUTOCUT_ROWS / Max(rows_per_unit, 1));
	if (ncand == 0)
	{
		v->reason = "no key-shaped column (a foreign key or *_id, not unique, int2, int4, int8 or date)";
		goto done;
	}
	if (full < AUTOCUT_MIN_SBS)
	{
		v->reason = "the units are too large for a sample";
		goto done;
	}
	want = quick ? Min(full, AUTOCUT_QUICK_SBS) : full;
	samp = palloc0(sizeof(Samp));
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
	def_row = layout_bytes(tr, ntr, samp, -1) + untracked * AUTOCUT_LEAF_BYTES + AUTOCUT_DIR_BYTES;
	for (int t = 0; t < ntr && pick < 0; t++)
	{
		double		fill = 0,
					u;
		bool		fired;

		if (!tr[t].cand)
			continue;
		/* the key list of a leaf is kept where it fits the share of the bytes of a unit (SIFT_CSET_PRES_PCT) */
		/* the first look asks a quarter of the units: a sample of a few superblocks puts one hit at 1/f units, so it cannot tell 16 from 64, but it can tell a key that sits in few */
		u = units_read(&tr[t], samp->nsb, nsb, rows_per_unit,
					   AUTOCUT_KEY_BYTES * tr[t].ncells / samp->nsb <= SIFT_CSET_PRES_PCT / 100.0 * rows_per_unit * def_row,
					   quick ? AUTOCUT_MIN_UNITS / 4 : AUTOCUT_MIN_UNITS, &fill, &fired);
		if (first < 0)
		{
			first = t;
			v->reads = u;
			v->ideal = fill;
		}
		if (fired)
		{
			pick = t;
			v->reads = u;
			v->ideal = fill;
		}
	}
	leaf = NameStr(TupleDescAttr(RelationGetDescr(index), sc->cols[tr[pick >= 0 ? pick : first].c].attno - 1)->attname);
	strlcpy(v->leaf, leaf, sizeof(v->leaf));
	if (pick < 0)
	{
		v->reason = "an equality on the key reads few units, or not many times those its rows fill";
		goto done;
	}
	if (quick)
	{
		v->maybe = true;
		goto done;
	}
	/* a leaf that follows the order of the table would lose its zones to the cut */
	for (int t = 0; t < ntr; t++)
		if (t != pick && !scattered(&tr[t], samp->nsb))
		{
			v->reason = psprintf("the leaf \"%s\" follows the order of the table, and the cut would widen its zones",
								 NameStr(TupleDescAttr(RelationGetDescr(index), sc->cols[tr[t].c].attno - 1)->attname));
			goto done;
		}
	kc_row = layout_bytes(tr, ntr, samp, pick) + untracked * AUTOCUT_LEAF_BYTES + AUTOCUT_DIR_BYTES + AUTOCUT_TID_BYTES;
	v->growth = 100.0 * (kc_row - def_row) / def_row;
	if (v->growth <= AUTOCUT_GROWTH_PCT)
	{
		v->res = SIFT_AUTOCUT_CUT;
		v->reason = "an equality on the key reads many units and the index does not grow";
	}
	else
	{
		v->res = SIFT_AUTOCUT_ADVISED;
		v->reason = "an equality on the key reads many units, but the index would grow by more than 3%";
	}
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
		appendStringInfo(&opt, "auto: %s", v.leaf);
		sift_col_set_option(index, "key_cut", opt.data);
		ereport(NOTICE,
				(errmsg("sift_col index \"%s\": key_cut = '%s' was chosen: an equality on \"%s\" would read about %.0f of %.0f units, where %.0f hold its rows (predicted index size %+.1f%%)",
						RelationGetRelationName(index), v.leaf, v.leaf, v.reads, v.units, v.ideal, v.growth),
				 errdetail("A sample of %.0f superblocks shows the values of \"%s\" spread over the units, and no other leaf of the index follows the order of the table. The units are cut by the key, so the equality reads the few units of its range; the addresses of the rows of a unit are in the index.",
						   v.sampled, v.leaf),
				 errhint("ALTER INDEX %s SET (key_cut = 'off') and REINDEX keep the units by block range.", quote_identifier(RelationGetRelationName(index)))));
	}
	else
	{
		if (v.decided && sift_col_options_key_cut(index)[0] != '\0')
			sift_col_set_option(index, "key_cut", "auto");
		if (v.res == SIFT_AUTOCUT_ADVISED)
			ereport(NOTICE,
					(errmsg("sift_col index \"%s\": an equality on \"%s\" reads about %.0f of %.0f units, where %.0f hold its rows; key_cut = '%s' would cut that, and the index would grow by %.1f%%",
							RelationGetRelationName(index), v.leaf, v.reads, v.units, v.ideal, v.leaf, v.growth),
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
	TupleDesc	tupdesc;
	Datum		vals[8];
	bool		nulls[8] = {false};

	if (!sift_col_is_index(index))
		ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE), errmsg("\"%s\" is not a sift_col index", RelationGetRelationName(index))));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");	/* check-errors: allow */
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
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, vals, nulls)));
}
