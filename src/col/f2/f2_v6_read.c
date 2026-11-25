/*
 * col/f2/f2_v6_read.c - the granules of an index of format major 2 for the operators of SiftAggV6 (f2_v6_read.h).
 */
#include "postgres.h"

#include "access/table.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"

#include "col/am.h"
#include "col/f2/delvec.h"
#include "core/f2/col_delta_anchor.h"
#include "col/f2/f2_v6_read.h"
#include "col/f2/l0_stage.h"
#include "col/f2/l0_slab.h"
#include "col/f2/rowbuf.h"
#include "col/granule/granule.h"

F2V6Ctx *
f2v6_begin(MemoryContext cx, Snapshot snap, Oid indexoid)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	F2V6Ctx    *c = palloc0(sizeof(F2V6Ctx));

	c->cx = cx;
	c->snap = snap;
	c->index = index_open(indexoid, AccessShareLock);
	c->heap = table_open(c->index->rd_index->indrelid, AccessShareLock);
	c->f = sift_f2_rel(c->index);
	c->rel = sift_col_rel(c->index);
	c->schema = c->rel->schema;
	sift_stage_flush(c->index);
	{
		int			nstaged;
		uint64	   *staged = sift_stage_dead(c->index, snap, &nstaged);

		c->staged_px = sift_f2_staged_index(staged, nstaged);
	}
	c->plan = sift_plan_cache_get(c->index, cx);
	c->vm_blk = InvalidBlockNumber;
	sift_xid_cache_init(&c->xc, snap);
	c->fetch = sift_table_index_fetch_begin(c->heap);
	c->slot = table_slot_create(c->heap, NULL);
	MemoryContextSwitchTo(old);
	return c;
}

void
f2v6_end(F2V6Ctx *c)
{
	if (BufferIsValid(c->vmbuf))
		ReleaseBuffer(c->vmbuf);
	ExecDropSingleTupleTableSlot(c->slot);
	table_index_fetch_end(c->fetch);
	table_close(c->heap, AccessShareLock);
	index_close(c->index, AccessShareLock);
}

const SiftPendDelIndex *
f2v6_pending(F2V6Ctx *c, const SiftPartitionEntry *e)
{
	return sift_f2_pending_index(c->index, e, c->snap, c->staged_px);
}

