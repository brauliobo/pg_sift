/*
 * col/scan/super.c - the superunits of a scan: the group of units, the whole segment and the counted segment a source hands over as one unit, the switches that ask for them, and the split of
 * one into its units (docs/COL-PREAGG.md section 10)
 */
#include "postgres.h"

#include "col/am.h"
#include "col/pagg.h"
#include "col/scan_int.h"
#include "col/seg.h"

/* Every superblock of the segment is plain for the table (tbl_sb_mask: all pages or none): a table's segment has no summary of page masks to test */
static bool
sbs_plain(SiftColSource *src, SiftColSegment *seg)
{
	uint64		av[SIFT_PAGEMASK_WORDS];

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		src->visibility.sb_mask(src->visibility.arg, sift_cseg_unit_sb(seg, u), av);
		for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
			if (av[w] != ~UINT64CONST(0))
				return false;
	}
	return true;
}

static void
take_units(ColCursor *c, ColUnit *cu, uint32 first, uint32 n, uint32 rows)
{
	SiftColSegment *seg = c->seg;

	cu->seg = seg;
	cu->u = first;
	cu->nsu = n;
	cu->sb = seg->sbdir[first].id;
	cu->v.nrows = rows;
	cu->clean = cu->clean_ready = cu->av_ready = true;
	memset(cu->av, 0xFF, sizeof(cu->av));
	c->cur = first + n;
}

/*
 * The segment as one unit, for a scan that asks for it (sift_col_super_whole) and a segment that has the key table of its leaf: every unit of it exact on its
 * own, as col_super_take wants of a group, whatever the zone tests kept of them (the table answers the comparisons on the leaf itself, and the consumer checks
 * that those are the only ones). Its rows are the units' rows.
 */
static bool
col_whole_take(SiftColSource *src, ColCursor *c, ColUnit *cu)
{
	SiftColSegment *seg = c->seg;
	uint32		fan = sift_cseg_super_fan(seg),
				rows = 0;
	const SiftCHistDim *kd = sift_cseg_ktab(seg, src->whole - 1);
	bool		table = src->flags & COLO_TABLE;

	if (!c->full || c->prow != NULL || c->ptidv != NULL || (fan == 0 && !table) || kd == NULL)
		return false;
	for (int i = 0; i < src->nwmeas; i++)
	{
		int			m = 0;

		while (m < kd->nmeas && kd->meas[m] != src->wmeas[i])
			m++;
		if (m == kd->nmeas)
			return false;
	}
	if (table)
	{
		if (!sbs_plain(src, seg))
			return false;
	}
	else
	{
		for (uint32 u = 0; u < seg->nsb; u += fan)
			if (!units_allvisible(src, seg, u, Min(fan, seg->nsb - u)))
				return false;
		if (!segments_current(src))
			return false;
	}
	SIFT_COL_JITTER();
	if (!units_clean(seg, 0, seg->nsb, &rows))
		return false;
	take_units(c, cu, 0, seg->nsb, rows);
	cu->whole = true;
	return true;
}

/*
 * The group of units at the cursor as one unit (of the groups that start there, the largest that can: a key group of a scope segment, else the groups of fan units in it). Every unit of the group must have passed the zone tests and be exact on its own: its pages that hold rows
 * all-visible (the masks of the units are in the summary, so no unit record is read) and no dead row, read in the order of unit_allvisible and
 * unit_clean: the all-visible bits first, then the directory, then the liveness.
 */
bool
col_super_take(SiftColSource *src, ColCursor *c, ColUnit *cu)
{
	SiftColSegment *seg = c->seg;
	uint32		fan,
				first,
				n,
				k,
				rows = 0;

	if (c->side != NULL && col_side_take(src, c, cu))
		return true;
	if (c->count_ok && src->pcnt)
	{
		c->count_ok = false;
		take_units(c, cu, 0, seg->nsb, c->count_unit_rows);
		cu->counted = true;
		cu->crows = c->count_rows;
		if (c->count_dirty != NULL)
			c->cur = 0;			/* the units that are not exact follow, one by one */
		return true;
	}
	if (src->whole && col_whole_take(src, c, cu))
		return true;
	if (!src->super || c->cur + SIFT_SUPER_MIN_FAN > c->end || !keep_all(c, c->cur, SIFT_SUPER_MIN_FAN))
		return false;			/* a group is of two units or more, all kept: a lone unit reads no block of the superunit block */
	fan = sift_cseg_super_fan(seg);
	if (fan == 0 || c->cur < c->nosuper_to)
		return false;
	for (uint32 skip = 0; sift_cseg_super_at(seg, c->cur, skip, &k); skip++)
	{
		sift_cseg_super_range(seg, k, &first, &n);
		if (n < 2 || c->cur + n > c->end || !keep_all(c, c->cur, n) || (src->suprows && !sift_cseg_super_has_rollup(seg, k)) || (src->supsets && !sift_cseg_super_has_dsets(seg, k, src->supsets)) || !col_super_inside(src, seg, c->cur, n) || !units_allvisible(src, seg, c->cur, n))
			continue;
		if (!(src->flags & COLO_TABLE) && !segments_current(src))
			return false;
		SIFT_COL_JITTER();
		if (!units_clean(seg, c->cur, n, &rows))
			continue;
		take_units(c, cu, c->cur, n, rows);
		return true;
	}
	return false;
}

