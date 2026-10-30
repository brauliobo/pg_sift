/*
 * table/gscan_cache.c - the group scan's cache of a superblock, the rows of a block, and the TID fetches (locate, fetch)
 */
#include "gscan_int.h"

/* The TIDs of every unit of superblock sb (ascending, one array per unit) */
static void
cache_superblock(SiftTblGroupScan *g, uint32 sb)
{
	MemoryContext old;
	int			cap = 4;

	if (g->cache_sb == sb)
		return;
	if (g->cu)
	{
		for (int k = 0; k < g->ncu; k++)
		{
			if (g->cu[k].tid)
			{
				pfree(g->cu[k].tid);
				pfree(g->cu[k].ord);
			}
			if (g->cu[k].ix)
				pfree(g->cu[k].live);
			if (g->cu[k].xmin)
			{
				pfree(g->cu[k].xmin);
				pfree(g->cu[k].cmin);
			}
		}
		pfree(g->cu);
	}
	old = MemoryContextSwitchTo(g->cxt);
	g->cu = palloc(cap * sizeof(UnitTids));
	g->ncu = 0;
	for (int s = 0; s < g->nsrc; s++)
	{
		SiftColSource *src = g->s[s].src;
		SiftColUnit unit;
		SiftColSel sel;
		uint32		ordinal = 0;

		sift_col_begin(src, NULL, 0, sb, sb + 1);
		while (sift_col_next(src, &unit))
		{
			UnitTids   *u;
			SiftColSegment *seg;
			uint32		useg;
			SiftColVerSum sum;

			if (g->ncu == cap)
				g->cu = repalloc(g->cu, (cap *= 2) * sizeof(UnitTids));
			u = &g->cu[g->ncu++];
			sift_col_sel_all(&unit, &sel);
			*u = (UnitTids) {.src = s,.ordinal = ordinal++,.n = sel.count};
			if (g->point)
				u->ix = sift_col_unit_index(&unit);
			if (u->ix)
			{
				u->live = memcpy(palloc(sel.nwords * sizeof(uint64)), sel.bits, sel.nwords * sizeof(uint64));
			}
			else
			{
				u->tid = palloc(Max(sel.count, 1) * sizeof(ItemPointerData));
				u->ord = palloc((sel.count + SIFT_COL_IDXPAD) * sizeof(uint32));
				sift_col_tids(&unit, &sel, u->tid);
				sift_col_sel_idx(&sel, u->ord);
			}
			if (sift_col_unit_ref(&unit, &seg, &useg))
			{
				sift_cseg_ver_sum(seg, useg, &sum);
				if (sum.any)
				{
					u->xmin = palloc(seg->sbdir[useg].nrows * sizeof(uint32));
					u->cmin = palloc(seg->sbdir[useg].nrows * sizeof(uint32));
					sift_cseg_ver_unit(seg, useg, u->xmin, u->cmin);
					sift_cseg_close(seg);
				}
			}
		}
	}
	g->cache_sb = sb;
	g->serial++;
	MemoryContextSwitchTo(old);
}

/* First position of u->tid whose block is >= vblock */
static uint32
block_start(const UnitTids *u, BlockNumber vblock)
{
	uint32		lo = 0,
				hi = u->n;

	SIFT_BISECT(uint32, mid, lo, hi, ItemPointerGetBlockNumberNoCheck(&u->tid[mid]) < vblock);
	return lo;
}

/* Makes unit k of the cached superblock the current one with every row read: its rows are fetched without decoding again */
static void
unit_load(SiftTblGroupScan *g, int k, uint32 sb)
{
	const UnitTids *u = &g->cu[k];
	SiftColSource *src = g->s[u->src].src;

	if (g->rd_unit == k && g->rd_serial == g->serial)
		return;
	g->rd_unit = -1;
	g->token++;
	g->cur = u->src;
	sift_col_begin(src, NULL, 0, sb, sb + 1);
	for (uint32 j = 0; j <= u->ordinal; j++)
		if (!sift_col_next(src, &g->unit))
			ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("sift_table: the unit %u of superblock %u is gone", u->ordinal, sb),
				 errhint("A concurrent merge or defrag replaced the rows it reads: retry the statement.")));
	MemoryContextReset(g->unitcxt);
	for (int c = 0; c < g->s[g->cur].schema->ncols; c++)
		g->vec[c].n = PG_UINT32_MAX;
	g->rd_unit = k;
	g->rd_serial = g->serial;
}

int
sift_tbl_gscan_block(SiftTblGroupScan *g, BlockNumber vblock, const uint64 *offmask, HeapTuple *out, TransactionId *xmin, CommandId *cmin)
{
	uint32		sb = vblock / SIFT_SB_PAGES;
	int			nout = 0;

	cache_superblock(g, sb);
	for (int k = 0; k < g->ncu; k++)
	{
		const UnitTids *u = &g->cu[k];
		uint32		first = block_start(u, vblock),
					last = first;

		while (last < u->n && ItemPointerGetBlockNumberNoCheck(&u->tid[last]) == vblock)
			last++;
		for (uint32 j = first; j < last; j++)
			if (offmask == NULL || sift_col_bit_test(offmask, ItemPointerGetOffsetNumberNoCheck(&u->tid[j]) - 1))
			{
				unit_load(g, k, sb);
				if (xmin)
				{
					xmin[nout] = u->xmin ? u->xmin[u->ord[j]] : InvalidTransactionId;
					cmin[nout] = u->xmin ? u->cmin[u->ord[j]] : 0;
				}
				out[nout++] = row_tuple(g, u->ord[j], &u->tid[j]);
			}
	}
	return nout;
}