bool
f2v6_time_dirty(F2V6Ctx *c, const SiftPartitionEntry *e)
{
	return (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0 && (sift_manifest_read_col_delta(c->index, e).flags & (SIFT_CDLA_DIRTY_TIME0 | SIFT_CDLA_DIRTY_TIME1)) != 0;
}

bool
f2v6_entry_meets_time(F2V6Ctx *c, const SiftPartitionEntry *e, const SiftF2TimeWin *w)
{
	return sift_f2_entry_meets_time(e, w) || f2v6_time_dirty(c, e);
}

bool
f2v6_part_meets_time(F2V6Ctx *c, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftF2TimeWin *w)
{
	return sift_f2_part_meets_time(d, w) || f2v6_time_dirty(c, e);
}

F2V6Part *
f2v6_part_open(F2V6Ctx *c, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftPendDelIndex *px)
{
	F2V6Part   *p = palloc0(sizeof(F2V6Part));
	uint64		tlen;

	p->e = e;
	p->d = d;
	p->px = px;
	p->part = sift_part_open_desc(c->index, d, (uint32) e->part_id);
	p->ngranules = p->part->hdr.ngranules;
	p->dir = sift_part_granule_dir(c->index, p->part);
	p->cd = sift_cdi_part_map(c->index, e, p->part, &c->xc);
	if (d->dv_blk != InvalidBlockNumber)
	{
		uint16	   *rows = palloc(Max(p->ngranules, 1) * sizeof(uint16));

		for (uint32 g = 0; g < p->ngranules; g++)
			rows[g] = p->dir[g].nrows;
		p->dv = sift_dv_read(c->index, d, (uint32) e->part_id, rows);
	}
	if (px != NULL)
	{
		p->tix = sift_part_read_section(c->index, p->part, SEC_TID_INDEX, &tlen);
		if (p->tix == NULL || tlen != (uint64) p->ngranules * sizeof(*p->tix))
			SIFT_SEG_CORRUPT(c->index, p->part->hdr.part_gen, "The part has no usable TID index.");
	}
	return p;
}

static ColView
view_of(const SiftColDef *d, const SiftGranuleView *v, int c, int n)
{
	ColView		cv = {0};
	bool	   *nl = palloc(n);
	uint8	   *tg = palloc(n);

	cv.kind = d->store == SIFT_COLS_INT ? SIFT_CELL_INT : (d->store == SIFT_COLS_STR || d->store == SIFT_COLS_VAR) ? SIFT_CELL_STR : SIFT_CELL_F8;
	if (cv.kind == SIFT_CELL_INT)
	{
		int64	   *iv = palloc(n * sizeof(int64));

		sift_granule_ints(v, c, iv, nl);
		cv.i = iv;
	}
	else if (cv.kind == SIFT_CELL_F8)
	{
		double	   *fv = palloc(n * sizeof(double));

		sift_granule_f8(v, c, fv, nl);
		cv.f = fv;
	}
	else
	{
		SiftStrCell *sc = palloc(n * sizeof(SiftStrCell));
		const char **sp = palloc(n * sizeof(char *));
		uint32	   *sl = palloc(n * sizeof(uint32));

		sift_granule_strs(v, c, sc);
		for (int r = 0; r < n; r++)
		{
			sp[r] = sc[r].p;
			sl[r] = sc[r].len;
			nl[r] = sc[r].isnull;
		}
		cv.s = sp;
		cv.slen = sl;
	}
	sift_granule_tags(v, c, tg);
	cv.isnull = nl;
	cv.tag = tg;
	return cv;
}

static bool *
keyp_of(const SiftGranuleView *v, int c, int n)
{
	bool	   *kp = palloc0(n);

	for (int off = 0; off < n; off += SIFT_VEC_LEN)
	{
		uint64		mask[SIFT_VEC_WORDS];
		const uint16 cnt = Min(SIFT_VEC_LEN, n - off);
		const SiftMaskState st = sift_granule_decode_key_present(v, c, off, cnt, mask);

		for (int i = 0; i < cnt; i++)
			kp[off + i] = st == SIFT_MASK_ALL_ONE || (st == SIFT_MASK_BITS && ((mask[i >> 6] >> (i & 63)) & 1));
	}
	return kp;
}

static void
live_rows(uint64 *live, int n, const uint64 *dead, const SiftPendDelIndex *px, const uint64 *tid, bool pend_hit)
{
	const int	words = (n + 63) / 64;

	for (int w = 0; w < words; w++)
		live[w] = ~UINT64CONST(0);
	if (n & 63)
		live[words - 1] = (UINT64CONST(1) << (n & 63)) - 1;
	if (dead != NULL)
		for (int w = 0; w < words; w++)
			live[w] &= ~dead[w];
	if (pend_hit)
		for (int r = 0; r < n; r++)
			if (sift_pdel_idx_has(px, tid[r]))
				live[r >> 6] &= ~(UINT64CONST(1) << (r & 63));
}

void
f2v6_granule(F2V6Ctx *c, const F2V6Part *p, uint32 g, const int *leaves, int nleaves, F2V6Gran *out)
{
	SiftLazyGranule lg;
	SiftGranuleView v;
	const SiftDvGran *dg = p->dv != NULL ? sift_dv_granule(p->dv, g) : NULL;
	const SiftColDeltaGranMap *gm = p->cd != NULL ? sift_col_delta_gran_map(p->cd, g) : NULL;
	const bool	remapped = sift_cdi_gran_remaps(gm);
	uint64		dead[SIFT_DV_MASK_WORDS];
	bool		any_dead = false;
	int		   *chunks = palloc((nleaves + 1) * sizeof(int));

	sift_lazy_granule_open(c->index, p->part, g, &lg);
	chunks[0] = 0;
	for (int i = 0; i < nleaves; i++)
		chunks[i + 1] = leaves[i] + 1;
	sift_lazy_granule_load(c->index, &lg, chunks, nleaves + 1);
	pfree(chunks);
	v = sift_granule_open_part(c->index, p->part, lg.rec, lg.gd.data_len, &lg.gd);
	out->n = lg.gd.nrows;
	out->views = palloc0(c->schema->ncols * sizeof(ColView));
	for (int i = 0; i < nleaves; i++)
	{
		const int	leaf = leaves[i];

		if (out->views[leaf].isnull != NULL || out->views[leaf].keyp != NULL)
			continue;
		if (c->f->key_leaf[leaf])
			out->views[leaf].keyp = keyp_of(&v, leaf, out->n);
		else
			out->views[leaf] = view_of(&c->schema->cols[leaf], &v, leaf, out->n);
	}
	out->tid = palloc(out->n * sizeof(uint64));
	sift_granule_tids(&v, out->tid);
	if (gm != NULL)
		sift_cdi_patch_granule(gm, c->schema->ncols, out->views, out->tid, out->n, CurrentMemoryContext);
	if (dg != NULL)
		any_dead = sift_dv_mask64(dg, c->snap, dead) > 0;
	out->live = palloc((out->n + 63) / 64 * sizeof(uint64));
	live_rows(out->live, out->n, any_dead ? dead : NULL, p->px, out->tid, p->px != NULL && (remapped || sift_pdel_idx_may_hit_granule(p->px, &p->tix[g])));
}

void
f2v6_slab(F2V6Ctx *c, const SiftPartitionEntry *e, const SiftPendDelIndex *px, bool sorted, const int *leaves, int nleaves, F2V6Gran *out)
{
/* check-memory: allow bounded to one micro-slab: SIFT_L0_SLAB_MAX_PAGES pages */
	SiftRowBuf *rb = sift_rowbuf_new(&c->f->spec);
	BlockNumber *pages;
	int			npages;

	sift_l0_slab_read(c->index, e->part_id, e->l0_slab_blk, rb, &c->f->spec, c->snap, &pages, &npages);
	if (sorted)
		sift_rowbuf_sort(rb, &c->f->spec);
	out->n = rb->n;
	out->views = palloc0(c->schema->ncols * sizeof(ColView));
	for (int i = 0; i < nleaves; i++)
	{
		const int	leaf = leaves[i];

		out->views[leaf] = (ColView) {.kind = rb->kinds[leaf],.i = rb->i[leaf],.f = rb->f[leaf],.s = (const char *const *) rb->s[leaf],
		.slen = rb->slen[leaf],.isnull = rb->isnull[leaf],.tag = rb->tag[leaf]};
		if (c->f->key_leaf[leaf])
		{
			bool	   *kp = palloc(Max(rb->n, 1));

			for (int r = 0; r < rb->n; r++)
				kp[r] = sift_key_row_present(rb->isnull[leaf][r], rb->tag[leaf] != NULL ? rb->tag[leaf][r] : 0);
			out->views[leaf].keyp = kp;
		}
	}
	out->tid = rb->tid;
	out->live = palloc(((rb->n + 63) / 64 + 1) * sizeof(uint64));
	live_rows(out->live, rb->n, NULL, px, rb->tid, px != NULL);
}

SiftZone *
f2v6_zones(F2V6Ctx *c, const F2V6Part *p, int leaf)
{
	const SiftSecDirEntry *zs = sift_part_section(p->part, SEC_ZONES);
	const uint32 ng = p->ngranules;
	SiftZone   *z;

	if (zs == NULL || zs->len != (uint64) p->part->hdr.ncols * ng * sizeof(SiftZone))
		return NULL;
	z = palloc(ng * sizeof(SiftZone));
	sift_part_read_section_bytes(c->index, p->part, zs, (uint64) leaf * ng * sizeof(SiftZone), (uint64) ng * sizeof(SiftZone), z);
	for (uint32 g = 0; p->cd != NULL && g < ng; g++)
	{
		const SiftColDeltaGranMap *gm = sift_col_delta_gran_map(p->cd, g);

		if (gm != NULL && (gm->attnum_bloom & sift_cd_att_bit((int16) (leaf + 1))) != 0)
			z[g].flags |= SIFT_ZONE_NONE;
	}
	return z;
}

bool
f2v6_row_visible(F2V6Ctx *c, uint64 addr)
{
	ItemPointerData tid;
	bool		again = false,
				dead = false;

	sift_f2_tid_set(&tid, addr);
	if (ItemPointerGetBlockNumber(&tid) != c->vm_blk)
	{
		c->vm_blk = ItemPointerGetBlockNumber(&tid);
		c->vm_visible = VM_ALL_VISIBLE(c->heap, c->vm_blk, &c->vmbuf);
	}
	return c->vm_visible || table_index_fetch_tuple(c->fetch, &tid, c->snap, c->slot, &again, &dead);
}

bool
f2v6_fetch(F2V6Ctx *c, uint64 addr, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	ItemPointerData tid;
	bool		again = false,
				dead = false;

	sift_f2_tid_set(&tid, addr);
	if (!table_index_fetch_tuple(c->fetch, &tid, c->snap, c->slot, &again, &dead))
		return false;
	for (int i = 0; i < n; i++)
		values[i] = slot_getattr(c->slot, attnos[i], &isnull[i]);
	return true;
}
