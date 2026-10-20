/*
 * table/gscan.c - the group scan: the rows of the column groups in order, the rows of a vblock and the TID fetches
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/skey.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/pg.h"
#include "col/seg.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "bm25/merge.h"
#include "core/ops.h"
#include "table/table.h"
#include "table/seg_int.h"

#define SB_KEYS		(((uint64) SIFT_SB_PAGES) << 16)	/* keys of a superblock: (vblock << 16) | offset */

typedef struct GroupSrc
{
	SiftColSource *src;
	const SiftColSchema *schema;
	int		   *leaf;			/* [natts + 1] the leaf of attribute a in this schema, -1 for none */
	int			exc;
} GroupSrc;

typedef struct UnitTids
{
	int			src;
	uint32		ordinal;
	uint32		n;				/* live rows */
	ItemPointerData *tid;		/* of the live rows, ascending */
	uint32	   *ord;			/* of tid: the row index in the vectors of the unit, taken together with tid (VACUUM clears live bits in place) */
	uint32	   *xmin;			/* the versions of every row of the unit by that index; NULL when none is unfrozen */
	uint32	   *cmin;
} UnitTids;

/* A unit of the scan's range: which source, its superblock, and its place among the units of that superblock */
typedef struct UnitRef
{
	int			src;
	uint32		sb;
	uint32		ordinal;
} UnitRef;

typedef struct VisArg
{
	Relation	rel;
	SiftTblRel *st;
	Snapshot	snapshot;
	SiftTblVers *vers;
} VisArg;

struct SiftTblGroupScan
{
	Relation	rel;
	SiftTblRel *st;
	TupleDesc	desc;
	MemoryContext cxt;
	MemoryContext rowcxt;
	MemoryContext unitcxt;		/* the TIDs, selection and vectors of the unit being read (reset by the next one) */
	int			nsrc;
	GroupSrc   *s;
	Datum	   *values;
	bool	   *isnull;
	SiftColVec *vec;
	Snapshot	snapshot;
	SiftTblVers *vers;			/* of the directory copy the scan reads */
	SiftColSegment *useg;		/* the segment and unit of the current unit, to read its versions */
	uint32		uu;
	SiftColVerSum usum;
	uint32	   *xm;				/* its versions by row ordinal, read when a row's version is asked for */
	uint32	   *cm;
	bool		hidden;			/* the (MVCC) snapshot sees no row of the current unit: it has none to return */
	/* iteration */
	int			cur;
	bool		begun;
	uint32		sb_lo;
	uint32		sb_hi;
	SiftColUnit unit;
	SiftColSel	sel;
	ItemPointerData *tids;
	uint32	   *ord;			/* the row ordinals of tids: the position of a row in the vectors, which hold every row of the unit */
	uint32		n;
	uint32		i;
	bool		in_unit;
	bool		plain;
	/* the units of the range in the order of a forward scan, listed when a scan first goes backward; uidx is the current one (-1: none yet) */
	UnitRef    *units;
	uint32		nunits;
	int64		uidx;
	/* the rows of the superblock of the last block read */
	uint32		cache_sb;
	UnitTids   *cu;
	int			ncu;
	/* the unit of cu whose rows are read into vec (all of them: the next fetches of its rows decode nothing), valid for one cu */
	int			rd_unit;
	uint64		serial;			/* of cu */
	uint64		rd_serial;
	/* readers of the current unit that outlive the call that found the row: a slot holds a reference and its token */
	int			refs;
	bool		ended;
	bool		park_fill;		/* the fetch scan: the pages a fill pinned are released at once (nothing ends it before the transaction does) */
	uint32		token;		/* changes whenever another unit becomes the current one: vec, ord and unit are then not the slot's */
};

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

	return sift_tbl_filter_page_vers(v->rel, v->snapshot, v->vers, blk, offmask);
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
static void
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
			elog(ERROR, "sift_table: the unit %u of superblock %u is gone", u->ordinal, u->sb);
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

