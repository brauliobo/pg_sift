/*
 * col/f2/f2_sink_ext.c - the rows of an index of format major 2 taken a slice at a time (col/f2/f2_sink_ext.h): the partitions of the plan snapshot in the order of their keys, for each the rows of
 * its slab (not yet in a part) and then its parts, for each part its granules; a granule is read for the chunks the consumer named, decoded to vectors in slices of SIFT_VEC_LEN rows.
 */
#include "postgres.h"

#include "miscadmin.h"
#include "utils/memutils.h"

#include "col/am.h"
#include "col/f2/delvec.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_sink_ext.h"
#include "col/f2/l0_slab.h"
#include "col/f2/l0_stage.h"
#include "col/f2/part_format.h"
#include "col/f2/pend_del.h"
#include "col/f2/rowbuf.h"
#include "col/granule/granule.h"
#include "core/memtrack.h"
#include "agg/vexpr_str.h"

struct SiftF2Pull
{
	Relation	index;
	Snapshot	snap;
	const SiftColRel *r;
	const SiftPlanSnapshot *plan;
	SiftF2PullSpec spec;
	MemoryContext cx;			/* the scan */
	MemoryContext ecx;			/* the partition entry: its pending deletes and its slab */
	MemoryContext pcx;			/* the part: its header, directory and deletion vector */
	MemoryContext gcx;			/* the granule or the slab: its record, ids and decoded elements */
	const SiftPendDelIndex *staged_px;

	int			ent;			/* the next partition entry */
	int			pi;				/* the next part of its list */
	bool		slab_pending;
	const SiftPartitionEntry *e;
	const SiftPlanPartList *list;
	SiftPendDelIndex *px;

	SiftPart   *part;
	const SiftPartDesc *pd;
	SiftGranuleDesc *dir;
	SiftDv	   *dv;
	SiftTidIndexEntry *tix;
	uint32		g;

	uint32		n;				/* the rows of the granule or slab being sliced, and the next slice's first */
	uint32		off;
	bool		is_slab;
	SiftLazyGranule lg;
	SiftGranuleView gv;
	SiftRowBuf *rb;
	const uint64 *tids;
	uint64	   *live;
	bool		has_layout;
	SiftArrayLayout lay;
	uint64	   *poison;
	uint64	   *doc_valid;
	void	   *child_data[SIFT_F2_PULL_MAX_CHILD];
	uint64	   *child_valid[SIFT_F2_PULL_MAX_CHILD];
	uint32		n_elems;
	SiftGranVecCursor cur[SIFT_F2_PULL_MAX_COLS];	/* the cursor of each column of the granule, opened by its first slice: the chunk is opened once for all of them */
	bool		cur_on[SIFT_F2_PULL_MAX_COLS];
	SiftGranVecCursor child_cur[SIFT_F2_PULL_MAX_CHILD];	/* the strings of an element column live as long as its cursor */
	int			nchild_cur;

	SiftVec		vec[SIFT_F2_PULL_MAX_COLS];
	uint64	   *valid[SIFT_F2_PULL_MAX_COLS];
	SiftVexprMaskIn masks[SIFT_F2_PULL_MAX_MASKS];
	uint64	   *mbits[SIFT_F2_PULL_MAX_MASKS];
	SiftF2ElemStream es;
	SiftF2ExtBatch out;
};

static Size
esize(SiftVecType t)
{
	switch (t)
	{
		case SVEC_BOOL:
			return 1;
		case SVEC_I32:
		case SVEC_DICT32:
			return 4;
		case SVEC_I64:
		case SVEC_F64:
		case SVEC_NUM64:
			return 8;
		default:
			return 16;
	}
}

