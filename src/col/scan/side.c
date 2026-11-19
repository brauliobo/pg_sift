/*
 * col/scan/side.c - the runs of the scope sidecar as superunits of a scan (col/side.h, docs/COL-SCOPE.md "Scope sidecar")
 *
 * A read of one scope key lists, for each segment that has a sidecar, the runs of the key the caller says may be read. At the first unit of such a run that the cursor reaches (the first the scan's
 * zone tests keep) it hands over the run as one unit (nsu units, the key's rows of them) when every unit of the run that can hold a row the scan wants is kept, is clean and has its pages
 * all-visible: the other units of the run are then not read. The items of a scan end where no run of the key is cut (sift_col_super_align), so the units of a run are in one item. A scan with no
 * comparison takes the cube of all the rows of the run; a scan whose only comparison is a range on the time leaf takes, one after the other, the cube of each month that lies inside the range, when
 * the range cuts no month the run has. A run that cannot be taken, and a run the consumer splits, is read unit by unit.
 */
/* check-memory: allow-allocs the runs of one key in one segment, which the sidecar of the segment bounds */
#include "postgres.h"

#include "datatype/timestamp.h"

#include "core/cmp.h"
#include "col/pagg.h"
#include "col/preagg.h"
#include "col/scan_int.h"
#include "col/side.h"

#define RUN_PENDING		0
#define RUN_YIELDING	1		/* some month cubes of the run are handed over, the cursor stays at its first unit */
#define RUN_TAKEN		2
#define RUN_DECLINED	3

typedef struct ColSide
{
	SiftColSegment *seg;
	const SiftSide *sd;
	const SideRun *run;
	uint32		nrun;
	uint8	   *state;
	uint16	   *was;			/* the units of a run that were kept before the run took the others' place, by index into its units */
	int			time;			/* the time leaf of the schema */
	bool		date;			/* ... its keys are days (else microseconds) */
	uint32	   *pick;			/* the cubes of the run in hand to hand over, by index into its cells */
	uint32		npick;
	uint32		next;
	const SideCell *cells;		/* the cells of run cells_run, in the scan's context */
	uint32		cells_run;
	uint64		take;			/* the units handed over so far: what a decoded cube below is valid for */
	struct
	{
		const SideCell *cell;
		uint64		take;
		SiftCHist	h;
	}			hist[2];		/* the cube of the cell in hand, and that of all the rows of its run */
} ColSide;

void
sift_col_side_read(SiftColSource *src, int scope, int64 key, SiftColRunValid valid, void *arg)
{
	src->side = (ColSideRead) {.on = true,.scope = scope,.key = key,.valid = valid,.arg = arg};
}

void
sift_col_side_strict(SiftColSource *src, SiftColSegCovers covers)
{
	src->side.covers = covers;
}

/* The runs of the key hold every unit of the segment with a member the read lists: the other units hold none, whatever their rows */
static void
run_units_only(ColCursor *c, const ColSide *s)
{
	uint64	   *only = palloc0(SIFT_COL_NWORDS(c->seg->nsb) * sizeof(uint64));
	uint32		units[SIDE_RUN_UNITS];

	for (uint32 i = 0; i < s->nrun; i++)
		for (uint32 j = 0, n = sift_side_run_units(c->seg, s->sd, &s->run[i], units); j < n; j++)
			only[units[j] / 64] |= UINT64CONST(1) << (units[j] % 64);
	for (uint32 w = 0; w < SIFT_COL_NWORDS(c->seg->nsb); w++)
		c->keep[w] &= only[w];
	pfree(only);
}

/* The tests of the scan that a run is not tried against: those of the scope's members (the run is the key's rows), and a range of the time leaf */
static const SiftColSuperTest *
time_test(const SiftColSource *src)
{
	for (int k = 0; k < src->nsuptest; k++)
		if (!src->suptest[k].scope)
			return &src->suptest[k];
	return NULL;
}

