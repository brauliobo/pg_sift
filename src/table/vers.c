/*
 * table/vers.c - the versions of the rows of the groups with unfrozen rows (docs/COL-TAM.md section 7)
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
#include "core/cmp.h"

static const VerGroup *
ver_group(Relation rel, SiftTblRel *st, const SiftSegEntry *e)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	MemoryContext tmp;
	MemoryContext old;
	SiftColSegment *seg;
	VerGroup   *g;

	for (int i = 0; i < c->nvg; i++)
		if (c->vg[i].gen == e->gen)
			return &c->vg[i];
	tmp = AllocSetContextCreate(CurrentMemoryContext, "sift_table versions", ALLOCSET_SMALL_SIZES);
	seg = sift_cseg_open(rel, e, tmp);
	old = MemoryContextSwitchTo(st->cxt);
	c->vg = c->vg ? repalloc(c->vg, (c->nvg + 1) * sizeof(VerGroup)) : palloc((c->nvg + 1) * sizeof(VerGroup));
	g = &c->vg[c->nvg++];
	*g = (VerGroup) {.gen = e->gen,.nsb = seg->nsb,.sb = palloc(seg->nsb * sizeof(uint32)),.sum = palloc(seg->nsb * sizeof(SiftColVerSum))};
	MemoryContextSwitchTo(old);
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		g->sb[u] = seg->sbdir[u].sb;
		sift_cseg_ver_sum(seg, u, &g->sum[u]);
	}
	sift_cseg_close(seg);
	MemoryContextDelete(tmp);
	return g;
}

/* Forgets the groups that neither directory has any more */
static void
ver_groups_prune(SiftTblRel *st, const SiftDirSnapshot *dir)
{
	struct SiftTblGroupCache *c = gcache_of(st);
	int			n = 0;

	for (int i = 0; i < c->nvg; i++)
	{
		bool		used = false;

		for (int k = 0; k < dir->nsegs && !used; k++)
			used = dir->segs[k].gen == c->vg[i].gen;
		for (int k = 0; k < st->dir.nsegs && !used; k++)
			used = st->dir.segs[k].gen == c->vg[i].gen;
		if (used)
			c->vg[n++] = c->vg[i];
		else
		{
			pfree(c->vg[i].sb);
			pfree(c->vg[i].sum);
		}
	}
	c->nvg = n;
}

struct SiftTblVers
{
	Relation	rel;
	MemoryContext cxt;
	int			n;
	SiftSegEntry *ent;			/* [n] the groups with unfrozen rows of the directory copy */
	const VerGroup **grp;
	/* the unit whose versions were read last (a scan of a page at a time asks for the next page of the same unit) */
	int			seg_g;
	SiftColSegment *seg;
	int			cu_g;
	uint32		cu_u;
	uint32		cap;
	uint32	   *xm;
	uint32	   *cm;
};

SiftTblVers *
sift_tbl_vers_build(Relation rel, SiftTblRel *st, const SiftDirSnapshot *dir, MemoryContext cxt)
{
	SiftTblVers *v = MemoryContextAllocZero(cxt, sizeof(SiftTblVers));
	int			n = 0;

	for (int i = 0; i < dir->nsegs; i++)
		n += sift_tbl_seg_unfrozen(&dir->segs[i]);
	v->rel = rel;
	v->cxt = cxt;
	v->ent = MemoryContextAlloc(cxt, Max(n, 1) * sizeof(SiftSegEntry));
	v->grp = MemoryContextAlloc(cxt, Max(n, 1) * sizeof(VerGroup *));
	v->seg_g = v->cu_g = -1;
	for (int i = 0; i < dir->nsegs; i++)
		if (sift_tbl_seg_unfrozen(&dir->segs[i]))
		{
			const VerGroup *src = ver_group(rel, st, &dir->segs[i]);
			VerGroup   *g = MemoryContextAlloc(cxt, sizeof(VerGroup));

			/* a copy: the cache moves and forgets groups as the directory changes, and this one is of the directory it was built for */
			*g = *src;
			g->sb = MemoryContextAlloc(cxt, src->nsb * sizeof(uint32));
			g->sum = MemoryContextAlloc(cxt, src->nsb * sizeof(SiftColVerSum));
			memcpy(g->sb, src->sb, src->nsb * sizeof(uint32));
			memcpy(g->sum, src->sum, src->nsb * sizeof(SiftColVerSum));
			v->ent[v->n] = dir->segs[i];
			v->grp[v->n++] = g;
		}
	ver_groups_prune(st, dir);
	return v;
}

