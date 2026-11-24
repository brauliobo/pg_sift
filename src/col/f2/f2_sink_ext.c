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
#include "col/f2/f2_col_delta_idx.h"
#include "col/f2/f2_pull_prune.h"
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

	SiftXidCache xc;
	SiftPart   *part;
	const SiftPartDesc *pd;
	SiftGranuleDesc *dir;
	SiftDv	   *dv;
	SiftTidIndexEntry *tix;
	SiftColDeltaPartMap *dmap;	/* what the snapshot sees of the column deltas of the part: NULL when it sees none */
	const SiftColDeltaGranMap *gm;	/* the patches of the granule being read, NULL: none */
	uint32		g;				/* the granule being read */
	SiftF2PullPrune prune;
	SiftF2PullStats stats;

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
	SiftVecType child_type[SIFT_F2_PULL_MAX_CHILD];
	SiftGranuleDict *child_dict[SIFT_F2_PULL_MAX_CHILD];
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
	sift_f2_pull_prune_begin(&p->prune, index, spec);
	sift_xid_cache_init(&p->xc, snap);
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

const SiftF2PullStats *
sift_f2_pull_stats(const SiftF2Pull *p)
{
	return &p->stats;
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
	p->dmap = (p->e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0 ? sift_cdi_part_map(p->index, p->e, p->part, &p->xc) : NULL;
	p->gm = NULL;
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
	sift_f2_pull_prune_open(&p->prune, p->index, p->part, p->dir, p->dmap, &p->stats);
	MemoryContextSwitchTo(old);
}

/* the leaves a granule is first read for: the _tid and the lengths of the arrays, which say which rows are live and where the elements are before any column is read */
static int
head_chunks(const SiftF2Pull *p, int *chunks)
{
	int			nc = 0;

	chunks[nc++] = SIFT_COL_TID_IDX;
	if (p->spec.group_leaf >= 0)
		chunks[nc++] = p->spec.group_leaf + 1;
	return nc;
}

/* the leaves of a granule that is not ruled out: the columns, the keys, and the elements when the granule has any */
static int
body_chunks(const SiftF2Pull *p, int *chunks)
{
	int			nc = 0;

	for (int i = 0; i < p->spec.ncols; i++)
		chunks[nc++] = p->spec.col[i].leaf + 1;
	for (int m = 0; m < p->spec.nmasks; m++)
		chunks[nc++] = p->spec.mask[m].leaf + 1;
	for (int c = 0; p->n_elems > 0 && c < p->spec.nchild; c++)
		chunks[nc++] = p->spec.child[c].leaf + 1;
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

/* a value a vector cannot hold (an exception of a chunk): the consumer reads the rows of the granule from the heap, as it does those of a slab */
static void
poison_granule(SiftF2Pull *p)
{
	memset(p->poison, 0xff, ((p->n + 63) / 64) * sizeof(uint64));
}

/* a value a vector of a column cannot hold: the rows of the granule are the consumer's when it reads arrays, else the scan has no answer */
static void
unholdable(SiftF2Pull *p)
{
	if (p->spec.group_leaf < 0)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a column of the index holds a value its vector cannot"),
				errhint("Read the column through a scan that decodes exceptions.")));
	poison_granule(p);
}

static void
put_patch(SiftVec *v, uint16 j, const SiftColDeltaPatch *pt)
{
	const bool	isnull = (pt->flags & SIFT_CDCOL_IS_NULL) != 0;

	switch (v->type)
	{
		case SVEC_I32:
			((int32 *) v->data)[j] = isnull ? 0 : (int32) pt->val.i;
			break;
		case SVEC_F64:
			((double *) v->data)[j] = isnull ? 0 : pt->val.f;
			break;
		case SVEC_STRVIEW:
			((SiftStrView *) v->data)[j] = isnull ? (SiftStrView) {0} : sift_vstr_make(pt->val.s, pt->vlen);
			break;
		default:
			((int64 *) v->data)[j] = isnull ? 0 : pt->val.i;
	}
	v->valid[j >> 6] = (v->valid[j >> 6] & ~(UINT64CONST(1) << (j & 63))) | ((uint64) !isnull << (j & 63));
}

