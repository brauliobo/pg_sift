/*
 * col/scan/count.c - the count of a range or an element from a segment's summaries
 * the global order, the dictionary or the point directory answer count(*) for the units that are exact; the others are read
 */
#include "postgres.h"
#include "col/seg.h"
#include "core/cmp.h"
#include "col/scan_int.h"

/*
 * The range of keys that the comparisons of an integer leaf (sift_col_count_cmp) select, all on that leaf: false when a test is of another leaf or is not a range
 */
static bool
count_keys(SiftColSource *src, SiftColSegment *seg, const SiftColPred *preds, int npreds, int64 *lo, int64 *hi, int *gcol, int64 *gval)
{
	const SiftColDef *def = &seg->schema->cols[src->pcnt - 1];
	int64		glo = PG_INT64_MIN,
				ghi = PG_INT64_MAX;

	*lo = PG_INT64_MIN;
	*hi = PG_INT64_MAX;
	*gcol = -1;
	if (npreds == 0 || def->store == SIFT_COLS_STR || sift_colw_is_wide_int(def))
		return false;
	for (int i = 0; i < npreds; i++)
	{
		SiftCTest	t;
		const SiftColDef *d = &seg->schema->cols[preds[i].col];

		/* the tests of another integer leaf are its group: bounds that meet in one value */
		if (preds[i].col != src->pcnt - 1)
		{
			if (d->store == SIFT_COLS_STR || sift_colw_is_wide_int(d) || (*gcol >= 0 && *gcol != preds[i].col))
				return false;
			make_test(src, d, NULL, &preds[i], &t);
			if (t.kind != SIFT_CT_RANGE)
				return false;
			*gcol = preds[i].col;
			glo = Max(glo, t.lo);
			ghi = Min(ghi, t.hi);
			continue;
		}
		make_test(src, def, NULL, &preds[i], &t);
		if (t.kind != SIFT_CT_RANGE)
			return false;
		*lo = Max(*lo, t.lo);
		*hi = Min(*hi, t.hi);
	}
	if (*gcol >= 0 && glo != ghi)
		return false;
	*gval = glo;
	return true;
}

/* ... and of a text leaf (sift_col_count_strs): the keys of the entries of its global order between the ranks of the two bounds */
static bool
count_str_keys(SiftColSource *src, SiftColSegment *seg, int64 *lo, int64 *hi)
{
	uint32		collid = src->pcnt_box.collid;

	/* collid 0: the test is bytewise, which is the order of a block of the C collation */
	if (collid == 0)
	{
		collid = sift_cseg_gorder_collation(seg, src->pcnt - 1);
		if (collid == 0 || !sift_collate_is_c(collid))
			return false;
	}
	return sift_cseg_gorder_strkeys(seg, src->pcnt - 1, collid, src->pcnt_box.lo, src->pcnt_box.lolen, src->pcnt_box.lo_excl, src->pcnt_box.hi, src->pcnt_box.hilen,
									src->pcnt_box.hi_excl, lo, hi);
}

/*
 * What a count is of, in the global order of leaf col: the keys [lo, hi]; or, with group, every entry of group lo of the second order in slot col; or, with gcol >= 0, the keys [lo, hi] of the rows of
 * group gval of leaf gcol (the second order of leaf col grouped by it)
 */
typedef struct CountSpan
{
	int			col;
	bool		str;
	bool		group;
	int			gcol;
	int64		gval;
	int64		lo;
	int64		hi;
} CountSpan;

static bool
span_each(SiftColSegment *seg, const CountSpan *sp, uint32 limit, void (*fn) (void *arg, SiftKey tid), void *arg)
{
	return sp->gcol >= 0 ? sift_cseg_gorder_each_group(seg, sp->col, sp->gcol, sp->gval, sp->lo, sp->hi, limit, fn, arg) :
		sift_cseg_gorder_each(seg, sp->col, sp->str, sp->group, sp->lo, sp->hi, limit, fn, arg);
}

/* Unit u is exact on its own: every page its rows are on is all-visible (the summary's page mask against the snapshot) and it has no dead row or exception */
static bool
unit_exact(SiftColSource *src, SiftColSegment *seg, uint32 u)
{
	uint64		pm[SIFT_PAGEMASK_WORDS];

	if (sift_cseg_dirty(seg, u) || !sift_cseg_unit_pagemask(seg, u, pm))
		return false;
	return col_sb_covers(src, seg, sift_cseg_unit_sb(seg, u), NULL, pm);
}