static bool
side_tests_ok(const SiftColSource *src, int time)
{
	int			others = 0;

	for (int k = 0; k < src->nsuptest; k++)
		others += !src->suptest[k].scope;
	return others == 0 || (others == 1 && time_test(src)->leaf == time && time_test(src)->nvals == 0);
}

void
scan_side_begin(SiftColSource *src, ColCursor *c)
{
	SiftColSegment *seg = c->seg;
	const SiftSide *sd;
	const SideScope *sc;
	ColSide    *s;
	uint32		nrun;
	int			time = sift_scope_time_leaf(seg->schema->cols, seg->schema->ncols);

	c->side = NULL;
	if (!src->side.on || src->supdim < 0 || src->suprows || src->supsets || (src->flags & COLO_TABLE) || !side_tests_ok(src, time) ||
		(sd = sift_cseg_side(seg)) == NULL || (sc = sift_side_scope(sd, src->side.scope)) == NULL)
		return;
	s = MemoryContextAlloc(src->scan, sizeof(ColSide));
	s->run = sift_side_runs(seg, sd, sc, src->side.key, &nrun);
	if (nrun == 0)
		return;
	s->seg = seg;
	s->sd = sd;
	s->nrun = nrun;
	s->time = time;
	s->date = time >= 0 && seg->schema->cols[time].type == SIFT_COLT_DATE;
	s->cells_run = PG_UINT32_MAX;
	s->take = 1;
	s->hist[0].cell = s->hist[1].cell = NULL;
	s->state = MemoryContextAlloc(src->scan, nrun);
	s->was = MemoryContextAllocZero(src->scan, nrun * sizeof(uint16));
	for (uint32 i = 0; i < nrun; i++)
		s->state[i] = src->side.valid(src->side.arg, seg, &s->run[i]) ? RUN_PENDING : RUN_DECLINED;
	c->keep = memcpy(MemoryContextAlloc(src->scan, SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64)), c->keep, SIFT_COL_NWORDS(seg->nsb) * sizeof(uint64));
	c->side = s;
	if (src->side.covers != NULL && src->side.covers(src->side.arg, seg))
		run_units_only(c, s);
}

/*
 * The run the unit u at the cursor belongs to, when it is still to be taken or in hand (NSB: none), with its units. The cursor reaches the first unit of the run that the zone tests keep before
 * any other, and the run is taken there, whether or not they have taken out the units before it.
 */
static uint32
run_of(const ColSide *s, uint32 u, uint32 *units, uint32 *n)
{
	uint32		lo = 0,
				hi = s->nrun;

	SIFT_BISECT(uint32, m, lo, hi, s->run[m].first <= u);
	if (lo == 0 || s->state[lo - 1] > RUN_YIELDING)
		return s->nrun;
	*n = sift_side_run_units(s->seg, s->sd, &s->run[lo - 1], units);
	for (uint32 j = 0; j < *n && units[j] <= u; j++)
		if (units[j] == u)
			return lo - 1;
	return s->nrun;
}

/* The end of the units a boundary at unit u of the segment cuts the run of the scan's key out of: the item that ends at u would take the run apart */
uint32
scan_side_align(SiftColSource *src, SiftColSegment *seg, uint32 u)
{
	const SiftSide *sd;
	const SideScope *sc;
	SideRun    *runs;
	uint32		n,
				lo = 0,
				k,
				units[SIDE_RUN_UNITS],
				end = u;

	if (!src->side.on || u == 0 || u >= seg->nsb || (sd = sift_cseg_side(seg)) == NULL || (sc = sift_side_scope(sd, src->side.scope)) == NULL)
		return u;
	runs = (SideRun *) sift_side_runs(seg, sd, sc, src->side.key, &n);
	if (runs == NULL)
		return u;
	SIFT_BISECT(uint32, m, lo, n, runs[m].first < u);
	if (lo > 0)
	{
		k = sift_side_run_units(seg, sd, &runs[lo - 1], units);
		end = Max(end, units[k - 1] + 1);
	}
	pfree(runs);
	return end;
}

