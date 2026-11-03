/*
 * col/autocut/autocut.c - the default of the option key_cut: CREATE INDEX and REINDEX of an index without key_cut (or with key_cut = 'auto')
 * cut the units by a key when a sample of the heap says that zones cannot skip the units of that key. The key is the first leaf, in the order of the index,
 * that is key-shaped (sift_col_key_shaped); when none of those is cut by, the first date or timestamp leaf, for a range of its values (ranges_read). A leaf is
 * chosen when the default formula says the cut pays: the benefit of the lookups it saves (an equality on one of its values, or a range holding the rows of one unit,
 * reads the units its rows fill and not those whose zones hold the key) is at least the cost of the growth of the index and of the build; no other leaf of the index
 * follows the order of the table (key_cut would widen its zones) unless a unique b-tree of the table on that column alone answers its lookups (the primary key). The size is predicted from the sample: the integer leaves of the sampled rows are encoded by the codec of the index in the
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
#include "access/heapam.h"
#include "col/am.h"
#include "core/sql.h"
#include "core/cmp.h"
#include "col/autocut/autocut_int.h"

#define AUTOCUT_MIN_SBS	16

#define AUTOCUT_QUICK_SBS	8		/* superblocks of the first look, which most tables end at: it prices the decision at a share of the heap, not at a pass over it */

#define AUTOCUT_ROWS		400000	/* rows sampled, at most, unless AUTOCUT_MIN_SBS superblocks hold more: the sample is a number of units, so that a table of narrow rows (25,000 to a unit) is sampled as one of wide rows */

#define AUTOCUT_OTHER_PCT	1		/* percent more units that an equality on another leaf may keep under the cut of a lane: the cut spreads its zones over all the units */

#define AUTOCUT_LEAVES		8		/* leaves read: the candidates and those that may compete with them */

#define AUTOCUT_KEY_BYTES	2.0		/* what a key takes in the key list of a unit */

#define AUTOCUT_SECOND_SLACK 5		/* percent the prediction (with the units of 4,096 rows of a key of 8,192 rows or more, 7.9 KB each in measures of 3 leaves and of 4) may exceed the cut by the key alone for the date to be its second key: about 2 units of cost; the sample does not see the runs and addresses a date order shrinks */

#define AUTOCUT_QUICK_SLACK	4		/* units of cost over the benefit that the first look still sends a time leaf to the sample: its prediction from a few superblocks is rough (the prediction for a range lane beside other leaves was 7 to 16 points of growth, 2 to 5 units of cost, over the sample's) */

/* point_tid_columns is a list the user wrote: the 'auto: col' of an earlier build (col/keycols/keycols.c) is decided again after this choice, and lists rows on a key-cut index as point_columns does */
static bool
point_tid_set(Relation index)
{
	const char *opt = sift_col_options_point_tid_columns(index);
	bool		autolist;

	(void) sift_cpagg_auto_list(opt, &autolist);
	return opt[0] != '\0' && !autolist;
}

int
acut_cell_cmp(const void *a, const void *b)
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