SiftF2Pull *
sift_f2_pull_begin(Relation index, Snapshot snap, const SiftF2PullSpec *spec)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 pull");
	SiftF2Pull *p = MemoryContextAllocZero(cx, sizeof(SiftF2Pull));

	p->index = index;
	p->snap = snap;
	p->spec = *spec;
	p->cx = cx;
	p->ecx = sift_mem_allocset_create(cx, "sift f2 pull partition");
	p->pcx = sift_mem_allocset_create(cx, "sift f2 pull part");
	p->gcx = sift_mem_allocset_create(cx, "sift f2 pull granule");
	p->r = sift_col_rel(index);
	sift_stage_flush(index);
	{
		int			nstaged;
		uint64	   *staged = sift_stage_dead(index, snap, &nstaged);

		p->staged_px = sift_f2_staged_index(staged, nstaged);
	}
	p->plan = sift_plan_cache_get(index, cx);
	for (int i = 0; i < spec->ncols; i++)
	{
		p->vec[i] = (SiftVec) {.type = spec->col[i].type,.scale = spec->col[i].scale};
		p->vec[i].data = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		p->valid[i] = MemoryContextAllocZero(cx, SIFT_VEC_WORDS * sizeof(uint64));
		p->vec[i].valid = p->valid[i];
	}
	for (int m = 0; m < spec->nmasks; m++)
		p->mbits[m] = MemoryContextAllocZero(cx, SIFT_VEC_WORDS * sizeof(uint64));
	return p;
}

void
sift_f2_pull_end(SiftF2Pull *p)
{
	MemoryContextDelete(p->cx);
}

bool
sift_f2_pull_single_runs(Relation index)
{
	const SiftPlanSnapshot *plan = sift_plan_cache_get(index, CurrentMemoryContext);

	for (int i = 0; i < plan->npartitions; i++)
		if (plan->partlists[i].nparts > 1 || plan->partitions[i].l0_slab_blk != InvalidBlockNumber)
			return false;
	return true;
}

/* ---- the pending deletes of an entry, its parts, its granules ---- */

static void
begin_entry(SiftF2Pull *p)
{
	MemoryContextReset(p->ecx);
	p->e = &p->plan->partitions[p->ent];
	p->list = &p->plan->partlists[p->ent];
	p->pi = 0;
	p->slab_pending = p->e->l0_slab_blk != InvalidBlockNumber;
	p->px = NULL;
	{
		MemoryContext old = MemoryContextSwitchTo(p->ecx);

		p->px = sift_f2_pending_index(p->index, p->e, p->snap, p->staged_px);
		MemoryContextSwitchTo(old);
	}
	p->ent++;
}

static void
begin_part(SiftF2Pull *p)
{
	MemoryContext old;

	MemoryContextReset(p->pcx);
	old = MemoryContextSwitchTo(p->pcx);
	p->pd = &p->list->parts[p->pi++];
	p->part = sift_part_open_desc(p->index, p->pd, (uint32) p->e->part_id);
	p->dir = sift_part_granule_dir(p->index, p->part);
	p->dv = NULL;
	p->tix = NULL;
	if (p->pd->dv_blk != InvalidBlockNumber)
	{
		uint16	   *rows = palloc(Max(p->part->hdr.ngranules, 1) * sizeof(uint16));

		for (uint32 g = 0; g < p->part->hdr.ngranules; g++)
			rows[g] = p->dir[g].nrows;
		p->dv = sift_dv_read(p->index, p->pd, (uint32) p->e->part_id, rows);
	}
	if (p->px != NULL)
	{
		uint64		tlen;

		p->tix = sift_part_read_section(p->index, p->part, SEC_TID_INDEX, &tlen);
		if (p->tix == NULL || tlen != (uint64) p->part->hdr.ngranules * sizeof(*p->tix))
			SIFT_SEG_CORRUPT(p->index, p->part->hdr.part_gen, "The part has no usable TID index.");
	}
	p->g = 0;
	MemoryContextSwitchTo(old);
}

/* the leaves the granule is read for: the _tid, the columns, the keys, the lengths of the arrays and their elements */
static int
granule_chunks(const SiftF2Pull *p, int *chunks)
{
	int			nc = 0;

	chunks[nc++] = SIFT_COL_TID_IDX;
	for (int i = 0; i < p->spec.ncols; i++)
		chunks[nc++] = p->spec.col[i].leaf + 1;
	for (int m = 0; m < p->spec.nmasks; m++)
		chunks[nc++] = p->spec.mask[m].leaf + 1;
	if (p->spec.group_leaf >= 0)
	{
		chunks[nc++] = p->spec.group_leaf + 1;
		for (int c = 0; c < p->spec.nchild; c++)
			chunks[nc++] = p->spec.child[c].leaf + 1;
	}
	return nc;
}