/* The unit has no row in the range of the scan's time test, so no cube of the run in the range has a row of it */
static bool
unit_outside(const SiftColSource *src, SiftColSegment *seg, int time, uint32 u)
{
	SiftCZone	z;
	const SiftColSuperTest *tt = time_test(src);

	if (tt == NULL)
		return false;
	sift_cseg_zone(seg, time, u, &z);
	return z.nvals > z.nnull && (z.max < tt->lo || z.min > tt->hi);
}

/* Every wanted unit of the run is all-visible (allvisible) or clean, checked by spans of adjacent units: one summary read for a span, not one for each unit */
static bool
spans_ok(SiftColSource *src, SiftColSegment *seg, const uint32 *units, const bool *in, uint32 n, bool allvisible)
{
	uint32		rows = 0;

	for (uint32 i = 0, j; i < n; i = j)
	{
		for (j = i + 1; j < n && in[j] == in[i] && units[j] == units[j - 1] + 1; j++)
			;
		if (in[i] && !(allvisible ? units_allvisible(src, seg, units[i], j - i) : units_clean(seg, units[i], j - i, &rows)))
			return false;
	}
	return true;
}

/* every unit of the run that can hold a row the scan wants is one it reads whole and the run's summary answers for: kept, at or after the cursor (an earlier one is read already), clean, all-visible */
static bool
run_readable(SiftColSource *src, ColCursor *c, const ColSide *s, const uint32 *units, uint32 n)
{
	SiftColSegment *seg = c->seg;
	bool		in[SIDE_RUN_UNITS];

	if (units[n - 1] >= c->end)
		return false;
	for (uint32 i = 0; i < n; i++)
	{
		in[i] = !unit_outside(src, seg, s->time, units[i]);
		if (in[i] && (units[i] < c->cur || !sift_col_bit_test(c->keep, units[i])))
			return false;
	}
	if (!spans_ok(src, seg, units, in, n, true) || !segments_current(src))
		return false;
	SIFT_COL_JITTER();
	return spans_ok(src, seg, units, in, n, false);
}

/* The microseconds or days a month cell covers, in the keys of the time leaf */
static void
cell_span(const ColSide *s, int64 cell, int64 *lo, int64 *hi)
{
	int64		next = sift_cpagg_cal_next(SIFT_CAL_MONTH, cell);

	*lo = s->date ? cell : cell * USECS_PER_DAY;
	*hi = (s->date ? next : next * USECS_PER_DAY) - 1;
}

/*
 * The cubes of the run to hand over: that of all its rows when the scan has no comparison, else those of the months inside the range of the time leaf, each of which has the histogram the
 * consumer asks for (a month the range cuts, or lacks the dim, leaves the run to be read from its units: a month handed over could not be undone). The count of them, 0: none.
 */
static const SideCell *
run_cells(SiftColSource *src, ColSide *s, uint32 i)
{
	if (s->cells_run != i)
	{
		MemoryContext old = MemoryContextSwitchTo(src->scan);

		s->cells = sift_side_run_cells(s->seg, s->sd, &s->run[i]);
		MemoryContextSwitchTo(old);
		s->cells_run = i;
	}
	return s->cells;
}

static uint32
run_pick(SiftColSource *src, ColSide *s, uint32 i)
{
	const SideRun *r = &s->run[i];
	const SideCell *cells = run_cells(src, s, i);
	uint32		n = 0;
	const SiftColSuperTest *tt = time_test(src);

	s->pick = MemoryContextAlloc(src->scan, r->ncells * sizeof(uint32));
	if (tt == NULL)
	{
		s->pick[0] = 0;
		return 1;
	}
	for (uint32 q = 1; q < r->ncells; q++)
	{
		int64		lo,
					hi;
		SiftCHistDim h;

		if (cells[q].cell == SIDE_CELL_NONE)
			return 0;
		cell_span(s, cells[q].cell, &lo, &hi);
		if (hi < tt->lo || lo > tt->hi)
			continue;
		if (lo < tt->lo || hi > tt->hi ||
			sift_cpagg_decode_dim(s->seg->schema, sift_side_cell_cube(s->seg, s->sd, &cells[q]), cells[q].len, src->scan, src->supdim, NULL, &h) <= 0)
			return 0;
		s->pick[n++] = q;
	}
	return n;
}

