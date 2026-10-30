/*
 * table/gscan.c - the group scan: its creation, its units and the iteration forward and backward
 */
#include "gscan_int.h"

static void
src_map(GroupSrc *gs, int natts, MemoryContext cxt)
{
	gs->leaf = MemoryContextAlloc(cxt, (natts + 1) * sizeof(int));
	gs->exc = -1;
	for (int a = 0; a <= natts; a++)
		gs->leaf[a] = -1;
	for (int i = 0; i < gs->schema->ncols; i++)
	{
		int			attno = gs->schema->cols[i].attno;

		if (attno == 0)
			gs->exc = i;
		else if (attno <= natts && !gs->schema->cols[i].path[0])
			gs->leaf[attno] = i;
	}
}

static void
vis_sb_mask(void *arg, uint32 sb, uint64 *pagemask)
{
	VisArg	   *v = arg;
	bool		plain = !sift_tbl_tomb_any(v->st, sb * SB_KEYS, (sb + 1) * SB_KEYS - 1) &&
		sift_tbl_vers_sb(v->rel, v->snapshot, v->vers, sb) == SIFT_TBL_VER_ALL;

	memset(pagemask, plain ? 0xFF : 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

static int
vis_filter_page(void *arg, BlockNumber blk, uint64 *offmask)
{
	VisArg	   *v = arg;

	return sift_tbl_filter_page_vers(v->rel, v->snapshot, v->vers, blk, offmask, false);
}

SiftTblGroupScan *
sift_tbl_gscan_begin(Relation rel, TupleDesc desc, SiftTblRel *st, const SiftDirSnapshot *dir, Snapshot snapshot)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_table group scan", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SiftTblGroupScan *g = palloc0(sizeof(SiftTblGroupScan));
	VisArg	   *va = palloc(sizeof(VisArg));
	SiftColVisibility vis = {vis_sb_mask, vis_filter_page, NULL, va};
	const SiftColSchema **schemas = palloc0(Max(dir->nsegs, 1) * sizeof(SiftColSchema *));
	int			nschemas = 0;
	uint32		sbs = SIFT_TBL_NEXT_VBLOCK(&dir->meta) / SIFT_SB_PAGES + 1;

	g->vers = sift_tbl_vers_build(rel, st, dir, cxt);
	*va = (VisArg) {rel, st, snapshot, g->vers};
	g->snapshot = snapshot;
	g->rel = rel;
	g->st = st;
	g->desc = desc;
	g->cxt = cxt;
	g->rowcxt = AllocSetContextCreate(cxt, "sift_table group row", ALLOCSET_SMALL_SIZES);
	g->unitcxt = AllocSetContextCreate(cxt, "sift_table group unit", ALLOCSET_DEFAULT_SIZES);
	g->values = palloc(g->desc->natts * sizeof(Datum));
	g->isnull = palloc(g->desc->natts * sizeof(bool));
	g->vec = palloc(SIFT_COL_MAX_COLS * sizeof(SiftColVec));
	g->cache_sb = PG_UINT32_MAX;
	g->rd_unit = -1;
	g->uidx = -1;
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftColSchema *schema;
		int			k;

		if (dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		schema = schema_of(rel, st, &dir->segs[i]);
		for (k = 0; k < nschemas && schemas[k] != schema; k++)
			;
		if (k == nschemas)
			schemas[nschemas++] = schema;
	}
	g->s = palloc0(Max(nschemas, 1) * sizeof(GroupSrc));
	for (int k = 0; k < nschemas; k++)
	{
		g->s[k].schema = schemas[k];
		g->s[k].src = sift_col_open_table(rel, schemas[k], dir, &vis, sbs, cxt);
		src_map(&g->s[k], g->desc->natts, cxt);
	}
	g->nsrc = nschemas;
	g->sb_hi = PG_UINT32_MAX;
	MemoryContextSwitchTo(old);
	return g;
}

/* Unpins the pages the segment readers hold (the source stays usable: a table source's close releases buffers only) */
void
gscan_park(SiftTblGroupScan *g)
{
	for (int k = 0; k < g->nsrc; k++)
		sift_col_close(g->s[k].src);
}

void
sift_tbl_gscan_end(SiftTblGroupScan *g)
{
	gscan_park(g);
	g->ended = true;
	if (g->refs == 0)
		MemoryContextDelete(g->cxt);
}

void
sift_tbl_gscan_ref(SiftTblGroupScan *g)
{
	g->refs++;
}

void
sift_tbl_gscan_unref(SiftTblGroupScan *g)
{
	if (--g->refs == 0 && g->ended)
		MemoryContextDelete(g->cxt);
}

void
sift_tbl_gscan_range(SiftTblGroupScan *g, uint32 sb_lo, uint32 sb_hi)
{
	g->sb_lo = sb_lo;
	g->sb_hi = sb_hi;
	g->cur = 0;
	g->begun = false;
	g->in_unit = false;
	g->rd_unit = -1;
	if (g->units)
		pfree(g->units);
	g->units = NULL;
	g->uidx = -1;
}

static void
unit_start(SiftTblGroupScan *g)
{
	uint64		lo = (uint64) g->unit.sb * SB_KEYS;
	SiftTblVerState state = SIFT_TBL_VER_ALL;
	MemoryContext old;

	g->rd_unit = -1;
	g->token++;
	MemoryContextReset(g->unitcxt);
	g->xm = g->cm = NULL;
	memset(&g->usum, 0, sizeof(g->usum));
	if (sift_col_unit_ref(&g->unit, &g->useg, &g->uu))
	{
		sift_cseg_ver_sum(g->useg, g->uu, &g->usum);
		state = sift_tbl_ver_state(g->rel, g->snapshot, &g->usum);
	}
	g->hidden = state == SIFT_TBL_VER_NONE;
	for (int c = 0; c < g->s[g->cur].schema->ncols; c++)
		g->vec[c].n = PG_UINT32_MAX;	/* not read yet */
	g->i = 0;
	g->in_unit = true;
	if (g->hidden)
	{
		g->n = 0;
		g->plain = false;
		return;
	}
	old = MemoryContextSwitchTo(g->unitcxt);
	sift_col_sel_all(&g->unit, &g->sel);
	g->n = g->sel.count;
	g->tids = palloc(Max(g->n, 1) * sizeof(ItemPointerData));
	sift_col_tids(&g->unit, &g->sel, g->tids);
	g->ord = palloc((g->n + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_sel_idx(&g->sel, g->ord);
	MemoryContextSwitchTo(old);
	g->plain = !sift_tbl_tomb_any(g->st, lo, lo + SB_KEYS - 1) && state == SIFT_TBL_VER_ALL;
}

/* The version of the row i of the selection: a unit with unfrozen rows that a snapshot does not see whole reads its vectors on the first ask */
static void
gscan_rowver(SiftTblGroupScan *g, uint32 i, SiftTblRowVer *rv)
{
	*rv = (SiftTblRowVer) {.key = sift_key_from_tid(&g->tids[i]),.wbuf = false,.xmin = InvalidTransactionId};
	if (!g->usum.any || g->plain)
		return;
	if (g->xm == NULL)
	{
		uint32		nrows = g->useg->sbdir[g->uu].nrows;
		MemoryContext old = MemoryContextSwitchTo(g->unitcxt);

		g->xm = palloc(nrows * sizeof(uint32));
		g->cm = palloc(nrows * sizeof(uint32));
		MemoryContextSwitchTo(old);
		sift_cseg_ver_unit(g->useg, g->uu, g->xm, g->cm);
		sift_cseg_close(g->useg);
	}
	rv->xmin = g->xm[g->ord[i]];
	rv->cmin = g->cm[g->ord[i]];
}

/* Makes unit k of the list the current one */
static void
unit_goto(SiftTblGroupScan *g, int64 k)
{
	const UnitRef *u = &g->units[k];

	g->cur = u->src;
	g->begun = false;
	sift_col_begin(g->s[u->src].src, NULL, 0, u->sb, u->sb + 1);
	for (uint32 j = 0; j <= u->ordinal; j++)
		if (!sift_col_next(g->s[u->src].src, &g->unit))
			ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("sift_table: the unit %u of superblock %u is gone", u->ordinal, u->sb),
				 errhint("A concurrent merge or defrag replaced the rows it reads: retry the statement.")));
	unit_start(g);
	g->uidx = k;
}

static bool
next_unit(SiftTblGroupScan *g)
{
	if (g->units)
	{
		if (g->uidx + 1 >= (int64) g->nunits)
			return false;
		unit_goto(g, g->uidx + 1);
		return true;
	}
	while (g->cur < g->nsrc)
	{
		if (!g->begun)
		{
			sift_col_begin(g->s[g->cur].src, NULL, 0, g->sb_lo, g->sb_hi);
			g->begun = true;
		}
		if (sift_col_next(g->s[g->cur].src, &g->unit))
		{
			unit_start(g);
			g->uidx++;
			return true;
		}
		g->cur++;
		g->begun = false;
	}
	return false;
}

/* The units of the range in the order next_unit returns them, found again by superblock and place in it, whichever way the scan goes */
static void
units_list(SiftTblGroupScan *g)
{
	MemoryContext old = MemoryContextSwitchTo(g->cxt);
	int			cap = 16;

	g->units = palloc(cap * sizeof(UnitRef));
	g->nunits = 0;
	for (int k = 0; k < g->nsrc; k++)
	{
		uint32		sb = PG_UINT32_MAX,
					ordinal = 0;
		SiftColUnit unit;

		sift_col_begin(g->s[k].src, NULL, 0, g->sb_lo, g->sb_hi);
		while (sift_col_next(g->s[k].src, &unit))
		{
			ordinal = unit.sb == sb ? ordinal + 1 : 0;
			sb = unit.sb;
			if (g->nunits == cap)
				g->units = repalloc(g->units, (cap *= 2) * sizeof(UnitRef));
			g->units[g->nunits++] = (UnitRef) {k, sb, ordinal};
		}
	}
	MemoryContextSwitchTo(old);
	g->begun = false;
}

/* Positions the scan after its last unit, so that sift_tbl_gscan_prev starts from the last row */
void
sift_tbl_gscan_seek_end(SiftTblGroupScan *g)
{
	if (g->units == NULL)
		units_list(g);
	g->uidx = g->nunits;
	g->in_unit = false;
}

/* The row before the one last returned (by sift_tbl_gscan_next too), in the scan's order; false at its start */
bool
sift_tbl_gscan_prev(SiftTblGroupScan *g, SiftTblRowVer *rv)
{
	for (;;)
	{
		if (g->in_unit && g->i > 1)
		{
			g->i--;
			gscan_rowver(g, g->i - 1, rv);
			return true;
		}
		if (g->units == NULL)
			units_list(g);
		if (g->uidx <= 0)
		{
			g->in_unit = false;
			return false;
		}
		unit_goto(g, g->uidx - 1);
		g->i = g->n + 1;
	}
}

bool
sift_tbl_gscan_next(SiftTblGroupScan *g, SiftTblRowVer *rv)
{
	while (!g->in_unit || g->i >= g->n)
		if (!next_unit(g))
		{
			g->in_unit = false;
			return false;
		}
	gscan_rowver(g, g->i, rv);
	g->i++;
	return true;
}

bool
sift_tbl_gscan_plain(const SiftTblGroupScan *g)
{
	return g->plain;
}