/*
 * The rows of the global order between the bounds, when some units of the segment are not exact (the pages of rows inserted since the last VACUUM, a dead row): the
 * order counts every entry of the range, so the entries whose address lies in such a unit are taken off its count and those units are read after it, with the tests of the
 * scan. The addresses of the entries are decoded a chunk at a time, so the range may be any size: it costs less than the rows of the segment. Not when more than a quarter of the units are not exact, or the segment is cut by key.
 */
typedef struct OutsideCount
{
	SiftColSegment *seg;
	const uint64 *dirty;
	uint32		n;
	uint32		sb;				/* the unit of the last address, by its superblock */
	uint32		u;
} OutsideCount;

/* An entry whose address lies in a unit that is not dirty counts */
static void
outside_entry(void *arg, SiftKey tid)
{
	OutsideCount *o = arg;
	uint32		sb = sift_block_sb(sift_key_block(tid));

	if (sb != o->sb)
	{
		o->sb = sb;
		o->u = sift_cseg_unit_of_sb(o->seg, sb);
	}
	o->n += o->u >= o->seg->nsb || !sift_col_bit_test(o->dirty, o->u);
}

static bool
count_dirty_units(SiftColSource *src, ColCursor *c, const CountSpan *sp)
{
	SiftColSegment *seg = c->seg;
	uint32		nw = SIFT_COL_NWORDS(seg->nsb),
				most = Max(2, seg->nsb / 4),
				nd = 0,
				rows = 0;
	uint64	   *dirty;
	OutsideCount o;

	if ((src->flags & COLO_TABLE) || sift_cseg_keyed(&seg->entry) || !segments_current(src))
		return false;
	dirty = palloc0(nw * sizeof(uint64));
	for (uint32 u = 0; u < seg->nsb; u++)
		if (unit_exact(src, seg, u))
			rows += seg->sbdir[u].nrows;
		else
		{
			sift_col_bit_set(dirty, u);
			if (++nd > most)
				break;
		}
	o = (OutsideCount) {seg, dirty, 0, PG_UINT32_MAX, 0};
	if (nd == 0 || nd > most || !span_each(seg, sp, PG_UINT32_MAX, outside_entry, &o))
	{
		pfree(dirty);
		return false;
	}
	c->count_ok = true;
	c->count_rows = o.n;
	c->count_unit_rows = rows;
	c->count_dirty = dirty;
	return true;
}

typedef struct EntryCount
{
	SiftColSource *src;
	int			seg;
	uint64		n;
	SiftKey    *chk;			/* the addresses on a page that is not all-visible */
	uint32		nchk;
	uint32		cap;
	bool		fail;
} EntryCount;

/* An entry of a row that is live in its unit and on an all-visible page counts; one on a page that is not is left for the heap's filter; a dead row counts for none */
static void
count_entry(void *arg, SiftKey tid)
{
	EntryCount *e = arg;
	int			st = e->fail ? -2 : sift_col_gorder_exact(e->src, e->seg, tid);

	if (st == 1)
		e->n++;
	else if (st == 0)
	{
		if (e->nchk == e->cap)
		{
			if (e->cap >= SIFT_CORDER_MAX_ROWS)
			{
				e->fail = true;
				return;
			}
			e->cap = e->cap ? Min(e->cap * 2, SIFT_CORDER_MAX_ROWS) : 256;
			e->chk = e->chk ? repalloc(e->chk, e->cap * sizeof(SiftKey)) : palloc(e->cap * sizeof(SiftKey));
		}
		e->chk[e->nchk++] = tid;
	}
	else if (st == -2)
		e->fail = true;
}

/*
 * The rows of the entries of the global order between the bounds, when most units are not exact (a dead row or a page that is not all-visible in each, after updates and deletes spread
 * over the table): reading those units costs their rows, the entries of the range cost themselves. An entry of a row that is live in its unit and on an all-visible page counts; one on a page
 * that is not is counted when the snapshot sees the row (the filter of the unit scans, a page at a time over those addresses in order); a dead row of its unit counts for none. The entries are
 * read a chunk at a time, so the memory is that of the addresses on pages that are not all-visible (up to SIFT_CORDER_MAX_ROWS of them). Not when the range holds more than limit entries or a unit cannot say.
 */