/*
 * Do the segments of the fetch scan stay readable? The xmin it was begun with keeps them from being recycled. A newer xmin (every
 * transaction that wrote moves it) keeps them only if the directory has not changed since: a segment leaves the directory before it
 * is recycled, and what a later change removes cannot be recycled before this xmin goes.
 */
static bool
fetch_kept(Relation rel, SiftTblRel *st, struct SiftTblGroupCache *c)
{
	if (!TransactionIdIsValid(TransactionXmin) || c->fetch_version != st->dir.version)
		return false;
	if (c->fetch_xmin != TransactionXmin)
	{
		if (c->fetch_version != sift_dir_version(rel))
			return false;
		c->fetch_xmin = TransactionXmin;
	}
	return true;
}

/* The slot of a new fetch scan: an empty one, else the least recently used; the scans are dropped, oldest first, while they hold more than SIFT_TBL_FETCH_BYTES */
static int
fetch_victim(struct SiftTblGroupCache *c)
{
	for (;;)
	{
		Size		bytes = 0;
		int			lru = -1;

		for (int i = 0; i < SIFT_TBL_FETCH_SCANS; i++)
		{
			if (c->fetch[i] == NULL)
				return i;
			bytes += MemoryContextMemAllocated(c->fetch[i]->cxt, true);
			if (lru < 0 || c->fetch_used[i] < c->fetch_used[lru])
				lru = i;
		}
		if (bytes <= SIFT_TBL_FETCH_BYTES)
			return lru;
		sift_tbl_gscan_end(c->fetch[lru]);
		c->fetch[lru] = NULL;
	}
}

/*
 * The fetch scan serving the TID fetches into superblock sb while the directory stays, and the xmin of the backend keeps the segments from
 * being recycled: the one that holds sb, else a new one
 */
static SiftTblGroupScan *
fetch_scan(Relation rel, SiftTblRel *st, uint32 sb)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	int			pick = -1;

	if (!fetch_kept(rel, st, c))
		for (int i = 0; i < SIFT_TBL_FETCH_SCANS; i++)
			if (c->fetch[i])
			{
				sift_tbl_gscan_end(c->fetch[i]);
				c->fetch[i] = NULL;
			}
	for (int i = 0; i < SIFT_TBL_FETCH_SCANS; i++)
		if (c->fetch[i] && c->fetch[i]->cache_sb == sb)
			pick = i;
	if (pick < 0)
	{
		MemoryContext old = MemoryContextSwitchTo(st->cxt);

		pick = fetch_victim(c);
		if (c->fetch[pick])
			sift_tbl_gscan_end(c->fetch[pick]);
		c->fetch[pick] = sift_tbl_gscan_begin(rel, RelationGetDescr(rel), st, &st->dir, NULL);
		c->fetch[pick]->park_fill = true;
		c->fetch[pick]->point = true;
		c->fetch_version = st->dir.version;
		c->fetch_xmin = TransactionXmin;
		MemoryContextSwitchTo(old);
	}
	c->fetch_used[pick] = ++c->fetch_clock;
	return c->fetch[pick];
}

/* Where tid is among the cached units of its superblock: unit k, the row index pos in its vectors */
static bool
gscan_find(SiftTblGroupScan *g, ItemPointer tid, int *k, uint32 *pos)
{
	BlockNumber vblock = ItemPointerGetBlockNumberNoCheck(tid);

	cache_superblock(g, vblock / SIFT_SB_PAGES);
	for (*k = 0; *k < g->ncu; (*k)++)
	{
		const UnitTids *u = &g->cu[*k];

		if (u->ix)
		{
			*pos = sift_col_unit_index_find(u->ix, vblock / SIFT_SB_PAGES, sift_key_from_tid(tid));
			if (*pos != PG_UINT32_MAX && sift_col_bit_test(u->live, *pos))
				return true;
			continue;
		}
		for (uint32 j = block_start(u, vblock); j < u->n && ItemPointerGetBlockNumberNoCheck(&u->tid[j]) == vblock; j++)
			if (ItemPointerEquals(&u->tid[j], tid))
			{
				*pos = u->ord[j];
				return true;
			}
	}
	return false;
}

bool
sift_tbl_group_locate(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblGroupScan **g, uint32 *token, uint32 *pos)
{
	int			k;

	if (!st->initialized || ItemPointerGetBlockNumberNoCheck(tid) >= SIFT_TBL_NEXT_VBLOCK(&st->dir.meta))
		return false;
	*g = fetch_scan(rel, st, ItemPointerGetBlockNumberNoCheck(tid) / SIFT_SB_PAGES);
	if (!gscan_find(*g, tid, &k, pos))
	{
		gscan_park(*g);
		return false;
	}
	unit_load(*g, k, ItemPointerGetBlockNumberNoCheck(tid) / SIFT_SB_PAGES);
	*token = (*g)->token;
	gscan_park(*g);
	return true;
}

bool
sift_tbl_group_fetch(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv, bool lazy)
{
	SiftTblGroupScan *g;
	int			k;
	uint32		pos;
	bool		has;

	if (!st->initialized || ItemPointerGetBlockNumberNoCheck(tid) >= SIFT_TBL_NEXT_VBLOCK(&st->dir.meta))
		return false;
	g = fetch_scan(rel, st, ItemPointerGetBlockNumberNoCheck(tid) / SIFT_SB_PAGES);
	has = gscan_find(g, tid, &k, &pos);
	*rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.wbuf = false};
	if (has)
	{
		if (g->cu[k].xmin)
		{
			rv->xmin = g->cu[k].xmin[pos];
			rv->cmin = g->cu[k].cmin[pos];
		}
		if (!lazy)
		{
			unit_load(g, k, ItemPointerGetBlockNumberNoCheck(tid) / SIFT_SB_PAGES);
			rv->tuple = row_tuple(g, pos, tid);
		}
	}
	gscan_park(g);
	return has;
}