bool
col_side_take(SiftColSource *src, ColCursor *c, ColUnit *cu)
{
	ColSide    *s = c->side;
	SiftColSegment *seg = c->seg;
	uint32		units[SIDE_RUN_UNITS],
				n,
				i = run_of(s, c->cur, units, &n);
	const SideRun *r;

	if (i == s->nrun)
		return false;
	r = &s->run[i];
	if (s->state[i] == RUN_PENDING)
	{
		if (c->prow != NULL || c->ptidv != NULL || !run_readable(src, c, s, units, n))
		{
			s->state[i] = RUN_DECLINED;
			return false;
		}
		if ((s->npick = run_pick(src, s, i)) == 0)
		{
			s->state[i] = RUN_DECLINED;
			return false;
		}
		s->next = 0;
		for (uint32 j = 0; j < n; j++)
			if (units[j] != c->cur)
			{
				if (sift_col_bit_test(c->keep, units[j]))
					s->was[i] |= 1 << j;
				c->keep[units[j] / 64] &= ~(UINT64CONST(1) << (units[j] % 64));
			}
	}
	cu->seg = seg;
	cu->u = c->cur;
	cu->nsu = n;
	cu->sb = seg->sbdir[c->cur].id;
	cu->srun = r;
	cu->scell = &run_cells(src, s, i)[s->pick[s->next++]];
	s->take++;
	cu->v.nrows = cu->scell->rows;
	cu->clean = cu->clean_ready = cu->av_ready = true;
	memset(cu->av, 0xFF, sizeof(cu->av));
	s->state[i] = s->next == s->npick ? RUN_TAKEN : RUN_YIELDING;
	if (s->state[i] == RUN_TAKEN)
		c->cur++;
	return true;
}

/* The consumer could not answer from the cube of the run: its units are read one by one, from the first */
void
scan_side_split(SiftColSource *src, const ColUnit *cu)
{
	for (int k = 0; k < src->nseg; k++)
	{
		ColCursor  *c = &src->seg[k];
		ColSide    *s = c->side;
		uint32		units[SIDE_RUN_UNITS],
					n,
					i;

		if (c->seg != cu->seg || s == NULL)
			continue;
		SIFT_INVARIANT(s->next == 1, "a scope sidecar run was split after a cube of it answered");
		n = sift_side_run_units(cu->seg, s->sd, cu->srun, units);
		i = cu->srun - s->run;
		for (uint32 j = 0; j < n; j++)
			if (s->was[i] & (1 << j))
				c->keep[units[j] / 64] |= UINT64CONST(1) << (units[j] % 64);
		s->state[i] = RUN_DECLINED;
		c->cur = cu->u;
		c->full = false;
		COL_HEAD_RESET(src);
	}
	src->cnt.units -= cu->nsu;
}

bool
sift_col_unit_run(const SiftColUnit *unit, SiftColSegment **seg, const SideRun **run, const SideCell **cell)
{
	ColUnit    *cu = unit->priv;

	*seg = cu->seg;
	*run = cu->srun;
	*cell = cu->scell;
	return cu->srun != NULL;
}

/*
 * What the cube of a cell says of an integer leaf without reading a row: as a dim, its groups (rows, the sum of key times rows, the smallest and largest key); as the measure of a dim, the
 * non-NULL count and the sum of its groups and, when the cube keeps them, their minimum and maximum. A cell with no minimum and maximum (a month) has those of all the rows of the run as bounds.
 */