typedef struct CutVerdict
{
	SiftAutoCut res;
	bool		decided;		/* the options let a build decide */
	bool		maybe;			/* the first look found a key that may be worth cutting by: the full sample decides */
	const char *reason;
	char		leaf[NAMEDATALEN];
	char		second[NAMEDATALEN];	/* the date or timestamp leaf the rows of a key are sorted by, or empty */
	char		widened[NAMEDATALEN];	/* the leaf that follows the table and whose zones the cut widens, its lookups being a unique b-tree's, or empty */
	double		units;
	double		sampled;
	double		reads;
	double		ideal;
	double		growth;
	double		benefit;		/* the default formula: B and C of the cut (cut_benefit, cut_cost) */
	double		cost;
	bool		range;			/* the leaf is a time leaf: a range of its values is read, not an equality */
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
				first = -1,
				nfire = 0,
				fire[AUTOCUT_LEAVES];
	double		untracked_bytes = 0;
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	double		nsb = ceil((double) nblocks / SIFT_SB_PAGES),
				rows_per_unit,
				def_row = 0,
				kc_row = 0,
				kc_units = 0;
	double		reads[AUTOCUT_LEAVES],
				fills[AUTOCUT_LEAVES];
	bool		blocked[AUTOCUT_LEAVES] = {false},
				keys_kept;
	int			widens[AUTOCUT_LEAVES] = {0};	/* 1 + the track of the leaf a cut widens, 0 for none */
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
	else if (point_tid_set(index))
		v->reason = "point_tid_columns is set";
	else if (sift_col_options_order_columns_set(index))
		v->reason = "order_columns is set";
	else if (!ActiveSnapshotSet())
		v->reason = "no snapshot";
	if (v->reason != NULL)
		goto done;
	v->decided = true;
	keys_kept = sift_col_options_key_columns(index) == NULL || strcmp(sift_col_options_key_columns(index), "off") != 0;
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
			tr[ntr].btree = acut_unique_btree(heap, hatt);
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
			untracked_bytes -= (d->lane == 1) * AUTOCUT_LEAF_BYTES;
			ntr++;
		}
		else
		{
			untracked++;
			untracked_bytes += acut_leaf_bytes(heap, d, hatt);
		}
	}
	rows_per_unit = heap->rd_rel->reltuples > 0 ? heap->rd_rel->reltuples / nsb : acut_mid_rows(heap, nblocks, nsb);
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
		v->reason = "the sample holds fewer than 16 superblocks";
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
			k->has[u] = true;
			if (!k->cand)
			{
				/* the zone alone: no cells to count, so no sort */
				int64		lo = tmp[0],
							hi = tmp[0];

				for (int i = 1; i < n; i++)
				{
					lo = Min(lo, tmp[i]);
					hi = Max(hi, tmp[i]);
				}
				k->lo[u] = (double) lo;
				k->hi[u] = (double) hi;
				continue;
			}
			sift_sort_i64(tmp, n);
			k->lo[u] = (double) tmp[0];
			k->hi[u] = (double) tmp[n - 1];
			for (int i = 0, j; i < n; i = j)
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
	def_row = acut_layout_bytes(tr, ntr, ntr + untracked, samp, -1, -1, false, NULL, nsb, nblocks, rows_per_unit) + untracked_bytes;
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
				u = acut_units_read(&tr[t], samp->nsb, nsb, rows_per_unit,
							   AUTOCUT_KEY_BYTES * tr[t].ncells / samp->nsb <= SIFT_CSET_PRES_PCT / 100.0 * rows_per_unit * def_row, keys_kept,
							   &fill, &fired);
			else
				u = acut_ranges_read(&tr[t], samp->n, samp->nsb, nsb, rows_per_unit, &fill, &fired);
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
			if (!acut_scattered(&tr[t], samp->nsb))
			{
				/* its zones would widen (an equality 1 unit to 36, a list of 16 to all of 178 at 1,000,000 rows), but a unique b-tree answers those lookups alone */
				if (!tr[t].btree)
					what = "follows the order of the table, and the cut would widen its zones";
				else if (widens[i] == 0)
					widens[i] = t + 1;
			}
			else if (tr[fire[i]].shape && acut_zone_share(&tr[t], samp->nsb, &m) * (100 + AUTOCUT_OTHER_PCT) < 100)
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
				double		g = 100.0 * (acut_layout_bytes(tr, ntr, ntr + untracked, samp, fire[i], -1, false, NULL, nsb, nblocks, rows_per_unit) + untracked_bytes - def_row) / def_row;

				if (acut_cut_benefit(acut_cut_speedup(reads[i], fills[i], rows_per_unit), tr[fire[i]].shape ? 0.5 : 1.0) + AUTOCUT_QUICK_SLACK < acut_cut_cost(g, ntr + untracked))
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
	/* the first that fires and whose benefit is at least its cost is cut by; when every one that fires costs more than it gains, the first is advised. The exception is a key
	 * whose rows fit one unit: its key list and bloom filter let an equality read the few units that hold it, so a later candidate whose benefit exceeds its cost by more
	 * (a scattered time leaf: a window reads every unit without the cut) is cut by instead */
	for (int i = 0, small = false; i < nfire && (v->res != SIFT_AUTOCUT_CUT || small); i++)
	{
		int			pick = fire[i],
					sec;
		double		growth,
					benefit,
					cost;
		bool		pays;

		if (blocked[i])
			continue;
		/* the cut by the key alone, with and without the units, from one pass over the sample */
		kc_units = acut_layout_bytes(tr, ntr, ntr + untracked, samp, pick, -1, !tr[pick].time, &kc_row, nsb, nblocks, rows_per_unit);
		kc_row += untracked_bytes;
		/*
		 * The rows of a key are sorted by the first date or timestamp leaf, scattered over the table like every other leaf (or the cut would be blocked): a key
		 * with more rows than a unit is split by date, and a window of dates skips the units outside it. Taken when the sample predicts an index not larger by more than AUTOCUT_SECOND_SLACK, the units of 4,096 rows of a key of 8,192 rows or more counted (layout_bytes).
		 */
		sec = -1;
		if (!tr[pick].time)
		{
			/* the units of a key cut by a second leaf are counted, with their pre-aggregates, against the cut by the key alone: what the two have in common cancels */
			for (int t = 0; t < ntr && sec < 0; t++)
				if (t != pick && (time_type(tr[t].type) || (tr[t].shape != NULL && tr[t].lane == 1)))
				{
					double		delta = acut_layout_bytes(tr, ntr, ntr + untracked, samp, pick, t, true, NULL, nsb, nblocks, rows_per_unit) - kc_units;

					if (delta <= kc_row * AUTOCUT_SECOND_SLACK / 100.0)
					{
						sec = t;
						kc_row += delta;
					}
				}
		}
		growth = 100.0 * (kc_row - def_row) / def_row;
		benefit = acut_cut_benefit(acut_cut_speedup(reads[i], fills[i], rows_per_unit), tr[pick].shape ? 0.5 : 1.0);
		cost = acut_cut_cost(growth, ntr + untracked);
		pays = benefit >= cost;
		if (tr[pick].time && !pays)
		{
			if (v->res == SIFT_AUTOCUT_NONE && i == first_open)
				v->reason = "a range of the leaf reads many units, but the cost of the cut is above its benefit";
			continue;
		}
		if (v->res == SIFT_AUTOCUT_CUT && (!pays || benefit - cost <= v->benefit - v->cost))
			continue;
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
			strlcpy(v->widened, widens[i] ? cut_name(index, sc, tr[widens[i] - 1].c) : "", sizeof(v->widened));
			if (sec >= 0)
				strlcpy(v->second, cut_name(index, sc, tr[sec].c), sizeof(v->second));
			v->reason = NULL;
			v->res = pays ? SIFT_AUTOCUT_CUT : SIFT_AUTOCUT_ADVISED;
			small = !tr[pick].time && fills[i] <= 1.0 && keys_kept;
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
void
sift_col_key_cut_advice(Relation index, const char *how, const char *leaf, double reads, double units, double ideal, const char *keys, const char *detail)
{
	if (!sift_cluster_advice || !sift_col_options(index)->cluster_advice)
		return;
	ereport(NOTICE,
			(errmsg("sift_col index \"%s\": %s \"%s\" reads about %.0f of %.0f units, where %.0f hold its rows; key_cut = '%s' would cut that",
					RelationGetRelationName(index), how, leaf, reads, units, ideal, keys),
			 errdetail_internal("%s", detail),
			 errhint("If the lookups of this key matter more than the size, run CREATE INDEX .. WITH (key_cut = '%s'), or ALTER INDEX %s SET (key_cut = '%s') and REINDEX; to silence this advice, SET sift.cluster_advice = off.",
					 keys, quote_identifier(RelationGetRelationName(index)), keys)));
}

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
				(errmsg("sift_col index \"%s\": key_cut = 'auto' chooses '%s': %s \"%s\" would read about %.0f of %.0f units, where %.0f hold its rows (predicted index size %+.1f%%, benefit %.0f against cost %.0f)",
						RelationGetRelationName(index), opt.data + 6, v.range ? "a range holding the rows of one unit of" : "an equality on", v.leaf, v.reads, v.units, v.ideal, v.growth, v.benefit, v.cost),
				 errdetail("A sample of %.0f superblocks shows the values of \"%s\" spread over the units, and no other leaf of the index follows the order of the table%s. The units are cut by the leaf, so %s reads the few units of its range; the addresses of the rows of a unit are in the index.%s%s",
						   v.sampled, v.leaf,
						   v.widened[0] ? psprintf(" but \"%s\", whose zones the cut widens and whose lookups a unique b-tree of the table answers", v.widened) : "",
						   v.range ? "a range" : "an equality",
						   v.range ? " Reading all the rows of one key of another leaf takes more units." : "",
						   v.second[0] ? psprintf(" The rows of a key are sorted by \"%s\" (the index is predicted to grow by at most %d%% more than with the key alone, counting the units of 4,096 rows into which a key of 8,192 rows or more is cut), so a range of its values skips the units outside it.", v.second, AUTOCUT_SECOND_SLACK) : ""),
				 errhint("To keep the units by block range, run ALTER INDEX %s SET (key_cut = 'off') and REINDEX.", quote_identifier(RelationGetRelationName(index)))));
	}
	else
	{
		if (v.decided && sift_col_options_key_cut(index)[0] != '\0')
			sift_col_set_option(index, "key_cut", "auto");
		if (v.res == SIFT_AUTOCUT_ADVISED)
		{
			const char *keys = v.second[0] ? psprintf("%s, %s", v.leaf, v.second) : v.leaf;

			sift_col_key_cut_advice(index, v.range ? "a range holding the rows of one unit of" : "an equality on", v.leaf, v.reads, v.units, v.ideal, keys,
									psprintf("The index would grow by %.1f%%. The predicted size comes from a sample of %.0f superblocks; an automatic cut is chosen only when its benefit is at least its cost (here %.0f against %.0f: a gain of 5%% in a lookup is one unit of benefit, 3%% of growth and 5%% of build instructions one of cost). No other leaf follows the order of the table.",
											 v.growth, v.sampled, v.benefit, v.cost));
		}
	}
	return v.res;
}