/* The leaf c of the current unit with every row (in the unit's own order: cheaper to decode than a selection that leaves a few dead rows out), read on first use */
static const SiftColVec *
col_vec(SiftTblGroupScan *g, int c)
{
	if (g->vec[c].n == PG_UINT32_MAX)
	{
		MemoryContext old = MemoryContextSwitchTo(g->unitcxt);

		sift_col_read(&g->unit, c, NULL, &g->vec[c]);
		MemoryContextSwitchTo(old);
	}
	return &g->vec[c];
}

static bool
exc_find(const SiftColVec *ev, uint32 i, int attno, const char **p, uint32 *len)
{
	const char *b = ev->vbytes + ev->voff[i],
			   *end = ev->vbytes + ev->voff[i + 1];

	while (b < end)
	{
		uint16		a;

		memcpy(&a, b, sizeof(a));
		memcpy(len, b + sizeof(a), sizeof(*len));
		b += sizeof(a) + sizeof(*len);
		if (a == attno)
		{
			*p = b;
			return true;
		}
		b += *len;
	}
	return false;
}

/* Attribute a of the row at position pos of the vectors of the current unit, allocated in the current memory context */
static void
fill_attr(SiftTblGroupScan *g, uint32 pos, int a, Datum *value, bool *isnull)
{
	const GroupSrc *gs = &g->s[g->cur];
	Form_pg_attribute att = TupleDescAttr(g->desc, a - 1);
	int			leaf = gs->leaf[a];
	const SiftColVec *vec;

	*isnull = false;
	*value = (Datum) 0;
	if (att->attisdropped)
		*isnull = true;
	else if (leaf < 0)
	{
		*value = getmissingattr(g->desc, a, isnull);
		if (!*isnull)
			*value = datumCopy(*value, att->attbyval, att->attlen);	/* like every value: the caller owns it */
	}
	else if ((vec = col_vec(g, leaf))->nulls != NULL && sift_col_bit_test(vec->nulls, pos))
	{
		const char *p;
		uint32		len;

		if (vec->ovf != NULL && sift_col_bit_test(vec->ovf, pos) && exc_find(col_vec(g, gs->exc), pos, a, &p, &len))
			*value = image_datum(att, p, len);
		else
			*isnull = true;
	}
	else
		*value = value_out(&gs->schema->cols[leaf], att, vec, pos);
}

bool
sift_tbl_gscan_fill(SiftTblGroupScan *g, uint32 token, uint32 pos, int from, int to, Datum *values, bool *isnull)
{
	if (g->token != token)
		return false;
	for (int a = from + 1; a <= to; a++)
		fill_attr(g, pos, a, &values[a - 1], &isnull[a - 1]);
	if (g->park_fill)
		gscan_park(g);
	return true;
}

uint32
sift_tbl_gscan_cursor(const SiftTblGroupScan *g, ItemPointer tid, uint32 *pos)
{
	*tid = g->tids[g->i - 1];
	*pos = g->ord[g->i - 1];
	return g->token;
}

/* The tuple of row i of the vectors read (position i of the selection) */
static HeapTuple
row_tuple(SiftTblGroupScan *g, uint32 i, ItemPointer tid)
{
	MemoryContext old = MemoryContextSwitchTo(g->rowcxt);
	HeapTuple	tup;

	for (int a = 1; a <= g->desc->natts; a++)
		fill_attr(g, i, a, &g->values[a - 1], &g->isnull[a - 1]);
	MemoryContextSwitchTo(old);
	tup = heap_form_tuple(g->desc, g->values, g->isnull);
	MemoryContextReset(g->rowcxt);
	tup->t_self = *tid;
	tup->t_tableOid = RelationGetRelid(g->rel);
	return tup;
}

HeapTuple
sift_tbl_gscan_tuple(SiftTblGroupScan *g)
{
	return row_tuple(g, g->ord[g->i - 1], &g->tids[g->i - 1]);
}

/* ---------- the rows of one vblock ---------- */

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
			pfree(g->cu[k].tid);
			pfree(g->cu[k].ord);
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
			*u = (UnitTids) {s, ordinal++, sel.count, palloc(Max(sel.count, 1) * sizeof(ItemPointerData)), palloc((sel.count + SIFT_COL_IDXPAD) * sizeof(uint32))};
			sift_col_tids(&unit, &sel, u->tid);
			sift_col_sel_idx(&sel, u->ord);
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

	while (lo < hi)
	{
		uint32		mid = (lo + hi) / 2;

		if (ItemPointerGetBlockNumberNoCheck(&u->tid[mid]) < vblock)
			lo = mid + 1;
		else
			hi = mid;
	}
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
			elog(ERROR, "sift_table: the unit %u of superblock %u is gone", u->ordinal, sb);
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