/* The group and unit that hold superblock sb (the superblocks of a group belong to it alone) */
static bool
vers_find(const SiftTblVers *v, uint32 sb, int *g, uint32 *u)
{
	for (int i = 0; v != NULL && i < v->n; i++)
	{
		const VerGroup *grp = v->grp[i];
		const uint32 *hit;

		if (sb < grp->sb[0] || sb > grp->sb[grp->nsb - 1])
			continue;
		if ((hit = bsearch(&sb, grp->sb, grp->nsb, sizeof(uint32), sift_cmp_u32)) != NULL)
		{
			*g = i;
			*u = hit - grp->sb;
			return true;
		}
	}
	return false;
}

SiftTblVerState
sift_tbl_vers_sb(Relation rel, Snapshot snapshot, SiftTblVers *vers, uint32 sb)
{
	int			g;
	uint32		u;

	if (!vers_find(vers, sb, &g, &u))
		return SIFT_TBL_VER_ALL;
	return sift_tbl_ver_state(rel, snapshot, &vers->grp[g]->sum[u]);
}

/* The versions of the rows of unit u of group g, read once for as long as the unit is the one asked for */
static void
vers_unit(SiftTblVers *v, int g, uint32 u)
{
	if (v->cu_g == g && v->cu_u == u)
		return;
	if (v->seg_g != g)
	{
		if (v->seg)
			sift_cseg_close(v->seg);
		v->seg = sift_cseg_open(v->rel, &v->ent[g], v->cxt);
		v->seg_g = g;
	}
	if (v->seg->sbdir[u].nrows > v->cap)
	{
		v->cap = v->seg->sbdir[u].nrows;
		v->xm = v->xm ? repalloc(v->xm, v->cap * sizeof(uint32)) : MemoryContextAlloc(v->cxt, v->cap * sizeof(uint32));
		v->cm = v->cm ? repalloc(v->cm, v->cap * sizeof(uint32)) : MemoryContextAlloc(v->cxt, v->cap * sizeof(uint32));
	}
	sift_cseg_ver_unit(v->seg, u, v->xm, v->cm);
	sift_cseg_close(v->seg);
	v->cu_g = g;
	v->cu_u = u;
}

void
sift_tbl_vers_page(SiftTblVers *vers, BlockNumber vblock, SiftTblVerPage *vp)
{
	int			g;
	uint32		u;
	bool		found;

	memset(vp, 0, sizeof(*vp));
	if (!vers_find(vers, vblock / SIFT_SB_PAGES, &g, &u) || !vers->grp[g]->sum[u].any)
		return;
	vers_unit(vers, g, u);
	found = sift_cseg_page_rows(vers->seg, u, vblock, &vp->first, vp->offmask);
	sift_cseg_close(vers->seg);
	if (!found)
		return;
	vp->xmin = vers->xm;
	vp->cmin = vers->cm;
}

void
sift_tbl_vers_rowver(const SiftTblVerPage *vp, uint32 off, TransactionId *xmin, CommandId *cmin)
{
	uint32		rank = 0;

	*xmin = InvalidTransactionId;
	*cmin = 0;
	if (vp->xmin == NULL || !sift_col_bit_test(vp->offmask, off - 1))
		return;
	for (uint32 w = 0; w < (off - 1) / 64; w++)
		rank += pg_popcount64(vp->offmask[w]);
	if ((off - 1) % 64)
		rank += pg_popcount64(vp->offmask[(off - 1) / 64] & ((UINT64CONST(1) << ((off - 1) % 64)) - 1));
	*xmin = vp->xmin[vp->first + rank];
	*cmin = vp->cmin[vp->first + rank];
}