bool
sift_col_unit_super(const SiftColUnit *unit, SiftColSegment **seg, uint32 *k)
{
	ColUnit    *cu = unit->priv;

	if (cu->nsu == 0 || cu->whole || cu->counted || cu->srun != NULL)
		return false;
	*seg = cu->seg;
	return sift_cseg_super_of(cu->seg, cu->u, cu->nsu, k);
}

bool
sift_col_unit_counted(const SiftColUnit *unit, uint64 *rows)
{
	ColUnit    *cu = unit->priv;

	*rows = cu->crows;
	return cu->counted;
}

bool
sift_col_unit_whole(const SiftColUnit *unit, SiftColSegment **seg)
{
	ColUnit    *cu = unit->priv;

	*seg = cu->seg;
	return cu->whole;
}

void
sift_col_super(SiftColSource *src, bool on)
{
	src->super = on;
	src->whole = 0;
	src->pcnt = 0;
	src->pcnt_int = src->pcnt_str = src->pcnt_elem = false;
}

void
sift_col_count_range(SiftColSource *src, int leaf, SiftColOp op, const char *s, uint32 len)
{
	src->pcnt = leaf + 1;
	src->pcnt_epoch++;
	src->pcnt_int = src->pcnt_str = src->pcnt_elem = false;
	src->pcnt_op = op;
	src->pcnt_val = (SiftColValue) {.v.s = s, .len = len};
	/* the strings that pass the test are those between two bounds in the order of the bytes (the test is bytewise: the C collation): a prefix is those from s up to, not including, the least string after them */
	src->pcnt_alt = true;
	memset(&src->pcnt_box, 0, sizeof(src->pcnt_box));
	if (op == SIFT_COLOP_EQ || op == SIFT_COLOP_GT || op == SIFT_COLOP_GE || op == SIFT_COLOP_PREFIX)
	{
		src->pcnt_box.lo = s, src->pcnt_box.lolen = len, src->pcnt_box.lo_excl = op == SIFT_COLOP_GT;
	}
	if (op == SIFT_COLOP_EQ || op == SIFT_COLOP_LT || op == SIFT_COLOP_LE)
	{
		src->pcnt_box.hi = s, src->pcnt_box.hilen = len, src->pcnt_box.hi_excl = op == SIFT_COLOP_LT;
	}
	else if (op == SIFT_COLOP_PREFIX)
	{
		src->pcnt_box.hi = sift_cgord_prefix_end(s, len, &src->pcnt_box.hilen);
		src->pcnt_box.hi_excl = true;
	}
}

void
sift_col_count_cmp(SiftColSource *src, int leaf)
{
	src->pcnt = leaf + 1;
	src->pcnt_epoch++;
	src->pcnt_int = true;
	src->pcnt_str = src->pcnt_elem = false;
}

void
sift_col_count_strs(SiftColSource *src, int leaf, uint32 collid, const char *lo, uint32 lolen, bool lo_excl, const char *hi, uint32 hilen, bool hi_excl)
{
	src->pcnt = leaf + 1;
	src->pcnt_epoch++;
	src->pcnt_int = src->pcnt_str = true;
	src->pcnt_elem = false;
	src->pcnt_box.lo = lo, src->pcnt_box.lolen = lolen, src->pcnt_box.lo_excl = lo_excl;
	src->pcnt_box.hi = hi, src->pcnt_box.hilen = hilen, src->pcnt_box.hi_excl = hi_excl;
	src->pcnt_box.collid = collid;
}

void
sift_col_count_elem(SiftColSource *src)
{
	src->pcnt = 1;
	src->pcnt_epoch++;
	src->pcnt_int = src->pcnt_str = false;
	src->pcnt_elem = true;
}

void
sift_col_super_whole(SiftColSource *src, int leaf, const int *meas, int nmeas)
{
	src->whole = leaf + 1;
	src->nwmeas = nmeas;
	memcpy(src->wmeas, meas, nmeas * sizeof(int));
}

void
sift_col_unit_split(SiftColSource *src, const SiftColUnit *unit)
{
	ColUnit    *cu = unit->priv;

	Assert(cu->nsu > 0);
	if (cu->srun != NULL)
	{
		scan_side_split(src, cu);
		return;
	}
	/* the table could not answer: the segment's own superunits and units, and no other segment is tried */
	if (cu->whole)
		src->whole = 0;
	if (cu->counted)
		src->pcnt = 0;
	for (int i = 0; i < src->nseg; i++)
		if (src->seg[i].seg == cu->seg)
		{
			src->seg[i].cur = cu->u;
			COL_HEAD_RESET(src);
			src->seg[i].full = false;
			src->seg[i].nosuper_to = cu->whole ? 0 : cu->u + cu->nsu;
		}
	src->cnt.units -= cu->nsu;
}