/*
 * The rewrite of every segment (col/maint/merge.c) decides the key again from the data, as a build does, for an index whose key_cut the build chose or left to the default:
 * rows appended at the end of the heap turn a table in the order of its tenants into one the units no longer follow, and the rewrite is where the units are cut again.
 * Writes the option when the verdict is a key other than the one the index has, and returns whether it did; the build's NOTICE is not repeated (the caller says what changed).
 * fits: the rows of the segments can be held in maintenance_work_mem (col/order.c, they are sorted again for a new key); when they cannot, a cut is not chosen now. A cut is not undone:
 * the segments it cut are streamed in the order of the key, and a table that stops paying for one is a REINDEX's to take back.
 */
bool
sift_col_auto_cut_again(Relation heap, Relation index, bool fits)
{
	CutVerdict	v;
	const char *have = sift_col_options_key_cut(index);
	char	   *want;

	decide(heap, index, false, &v);
	if (!v.decided)
		return false;
	while (*have == ' ')
		have++;
	want = v.res == SIFT_AUTOCUT_CUT ? psprintf("%s%s%s", v.leaf, v.second[0] ? ", " : "", v.second) : pstrdup("");
	if (strcmp(have, want) == 0 || want[0] == '\0' || !fits)
		return false;
	sift_col_set_option(index, "key_cut", want[0] != '\0' ? psprintf("auto: %s", want) : "auto");
	return true;
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