static bool
count_by_entries(SiftColSource *src, ColCursor *c, const CountSpan *sp, uint32 limit)
{
	SiftColSegment *seg = c->seg;
	EntryCount	e = {src, c - src->seg, 0, NULL, 0, 0, false};
	uint32		rows = 0;

	if ((src->flags & COLO_TABLE) || sift_cseg_keyed(&seg->entry) || !segments_current(src) ||
		!span_each(seg, sp, limit, count_entry, &e) || e.fail)
	{
		if (e.chk)
			pfree(e.chk);
		return false;
	}
	if (e.nchk > 1)
		sift_sort_u64(e.chk, e.nchk);
	for (uint32 i = 0; i < e.nchk;)
	{
		BlockNumber blk = sift_key_block(e.chk[i]);
		uint64		offmask[SIFT_OFFMASK_WORDS] = {0};

		for (; i < e.nchk && sift_key_block(e.chk[i]) == blk; i++)
			sift_col_bit_set(offmask, (e.chk[i] & 0xFFFF) - 1);
		e.n += src->visibility.filter_page(src->visibility.arg, blk, offmask);
	}
	if (e.chk)
		pfree(e.chk);
	for (uint32 u = 0; u < seg->nsb; u++)
		rows += seg->sbdir[u].nrows;
	c->count_ok = true;
	c->count_rows = e.n;
	c->count_unit_rows = rows;
	return true;
}

/*
 * What the units cost against what the entries do, in instructions, when units are not exact. An entry costs ENTRY_INSTR in its liveness test (sift_col_gorder_exact); a row of a unit read
 * costs ROW_INSTR; a unit with the pre-aggregates net of its dead rows (col/seg/net.c) answers a count by a leaf with a rollup at UNIT_INSTR, whatever its rows (measured at 1,000,000 and
 * 3,000,000 rows in 40 to 140 units).
 */
#define ENTRY_INSTR		265
#define ROW_INSTR		33
#define UNIT_INSTR		60000

/* The instructions the units that are not exact cost to answer the count, without the entries */
static uint64
units_cost(SiftColSource *src, SiftColSegment *seg)
{
	uint64		cost = 0;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift col count units", ALLOCSET_SMALL_SIZES);

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftKey    *dead = NULL;
		uint32		nd = 0,
					room = 0;

		if (unit_exact(src, seg, u))
			continue;
		cost += sift_cseg_unit_dead(seg, u, cx, &dead, &nd, &room) && sift_cseg_net_has(seg, u, nd) ? UNIT_INSTR : (uint64) seg->sbdir[u].nrows * ROW_INSTR;
		MemoryContextReset(cx);
	}
	MemoryContextDelete(cx);
	return cost;
}

/*
 * The count of a range when the segment has units that are not exact: the entries of the range when they cost less than those units (units_cost: an entry costs eight rows of a unit, a unit with its pre-aggregates net of dead rows a fixed price), and for a text key whatever their number (a text test of a row costs thousands of instructions: the strings of the unit are tested one by one),
 * else the units are read after the exact ones are counted (count_dirty_units), else the entries.
 */
static bool
count_inexact(SiftColSource *src, ColCursor *c, const CountSpan *sp)
{
	SiftColSegment *seg = c->seg;
	uint64		alt;
	uint32		m;

	if ((src->flags & COLO_TABLE) || sift_cseg_keyed(&seg->entry) || !segments_current(src))
		return false;
	alt = units_cost(src, seg);
	m = sp->gcol >= 0 ? sift_cseg_gorder_count_group(seg, sp->col, sp->gcol, sp->gval, sp->lo, sp->hi) : sift_cseg_gorder_count(seg, sp->col, sp->str, sp->group, sp->lo, sp->hi);
	return (alt / ENTRY_INSTR >= 256 && count_by_entries(src, c, sp, (uint32) Min(alt / ENTRY_INSTR, PG_UINT32_MAX))) || (sp->str && count_by_entries(src, c, sp, PG_UINT32_MAX)) ||
		count_dirty_units(src, c, sp) || (!sp->str && m != UINT32_MAX && (uint64) m * ENTRY_INSTR <= alt && count_by_entries(src, c, sp, PG_UINT32_MAX));
}