/* the validity of the cells of column i in a bitmap of the pull, which a patch may write */
static void
own_valid(SiftF2Pull *p, int i, uint16 cnt)
{
	SiftVec    *v = &p->vec[i];

	if (v->all_valid)
		memset(p->valid[i], 0xff, SIFT_VEC_WORDS * sizeof(uint64));
	else
		memcpy(p->valid[i], v->valid, ((cnt + 63) / 64) * sizeof(uint64));
	v->valid = p->valid[i];
	v->all_valid = false;
}

/* the cells of the rows [off, off + cnt) of column i that the column deltas the snapshot sees changed, over the cells the granule holds; a cell the vector cannot hold is the consumer's */
static void
patch_col(SiftF2Pull *p, int i, uint16 off, uint16 cnt)
{
	SiftVec    *v = &p->vec[i];
	uint32		lo = 0,
				hi = 0;

	if (p->gm == NULL)
		return;
	sift_col_delta_patch_slice(p->gm, (int16) (p->spec.col[i].leaf + 1), &lo, &hi);
	for (uint32 k = lo; k < hi; k++)
	{
		const SiftColDeltaPatch *pt = &p->gm->patches[k];

		if (pt->row_ordinal < off || pt->row_ordinal >= off + cnt)
			continue;
		if (v->all_valid || v->valid != p->valid[i])
			own_valid(p, i, cnt);
		put_patch(v, pt->row_ordinal - off, pt);
		if ((pt->flags & SIFT_CDCOL_HAS_TAG) != 0 && pt->tag != 0)
			unholdable(p);
	}
}

