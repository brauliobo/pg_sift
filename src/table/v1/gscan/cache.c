/*
 * table/v1/gscan/cache.c - the group scan's cache of a superblock, the rows of a block, and the TID fetches (locate, fetch)
 */
/* check-memory: allow-allocs every array here follows the rows of one unit (at most SIFT_COL_UNIT_MAX_ROWS, 74,496) or of one chunk, not the rows of the table */
#include "table/v1/gscan/int.h"
#include "core/cachebudget.h"

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
			SIFT_TBL_RETRY("sift_table: the unit %u of superblock %u is gone", u->ordinal, sb);
	MemoryContextReset(g->unitcxt);
	for (int c = 0; c < g->s[g->cur].schema->ncols; c++)
		g->vec[c].n = PG_UINT32_MAX;
	g->rd_unit = k;
	g->rd_serial = g->serial;
}

int
sift_tbl_gscan_block(SiftTblGroupScan *g, BlockNumber vblock, const uint64 *offmask, HeapTuple *out, TransactionId *xmin, CommandId *cmin, SiftTblBlockRow *lazy)
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
				if (lazy)
				{
					lazy[nout++] = (SiftTblBlockRow) {.tid = u->tid[j],.token = g->token,.pos = u->ord[j],
						.xmin = u->xmin ? u->xmin[u->ord[j]] : InvalidTransactionId,.cmin = u->xmin ? u->cmin[u->ord[j]] : 0};
					continue;
				}
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
	if (!TransactionIdIsValid(TransactionXmin) || c->fetch_version != st->dir->version)
		return false;
	if (c->fetch_xmin != TransactionXmin)
	{
		if (c->fetch_version != sift_dir_version(rel))
			return false;
		c->fetch_xmin = TransactionXmin;
	}
	return true;
}

/* What a fetch scan holds: its context and the unit records its segments keep a buffer for */
static Size
fetch_bytes(SiftTblGroupScan *g)
{
	Size		bytes = MemoryContextMemAllocated(g->cxt, true);

	for (int k = 0; k < g->nsrc; k++)
		bytes += sift_col_unit_bytes(g->s[k].src);
	return bytes;
}

/* Fetch scan i leaves the cache: freed now, else once no slot holds a row of it (fetch_sweep) */
static void
fetch_drop(struct SiftTblGroupCache *c, int i)
{
	SiftTblGroupScan *g = c->fetch[i];
	MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(c));

	c->fetch[i] = NULL;
	if (g->refs > 0)
		c->dropped = lappend(c->dropped, g);
	MemoryContextSwitchTo(old);
	sift_tbl_gscan_end(g);
}

/* Frees the dropped fetch scans no slot holds a row of; all: every one, at the end of the transaction, which no slot outlives */
static void
fetch_sweep(struct SiftTblGroupCache *c, bool all)
{
	foreach_ptr(SiftTblGroupScan, g, c->dropped)
		if (all || g->refs == 0)
		{
			MemoryContextDelete(g->cxt);
			c->dropped = foreach_delete_current(c->dropped, g);
		}
}

/*
 * The fetch scans are dropped, oldest first, until what they hold fits sift_cache_scan_bytes(), with a new one as large as the largest when one is to begin: the slot it takes,
 * an empty one, else the least recently used
 */
static int
fetch_fit(struct SiftTblGroupCache *c, bool begin)
{
	for (;;)
	{
		Size		bytes = 0,
					largest = 0,
					held;
		int			lru = -1,
					empty = -1;

		for (int i = 0; i < SIFT_TBL_FETCH_SCANS; i++)
		{
			if (c->fetch[i] == NULL)
			{
				empty = i;
				continue;
			}
			held = fetch_bytes(c->fetch[i]);
			largest = Max(largest, held);
			bytes += held;
			if (lru < 0 || c->fetch_used[i] < c->fetch_used[lru])
				lru = i;
		}
		if (lru < 0 || bytes + (begin ? largest : 0) <= sift_cache_scan_bytes())
			return empty >= 0 ? empty : lru;
		fetch_drop(c, lru);
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

	fetch_sweep(c, false);
	if (!fetch_kept(rel, st, c))
		for (int i = 0; i < SIFT_TBL_FETCH_SCANS; i++)
			if (c->fetch[i])
				fetch_drop(c, i);
	for (int i = 0; i < SIFT_TBL_FETCH_SCANS; i++)
		if (c->fetch[i] && c->fetch[i]->cache_sb == sb)
			pick = i;
	if (pick < 0)
	{
		MemoryContext old = MemoryContextSwitchTo(st->cxt);

		pick = fetch_fit(c, true);
		if (c->fetch[pick])
			fetch_drop(c, pick);
		c->fetch[pick] = sift_tbl_gscan_begin(rel, RelationGetDescr(rel), st, st->dir, NULL);
		c->fetch[pick]->park_fill = true;
		c->fetch[pick]->cached = true;
		c->fetch[pick]->point = true;
		c->fetch_version = st->dir->version;
		c->fetch_xmin = TransactionXmin;
		MemoryContextSwitchTo(old);
	}
	c->fetch_used[pick] = ++c->fetch_clock;
	return c->fetch[pick];
}

void
sift_tbl_fetch_trim(SiftTblRel *st)
{
	if (st->gcache == NULL)
		return;
	fetch_sweep(st->gcache, true);
	(void) fetch_fit(st->gcache, false);
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

	if (!st->initialized || ItemPointerGetBlockNumberNoCheck(tid) >= SIFT_TBL_NEXT_VBLOCK(&st->dir->meta))
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

	if (!st->initialized || ItemPointerGetBlockNumberNoCheck(tid) >= SIFT_TBL_NEXT_VBLOCK(&st->dir->meta))
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