/* A fetch scan serving the TID fetches while the directory stays, and the xmin of the backend keeps the segments from being recycled */
static SiftTblGroupScan *
fetch_scan(Relation rel, SiftTblRel *st)
{
	struct SiftTblGroupCache *c = gcache_of(st);

	if (c->fetch && !fetch_kept(rel, st, c))
	{
		sift_tbl_gscan_end(c->fetch);
		c->fetch = NULL;
	}
	if (c->fetch == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(st->cxt);

		c->fetch = sift_tbl_gscan_begin(rel, RelationGetDescr(rel), st, &st->dir, NULL);
		c->fetch->park_fill = true;
		c->fetch_version = st->dir.version;
		c->fetch_xmin = TransactionXmin;
		MemoryContextSwitchTo(old);
	}
	return c->fetch;
}

/* Where tid is among the cached units of its superblock: unit k, position j of its TIDs */
static bool
gscan_find(SiftTblGroupScan *g, ItemPointer tid, int *k, uint32 *j)
{
	BlockNumber vblock = ItemPointerGetBlockNumberNoCheck(tid);

	cache_superblock(g, vblock / SIFT_SB_PAGES);
	for (*k = 0; *k < g->ncu; (*k)++)
		for (*j = block_start(&g->cu[*k], vblock); *j < g->cu[*k].n && ItemPointerGetBlockNumberNoCheck(&g->cu[*k].tid[*j]) == vblock; (*j)++)
			if (ItemPointerEquals(&g->cu[*k].tid[*j], tid))
				return true;
	return false;
}

bool
sift_tbl_group_locate(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblGroupScan **g, uint32 *token, uint32 *pos)
{
	int			k;
	uint32		j;

	if (!st->initialized || ItemPointerGetBlockNumberNoCheck(tid) >= SIFT_TBL_NEXT_VBLOCK(&st->dir.meta))
		return false;
	*g = fetch_scan(rel, st);
	if (!gscan_find(*g, tid, &k, &j))
	{
		gscan_park(*g);
		return false;
	}
	unit_load(*g, k, ItemPointerGetBlockNumberNoCheck(tid) / SIFT_SB_PAGES);
	*pos = (*g)->cu[k].ord[j];
	*token = (*g)->token;
	gscan_park(*g);
	return true;
}

bool
sift_tbl_group_fetch(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv, bool lazy)
{
	SiftTblGroupScan *g;
	uint64		offmask[SIFT_OFFMASK_WORDS] = {0};
	HeapTuple	row[SIFT_TBL_MAX_OFF];
	TransactionId xmin[SIFT_TBL_MAX_OFF];
	CommandId	cmin[SIFT_TBL_MAX_OFF];
	int			found;

	if (!st->initialized || ItemPointerGetBlockNumberNoCheck(tid) >= SIFT_TBL_NEXT_VBLOCK(&st->dir.meta))
		return false;
	g = fetch_scan(rel, st);
	if (lazy)
	{
		int			k;
		uint32		j;
		bool		has = gscan_find(g, tid, &k, &j);

		*rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.wbuf = false};
		if (has && g->cu[k].xmin)
		{
			rv->xmin = g->cu[k].xmin[g->cu[k].ord[j]];
			rv->cmin = g->cu[k].cmin[g->cu[k].ord[j]];
		}
		gscan_park(g);
		return has;
	}
	sift_col_bit_set(offmask, ItemPointerGetOffsetNumberNoCheck(tid) - 1);
	found = sift_tbl_gscan_block(g, ItemPointerGetBlockNumberNoCheck(tid), offmask, row, xmin, cmin);
	gscan_park(g);
	if (found == 0)
		return false;
	*rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.wbuf = false,.xmin = xmin[0],.cmin = cmin[0],.tuple = row[0]};
	return true;
}