/*
 * A segment whose dictionary of the leaf of sift_col_count_range holds the rows of each string (option count_columns), whose global order holds the keys or the ranks of the
 * strings of the leaf of sift_col_count_cmp or sift_col_count_strs (order_array_columns: the rows of the range are the entries between its bounds), or whose point directory lists the rows of the value
 * of sift_col_count_elem, answers the count for all its units
 * at once when every one is exact on its own, as col_whole_take wants of a segment: every page all-visible, no dead row or exception, the directory current.
 * The checks come in the order of col_super_take (all-visible bits, directory, liveness). Then col_super_take hands the segment over as one unit.
 */
bool
col_count_segment(SiftColSource *src, ColCursor *c, const SiftColPred *preds, int npreds)
{
	SiftColSegment *seg = c->seg;
	uint32		fan = sift_cseg_super_fan(seg),
				rows = 0,
				entries,
				values;
	uint64		n;
	int64		gval = 0;
	bool		order = src->pcnt_int;
	CountSpan	sp = {src->pcnt - 1, src->pcnt_str, false, -1, 0, 0, 0};

	if (fan == 0 || seg->nsb == 0)
		return false;
	if (src->pcnt_elem)
	{
		if (!col_count_elem(src, seg, preds, npreds, &n))
			return false;
	}
	else if (src->pcnt_int)
	{
		if (!(src->pcnt_str ? count_str_keys(src, seg, &sp.lo, &sp.hi) : count_keys(src, seg, preds, npreds, &sp.lo, &sp.hi, &sp.gcol, &gval)))
			return false;
		sp.gval = gval;
			if (sp.gcol >= 0)
		{
			/* the rows of one group between two keys: the second order of the leaf, by that group */
			if (!sift_cseg_gorder_grouped_by(seg, sp.col, sp.gcol))
				return false;
		}
		else if (!sift_cseg_gorder(seg, sp.col, &entries, &values) || sift_cseg_gorder_group(seg, sp.col) >= 0)
		{
			/* the leaf has no array of its own but is the group of the second order of another: an equality counts the entries of its group */
			sp.col = src->pcnt_str || sp.lo != sp.hi ? -1 : sift_cseg_gorder_group_slot(seg, sp.col);
			if (sp.col < 0)
				return false;
			sp.group = true;
		}
	}
	else if (!sift_cseg_dict_counted(seg, src->pcnt - 1))
	{
		/* no row counts in the dictionary: a prefix is counted by the ranks of the order of the leaf, as the strings between two bounds are */
		if (!src->pcnt_alt || !sift_cseg_gorder(seg, src->pcnt - 1, &entries, &values) || sift_cseg_gorder_group(seg, src->pcnt - 1) >= 0 || !count_str_keys(src, seg, &sp.lo, &sp.hi))
			return false;
		order = sp.str = true;
	}
	for (uint32 u = 0; u < seg->nsb; u += fan)
		if (!units_allvisible(src, seg, u, Min(fan, seg->nsb - u)))
			return order && count_inexact(src, c, &sp);
	if (!(src->flags & COLO_TABLE) && !segments_current(src))
		return false;
	SIFT_COL_JITTER();
	if (!units_clean(seg, 0, seg->nsb, &rows))
		return order && count_inexact(src, c, &sp);
	if (order)
	{
		uint32		m = sp.gcol >= 0 ? sift_cseg_gorder_count_group(seg, sp.col, sp.gcol, sp.gval, sp.lo, sp.hi) : sift_cseg_gorder_count(seg, sp.col, sp.str, sp.group, sp.lo, sp.hi);

		if (m == UINT32_MAX)
			return false;
		n = m;
	}
	else if (!src->pcnt_elem && !sift_cseg_dict_count(seg, src->pcnt - 1, src->pcnt_op, &src->pcnt_val, &n))
		return false;
	c->count_ok = true;
	c->count_rows = n;
	c->count_unit_rows = rows;
	return true;
}