/* an element column held as a dictionary in this granule: its codes, whole, and the dictionary they name; false when the chunk is no dictionary */
static bool
decode_child_codes(SiftF2Pull *p, int c)
{
	const uint32 n = p->n_elems;
	uint32	   *codes = MemoryContextAllocAligned(p->gcx, Max((Size) n * sizeof(uint32), 1) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	bool	   *isnull = palloc(Max(n, 1));
	SiftGranuleDict *dict = palloc(sizeof(SiftGranuleDict));

	if (!sift_granule_str_codes(&p->gv, p->spec.child[c].leaf, codes, isnull, dict))
		return false;
	p->child_valid[c] = palloc0(((n + 63) / 64 + 1) * sizeof(uint64));
	for (uint32 e = 0; e < n; e++)
		p->child_valid[c][e >> 6] |= (uint64) !isnull[e] << (e & 63);
	p->child_data[c] = codes;
	p->child_dict[c] = dict;
	p->child_type[c] = SVEC_DICT32;
	return true;
}

/* an element column decoded batch by batch into whole-granule arrays of the cells of its type */
static void
decode_child_vec(SiftF2Pull *p, int c, SiftVecType type)
{
	const Size	es = esize(type);
	const uint32 words = (p->n_elems + 63) / 64 + 1;
	SiftVec		tmp = {.type = type,.scale = p->spec.child[c].scale};
	uint64		tv[SIFT_VEC_WORDS];

	tmp.data = palloc_aligned(SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	tmp.valid = tv;
	sift_vec_cursor_open(&p->gv, p->spec.child[c].leaf, type, &p->child_cur[p->nchild_cur++]);
	p->child_data[c] = palloc(Max((Size) p->n_elems * es, 1) + SIFT_VEC_PAD);
	p->child_valid[c] = palloc0(words * sizeof(uint64));
	p->child_type[c] = type;
	for (uint32 at = 0; at < p->n_elems; at += SIFT_VEC_LEN)
	{
		const uint16 cnt = (uint16) Min(SIFT_VEC_LEN, p->n_elems - at);
		const SiftVariantExcStream *exc = NULL;

		sift_vec_cursor_decode(&p->child_cur[p->nchild_cur - 1], (uint16) at, cnt, &tmp, &exc);
		if (exc != NULL)
			poison_granule(p);
		memcpy((char *) p->child_data[c] + (Size) at * es, tmp.data, (Size) cnt * es);
		for (uint16 i = 0; i < cnt; i++)
			if (tmp.all_valid || ((tv[i >> 6] >> (i & 63)) & 1))
				p->child_valid[c][(at + i) >> 6] |= UINT64CONST(1) << ((at + i) & 63);
	}
}

static void
decode_children(SiftF2Pull *p)
{
	for (int c = 0; c < p->spec.nchild; c++)
	{
		const bool	dict = p->spec.child[c].type == SVEC_DICT32;

		p->child_dict[c] = NULL;
		if (p->n_elems == 0)
		{
			p->child_type[c] = dict ? SVEC_STRVIEW : p->spec.child[c].type;
			p->child_data[c] = palloc0(SIFT_VEC_PAD);
			p->child_valid[c] = palloc0(sizeof(uint64));
		}
		else if (dict && decode_child_codes(p, c))
			p->stats.child_dict_granules++;
		else
		{
			decode_child_vec(p, c, dict ? SVEC_STRVIEW : p->spec.child[c].type);
			p->stats.child_str_granules += dict;
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
}

static void
finish_live(SiftF2Pull *p, const uint64 *dead)
{
	const uint32 words = (p->n + 63) / 64;
	const bool	any_tid = p->is_slab || sift_cdi_gran_remaps(p->gm) || (p->px != NULL && sift_pdel_idx_may_hit_granule(p->px, &p->tix[p->g]));

	p->live = palloc0((words + 1) * sizeof(uint64));
	for (uint32 r = 0; r < p->n; r++)
	{
		bool		ok = dead == NULL || !((dead[r >> 6] >> (r & 63)) & 1);

		if (ok && p->px != NULL && any_tid && sift_pdel_idx_has(p->px, p->tids[r]))
			ok = false;
		p->live[r >> 6] |= (uint64) ok << (r & 63);
	}
}

/* a live row whose array the columns cannot hold: the consumer reads it from the heap, so the zones of the elements say nothing of it */
static bool
live_poison(const SiftF2Pull *p)
{
	for (uint32 w = 0; w < (p->n + 63) / 64; w++)
		if (p->live[w] & p->poison[w])
			return true;
	return false;
}

/* whether the zones of the element leaves say that no element of the granule passes */
static bool
elems_ruled_out(const SiftF2Pull *p)
{
	return p->spec.nelem_pred > 0 && p->has_layout && sift_f2_pull_prune_elems(&p->prune, p->g, p->n_elems);
}

/* no element: every row has an empty array, and the elements are not read; the rows the layout cannot hold are still the consumer's */
static void
empty_layout(SiftF2Pull *p)
{
	const uint32 *zeros = palloc0((p->n + 2) * sizeof(uint32));

	p->lay = (SiftArrayLayout) {.n_parents = p->n,.n_elems = 0,.block_elem_off = zeros,.lengths = zeros,.offsets = zeros};
	p->n_elems = 0;
}

/*
 * A granule of the part: the _tid and the lengths first, then the rest. When the zones of the elements rule every element out the granule is not read, but for the rows the layout cannot hold (they
 * come with no element); false when it is not read at all.
 */
static bool
begin_granule(SiftF2Pull *p)
{
	int			chunks[3 + SIFT_F2_PULL_MAX_COLS + SIFT_F2_PULL_MAX_MASKS + SIFT_F2_PULL_MAX_CHILD];
	const SiftDvGran *dg = p->dv != NULL ? sift_dv_granule(p->dv, p->g) : NULL;
	uint64		dead[SIFT_DV_MASK_WORDS];
	bool		any_dead = false;
	bool		no_elems;
	MemoryContext old;
	uint64	   *tids;

	close_cursors(p);
	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	p->is_slab = false;
	sift_lazy_granule_open(p->index, p->part, p->g, &p->lg);
	sift_lazy_granule_load(p->index, &p->lg, chunks, head_chunks(p, chunks));
	p->gv = sift_granule_open_part(p->index, p->part, p->lg.rec, p->lg.gd.data_len, &p->lg.gd);
	p->n = p->lg.gd.nrows;
	p->off = 0;
	tids = palloc(Max(p->n, 1) * sizeof(uint64));
	sift_granule_tids(&p->gv, tids);
	p->gm = p->dmap != NULL ? sift_col_delta_gran_map(p->dmap, p->g) : NULL;
	if (p->gm != NULL)
		sift_cdi_remap_tids(p->gm, tids, p->n);
	p->tids = tids;
	if (dg != NULL)
		any_dead = sift_dv_mask64(dg, p->snap, dead) > 0;
	finish_live(p, any_dead ? dead : NULL);
	if (p->spec.group_leaf >= 0)
		begin_layout(p);
	no_elems = elems_ruled_out(p);
	if (no_elems && !live_poison(p))
	{
		p->stats.granules_elem_skipped++;
		MemoryContextSwitchTo(old);
		return false;
	}
	if (no_elems)
	{
		empty_layout(p);
		p->stats.granules_elem_elided++;
	}
	sift_lazy_granule_load(p->index, &p->lg, chunks, body_chunks(p, chunks));
	p->stats.child_chunks += p->n_elems > 0 ? p->spec.nchild : 0;
	if (p->spec.group_leaf >= 0 && p->has_layout)
		decode_children(p);
	p->stats.granules_read++;
	MemoryContextSwitchTo(old);
	return true;
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
	p->gm = NULL;
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

/* Whether the sink of the spec answered the granule from what its part stores */
static bool
granule_answered(const SiftF2Pull *p)
{
	const SiftDvGran *dg = p->dv != NULL ? sift_dv_granule(p->dv, p->g) : NULL;

	return p->spec.gran_sink != NULL &&
		p->spec.gran_sink(p->spec.gran_arg, &(SiftF2GranOffer) {.rel = p->index,.part = p->part,.desc = p->pd,.schema = p->r->schema,.g = p->g,.gd = &p->dir[p->g],.dead = dg,.snap = p->snap,
							.pend_hit = p->px != NULL && sift_pdel_idx_may_hit_granule(p->px, &p->tix[p->g]),.px = p->px,.vis = SIFT_GVIS_HEAP});
}

/* the next granule, slab or part; false at the end of the index */
static bool
advance(SiftF2Pull *p)
{
	for (;;)
	{
		if (p->part != NULL && sift_f2_pull_prune_next(&p->prune, &p->g, &p->stats))
		{
			if (!granule_answered(p) && begin_granule(p))
				return true;
			continue;
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
			if (sift_f2_pull_prune_part(&p->prune, &p->list->parts[p->pi]))
			{
				p->pi++;
				p->stats.parts_skipped++;
			}
			else
				begin_part(p);
			continue;
		}
		if (p->ent >= p->plan->npartitions)
			return false;
		if (sift_f2_pull_prune_entry(&p->prune, &p->plan->partitions[p->ent]))
		{
			p->ent++;
			p->stats.partitions_skipped++;
			continue;
		}
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
				unholdable(p);
			patch_col(p, i, off, cnt);
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
	for (int c = 0; p->has_layout && c < p->spec.nchild; c++)
	{
		es->child_data[c] = p->child_data[c];
		es->child_valid[c] = p->child_valid[c];
		es->child_type[c] = p->child_type[c];
		es->child_dict[c] = p->child_dict[c];
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