static bool
cell_leaf_agg(SiftColSource *src, ColSide *s, uint32 ri, const SideCell *cell, int col, SiftCAgg *a)
{
	const SiftColSchema *schema = s->seg->schema;
	SiftCAgg	all;
	int			slot = cell->cell == SIDE_CELL_ALL;
	SiftCHist  *hist = &s->hist[slot].h;

	if (schema->cols[col].store != SIFT_COLS_INT || cell->len == 0)
		return false;
	if (s->hist[slot].cell != cell || s->hist[slot].take != s->take)
	{
		if (!sift_cpagg_decode(schema, sift_side_cell_cube(s->seg, s->sd, cell), cell->len, src->arena, hist))
			return false;
		s->hist[slot].cell = cell;
		s->hist[slot].take = s->take;
	}
	for (int k = 0; k < hist->ndims; k++)
	{
		const SiftCHistDim *d = &hist->dim[k];
		uint64		rows = 0,
					nn = 0;
		SiftCI128	sum = 0;
		int64		mn = PG_INT64_MAX,
					mx = PG_INT64_MIN;
		bool		exact = true;

		if (d->leaf == col)
		{
			for (uint32 g = 0; g < d->ngroups; g++)
			{
				rows += d->count[g];
				if (d->has_null && g == d->ngroups - 1)
					continue;
				nn += d->count[g];
				sum += (SiftCI128) d->key[g] * d->count[g];
				mn = Min(mn, d->key[g]);
				mx = Max(mx, d->key[g]);
			}
			*a = (SiftCAgg) {.nvals = rows,.nnull = rows - nn,.min = mn,.max = mx,.sum = sum,.has_sums = true,.flags = SIFT_CAGG_SUMSQ_OVF};
			return true;
		}
		for (int m = 0; m < d->nmeas; m++)
		{
			if (d->meas[m] != col)
				continue;
			if (d->sum[m] == NULL)
				return false;
			exact = d->minmax && d->min[m] != NULL;
			if (!exact && slot)
				break;
			for (uint32 g = 0; g < d->ngroups; g++)
			{
				rows += d->count[g];
				nn += d->nn[m][g];
				sum += d->sum[m][g];
				if (exact && d->nn[m][g] > 0)
				{
					mn = Min(mn, d->min[m][g]);
					mx = Max(mx, d->max[m][g]);
				}
			}
			*a = (SiftCAgg) {.nvals = rows,.nnull = rows - nn,.min = mn,.max = mx,.sum = sum,.has_sums = true,.flags = SIFT_CAGG_SUMSQ_OVF};
			if (exact)
				return true;
			if (!cell_leaf_agg(src, s, ri, &run_cells(src, s, ri)[0], col, &all))
				return false;
			a->min = all.min;
			a->max = all.max;
			a->flags |= SIFT_CAGG_LOOSE;
			return true;
		}
	}
	return false;
}

/*
 * What a run knows of a leaf: the span of the month of a month cube for the time leaf, and for an integer leaf of its cube (above) the counts and the sum, with the minimum and maximum of
 * all the rows (of the month: bounds only, SIFT_CAGG_LOOSE). Not the sum of squares (SIFT_CAGG_SUMSQ_OVF): the consumers that take the sum do not use it.
 */
bool
sift_col_run_agg(const SiftColUnit *unit, int col, SiftCAgg *a)
{
	ColUnit    *cu = unit->priv;
	int64		lo,
				hi;

	if (cu->scell == NULL || cu->scell->cell == SIDE_CELL_NONE)
		return false;
	for (int k = 0; k < cu->src->nseg; k++)
	{
		ColSide    *s = cu->src->seg[k].side;

		if (cu->src->seg[k].seg != cu->seg || s == NULL)
			continue;
		if (s->time == col && cu->scell->cell > SIDE_CELL_NONE)
		{
			cell_span(s, cu->scell->cell, &lo, &hi);
			*a = (SiftCAgg) {.nvals = cu->scell->rows,.min = lo,.max = hi};
			return true;
		}
		return cell_leaf_agg(cu->src, s, (uint32) (cu->srun - s->run), cu->scell, col, a);
	}
	return false;
}