/* The cursors of the granule are closed before its memory goes: what a batch points to lives until then */
static void
close_cursors(SiftF2Pull *p)
{
	for (int i = 0; i < p->spec.ncols; i++)
		if (p->cur_on[i])
		{
			sift_vec_cursor_close(&p->cur[i]);
			p->cur_on[i] = false;
		}
	for (int c = 0; c < p->nchild_cur; c++)
		sift_vec_cursor_close(&p->child_cur[c]);
	p->nchild_cur = 0;
}

static void
decode_children(SiftF2Pull *p)
{
	const SiftF2PullSpec *spec = &p->spec;

	for (int c = 0; c < spec->nchild; c++)
	{
		const Size	es = esize(spec->child[c].type);
		const uint32 words = (p->n_elems + 63) / 64 + 1;
		SiftVec		tmp = {.type = spec->child[c].type,.scale = spec->child[c].scale};
		uint64		tv[SIFT_VEC_WORDS];

		tmp.data = palloc_aligned(SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		tmp.valid = tv;
		sift_vec_cursor_open(&p->gv, spec->child[c].leaf, tmp.type, &p->child_cur[c]);
		p->nchild_cur = c + 1;
		p->child_data[c] = palloc(Max((Size) p->n_elems * es, 1) + SIFT_VEC_PAD);
		p->child_valid[c] = palloc0(words * sizeof(uint64));
		for (uint32 at = 0; at < p->n_elems; at += SIFT_VEC_LEN)
		{
			const uint16 cnt = (uint16) Min(SIFT_VEC_LEN, p->n_elems - at);
			const SiftVariantExcStream *exc = NULL;

			sift_vec_cursor_decode(&p->child_cur[c], (uint16) at, cnt, &tmp, &exc);
			memcpy((char *) p->child_data[c] + (Size) at * es, tmp.data, (Size) cnt * es);
			for (uint16 i = 0; i < cnt; i++)
				if (tmp.all_valid || ((tv[i >> 6] >> (i & 63)) & 1))
					p->child_valid[c][(at + i) >> 6] |= UINT64CONST(1) << ((at + i) & 63);
		}
	}
}

static void
begin_layout(SiftF2Pull *p)
{
	const int	gl = p->spec.group_leaf;
	int64	   *lens = palloc(p->n * sizeof(int64));
	bool	   *isnull = palloc(p->n);
	uint8	   *tag = palloc(p->n);

	p->poison = palloc0(((p->n + 63) / 64 + 1) * sizeof(uint64));
	p->doc_valid = palloc0(((p->n + 63) / 64 + 1) * sizeof(uint64));
	p->has_layout = sift_granule_array_layout(&p->gv, gl, &p->lay);
	if (!p->has_layout)
	{
		memset(p->poison, 0xff, ((p->n + 63) / 64) * sizeof(uint64));
		p->n_elems = 0;
		return;
	}
	sift_granule_ints(&p->gv, gl, lens, isnull);
	sift_granule_tags(&p->gv, gl, tag);
	for (uint32 r = 0; r < p->n; r++)
	{
		p->poison[r >> 6] |= (uint64) (tag[r] != 0) << (r & 63);
		p->doc_valid[r >> 6] |= (uint64) !isnull[r] << (r & 63);
	}
	p->n_elems = p->lay.n_elems;
	decode_children(p);
}

static void
finish_live(SiftF2Pull *p, const uint64 *dead)
{
	const uint32 words = (p->n + 63) / 64;

	p->live = palloc0((words + 1) * sizeof(uint64));
	for (uint32 r = 0; r < p->n; r++)
	{
		bool		ok = dead == NULL || !((dead[r >> 6] >> (r & 63)) & 1);

		if (ok && p->px != NULL && (p->is_slab ? true : sift_pdel_idx_may_hit_granule(p->px, &p->tix[p->g - 1])) && sift_pdel_idx_has(p->px, p->tids[r]))
			ok = false;
		p->live[r >> 6] |= (uint64) ok << (r & 63);
	}
}

static void
begin_granule(SiftF2Pull *p)
{
	const uint32 g = p->g++;
	int			chunks[3 + SIFT_F2_PULL_MAX_COLS + SIFT_F2_PULL_MAX_MASKS + SIFT_F2_PULL_MAX_CHILD];
	const SiftDvGran *dg = p->dv != NULL ? sift_dv_granule(p->dv, g) : NULL;
	uint64		dead[SIFT_DV_MASK_WORDS];
	bool		any_dead = false;
	MemoryContext old;
	uint64	   *tids;

	close_cursors(p);
	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	p->is_slab = false;
	sift_lazy_granule_open(p->index, p->part, g, &p->lg);
	sift_lazy_granule_load(p->index, &p->lg, chunks, granule_chunks(p, chunks));
	p->gv = sift_granule_open_part(p->index, p->part, p->lg.rec, p->lg.gd.data_len, &p->lg.gd);
	p->n = p->lg.gd.nrows;
	p->off = 0;
	tids = palloc(Max(p->n, 1) * sizeof(uint64));
	sift_granule_tids(&p->gv, tids);
	p->tids = tids;
	if (dg != NULL)
		any_dead = sift_dv_mask64(dg, p->snap, dead) > 0;
	finish_live(p, any_dead ? dead : NULL);
	if (p->spec.group_leaf >= 0)
		begin_layout(p);
	MemoryContextSwitchTo(old);
}

static void
begin_slab(SiftF2Pull *p)
{
	const SiftF2Rel *f = sift_f2_rel(p->index);
	BlockNumber *pages;
	int			npages;
	MemoryContext old;

	close_cursors(p);
	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	p->slab_pending = false;
	p->rb = sift_rowbuf_new(&f->spec);
	sift_l0_slab_read(p->index, p->e->part_id, p->e->l0_slab_blk, p->rb, &f->spec, p->snap, &pages, &npages);
	p->is_slab = true;
	p->n = p->rb->n;
	p->off = 0;
	p->tids = p->rb->tid;
	finish_live(p, NULL);
	p->has_layout = false;
	p->poison = palloc((((p->n + 63) / 64) + 1) * sizeof(uint64));
	memset(p->poison, 0xff, (((p->n + 63) / 64) + 1) * sizeof(uint64));
	p->doc_valid = p->poison;
	p->n_elems = 0;
	MemoryContextSwitchTo(old);
}

/* the next granule, slab or part; false at the end of the index */
static bool
advance(SiftF2Pull *p)
{
	for (;;)
	{
		if (p->part != NULL && p->g < p->part->hdr.ngranules)
		{
			begin_granule(p);
			return true;
		}
		p->part = NULL;
		if (p->slab_pending)
		{
			begin_slab(p);
			if (p->n > 0)
				return true;
			continue;
		}
		if (p->e != NULL && p->pi < p->list->nparts)
		{
			begin_part(p);
			continue;
		}
		if (p->ent >= p->plan->npartitions)
			return false;
		begin_entry(p);
	}
}

/* ---- the slices ---- */

static void
fill_slab_col(SiftF2Pull *p, int i, uint16 off, uint16 cnt)
{
	const int	c = p->spec.col[i].leaf;
	SiftVec    *v = &p->vec[i];
	bool		nulls = false;

	memset(p->valid[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint16 j = 0; j < cnt; j++)
	{
		const bool	ok = !p->rb->isnull[c][off + j];

		p->valid[i][j >> 6] |= (uint64) ok << (j & 63);
		nulls |= !ok;
		switch (v->type)
		{
			case SVEC_I32:
				((int32 *) v->data)[j] = (int32) p->rb->i[c][off + j];
				break;
			case SVEC_F64:
				((double *) v->data)[j] = p->rb->f[c][off + j];
				break;
			case SVEC_STRVIEW:
				((SiftStrView *) v->data)[j] = ok ? sift_vstr_make(p->rb->s[c][off + j], p->rb->slen[c][off + j]) : (SiftStrView) {0};
				break;
			default:
				((int64 *) v->data)[j] = p->rb->i[c][off + j];
				break;
		}
	}
	v->valid = nulls ? p->valid[i] : NULL;
	v->all_valid = !nulls;
	v->len = cnt;
	v->dense = true;
	v->sel = NULL;
}

static void
fill_slab_mask(SiftF2Pull *p, int m, uint16 off, uint16 cnt)
{
	const int	c = p->spec.mask[m].leaf;

	memset(p->mbits[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint16 j = 0; j < cnt; j++)
		p->mbits[m][j >> 6] |= (uint64) sift_key_row_present(p->rb->isnull[c][off + j], p->rb->tag != NULL && p->rb->tag[c] != NULL ? p->rb->tag[c][off + j] : 0) << (j & 63);
}

static void
fill_granule_mask(SiftF2Pull *p, int m, uint16 off, uint16 cnt)
{
	const SiftMaskState st = sift_granule_decode_key_present(&p->gv, p->spec.mask[m].leaf, off, cnt, p->mbits[m]);

	if (st == SIFT_MASK_BITS)
		return;
	memset(p->mbits[m], st == SIFT_MASK_ALL_ONE ? 0xff : 0, SIFT_VEC_WORDS * sizeof(uint64));
}

static void
fill_slice(SiftF2Pull *p, uint16 off, uint16 cnt)
{
	for (int i = 0; i < p->spec.ncols; i++)
	{
		if (p->is_slab)
			fill_slab_col(p, i, off, cnt);
		else
		{
			const SiftVariantExcStream *exc = NULL;

			p->vec[i].valid = p->valid[i];
			if (!p->cur_on[i])
			{
				MemoryContext old = MemoryContextSwitchTo(p->gcx);

				sift_vec_cursor_open(&p->gv, p->spec.col[i].leaf, p->vec[i].type, &p->cur[i]);
				MemoryContextSwitchTo(old);
				p->cur_on[i] = true;
			}
			sift_vec_cursor_decode(&p->cur[i], off, cnt, &p->vec[i], &exc);
			if (exc != NULL)
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a column of the index holds a value its vector cannot"),
						errhint("Read the column through a scan that decodes exceptions.")));
		}
	}
	for (int m = 0; m < p->spec.nmasks; m++)
	{
		if (p->is_slab)
			fill_slab_mask(p, m, off, cnt);
		else
			fill_granule_mask(p, m, off, cnt);
		p->masks[m] = (SiftVexprMaskIn) {.bits = p->mbits[m],.valid = NULL};
	}
}

static void
fill_elems(SiftF2Pull *p, uint16 off, uint16 cnt)
{
	static const uint32 zero_offsets[2] = {0, 0};
	SiftF2ElemStream *es = &p->es;

	if (p->has_layout)
		es->layout = (SiftArrayLayout) {.n_parents = cnt,.n_elems = p->lay.n_elems,.block_elem_off = p->lay.block_elem_off + off / 64,.lengths = p->lay.lengths + off,.offsets = p->lay.offsets + off};
	else
		es->layout = (SiftArrayLayout) {.n_parents = 0,.n_elems = 0,.block_elem_off = zero_offsets,.lengths = zero_offsets,.offsets = zero_offsets};
	es->n_elems = p->n_elems;
	for (int c = 0; c < p->spec.nchild; c++)
	{
		es->child_data[c] = p->child_data[c];
		es->child_valid[c] = p->child_valid[c];
	}
	es->poison = p->poison + off / 64;
	es->doc_valid = p->doc_valid + off / 64;
}

const SiftF2ExtBatch *
sift_f2_pull_next(SiftF2Pull *p)
{
	uint16		cnt;
	uint16		off;

	while (p->off >= p->n)
	{
		CHECK_FOR_INTERRUPTS();
		if (!advance(p))
			return NULL;
	}
	off = (uint16) p->off;
	cnt = (uint16) Min(SIFT_VEC_LEN, p->n - p->off);
	p->off += cnt;
	fill_slice(p, off, cnt);
	p->out = (SiftF2ExtBatch) {.n = cnt,.col = p->vec,.masks = p->masks,.tid = p->tids + off,.live = p->live + off / 64};
	if (p->spec.group_leaf >= 0)
	{
		fill_elems(p, off, cnt);
		p->out.elem = &p->es;
	}
	return &p->out;
}
