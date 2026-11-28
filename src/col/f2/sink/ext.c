/*
 * col/f2/sink/ext.c - the rows of a relation of format major 2 taken a slice at a time (col/f2/sink/ext.h). Of an index: the partitions of the plan snapshot in the order of their keys,
 * for each the rows of its slab (not yet in a part) and then its parts, for each part its granules; a granule is read for the chunks the consumer named, decoded to vectors in slices of
 * SIFT_VEC_LEN rows. A sift_table gives its granules through its store (ext_table.c), cut into the same slices.
 */
#include "postgres.h"

#include "catalog/pg_class_d.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#include "col/am.h"
#include "col/f2/del/delvec.h"
#include "col/f2/cache.h"
#include "col/f2/col_delta/idx.h"
#include "col/f2/pull_prune.h"
#include "col/f2/sink/ext_int.h"
#include "col/f2/l0/slab.h"
#include "col/f2/l0/stage.h"
#include "col/f2/part/format.h"
#include "col/f2/del/pend.h"
#include "col/f2/rowbuf/rowbuf.h"
#include "col/granule/granule.h"
#include "core/memtrack.h"
#include "core/xid_cache.h"
#include "agg/vexpr/str.h"

static const SiftF2PullHost index_host;
static const SiftPlanSnapshot no_partitions;

static void
index_leaves(SiftF2Pull *p)
{
	const SiftF2Rel *f = sift_f2_rel(p->index);

	p->host = &index_host;
	p->kinds = f->kinds;
	p->key_leaf = f->key_leaf;
	sift_f2_pull_prune_begin(&p->prune, p->index, &p->spec);
}

static void
index_begin(SiftF2Pull *p)
{
	int			nstaged;
	uint64	   *staged;

	index_leaves(p);
	sift_stage_flush(p->index);
	staged = sift_stage_dead(p->index, p->snap, &nstaged);
	p->staged_px = sift_f2_staged_index(staged, nstaged);
	p->plan = sift_plan_cache_get(p->index, p->cx);
}

static SiftF2Pull *
pull_new(Relation rel, Snapshot snap, const SiftF2PullSpec *spec)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 pull");
	SiftF2Pull *p = MemoryContextAllocZero(cx, sizeof(SiftF2Pull));

	p->index = rel;
	p->snap = snap;
	sift_xid_cache_init(&p->xc, snap);
	p->spec = *spec;
	p->cx = cx;
	p->ecx = sift_mem_allocset_create(cx, "sift f2 pull partition");
	p->pcx = sift_mem_allocset_create(cx, "sift f2 pull part");
	p->gcx = sift_mem_allocset_create(cx, "sift f2 pull granule");
	return p;
}

static SiftF2Pull *
pull_vectors(SiftF2Pull *p)
{
	const SiftF2PullSpec *spec = &p->spec;

	for (int i = 0; i < spec->ncols; i++)
	{
		p->vec[i] = (SiftVec) {.type = spec->col[i].type,.scale = spec->col[i].scale};
		p->vec[i].data = MemoryContextAllocAligned(p->cx, SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
		p->valid[i] = MemoryContextAllocZero(p->cx, SIFT_VEC_WORDS * sizeof(uint64));
		p->vec[i].valid = p->valid[i];
		if (spec->col[i].type == SVEC_STRVIEW && p->kinds[spec->col[i].leaf] == SIFT_CELL_INT)
			p->bools[i] = MemoryContextAllocAligned(p->cx, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	}
	for (int m = 0; m < spec->nmasks; m++)
		p->mbits[m] = MemoryContextAllocZero(p->cx, SIFT_VEC_WORDS * sizeof(uint64));
	return p;
}

SiftF2Pull *
sift_f2_pull_begin(Relation rel, Snapshot snap, const SiftF2PullSpec *spec)
{
	SiftF2Pull *p = pull_new(rel, snap, spec);
	MemoryContext old = MemoryContextSwitchTo(p->cx);

	if (rel->rd_rel->relkind == RELKIND_INDEX)
		index_begin(p);
	else
		sift_f2_pull_table_begin(p);
	MemoryContextSwitchTo(old);
	return pull_vectors(p);
}

SiftF2Pull *
sift_f2_pull_begin_units(Relation index, Snapshot snap, const SiftF2PullSpec *spec, const uint64 *staged_dead, uint32 nstaged)
{
	SiftF2Pull *p = pull_new(index, snap, spec);
	MemoryContext old = MemoryContextSwitchTo(p->cx);

	index_leaves(p);
	p->staged_px = sift_f2_staged_index(staged_dead, (int) nstaged);
	p->plan = &no_partitions;
	MemoryContextSwitchTo(old);
	return pull_vectors(p);
}

const SiftF2PullStats *
sift_f2_pull_stats(const SiftF2Pull *p)
{
	return &p->stats;
}

void
sift_f2_pull_end(SiftF2Pull *p)
{
	if (p->host->end != NULL)
		p->host->end(p);
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
	p->cd = sift_cdi_part_map(p->index, p->e, p->part, &p->xc);
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
	sift_f2_pull_prune_open(&p->prune, p->index, p->part, p->dir, p->cd, &p->stats);
	MemoryContextSwitchTo(old);
}

void
sift_f2_pull_unit(SiftF2Pull *p, const SiftParPartRef *ref, uint32 first, uint32 n)
{
	const bool	same_entry = p->e != NULL && p->unit_e.part_id == ref->part_id;
	MemoryContext old;

	p->unit_e = (SiftPartitionEntry) {.part_id = ref->part_id,.l0_slab_blk = ref->l0_slab_blk,.pend_del_blk = ref->pend_del_blk,.flags = ref->entry_flags,.manifest_gen = ref->manifest_gen,
	.manifest_blk = ref->manifest_blk};
	p->e = &p->unit_e;
	if (!same_entry)
	{
		MemoryContextReset(p->ecx);
		old = MemoryContextSwitchTo(p->ecx);
		p->px = sift_f2_pending_index(p->index, p->e, p->snap, p->staged_px);
		MemoryContextSwitchTo(old);
		p->unit_part = NULL;
	}
	p->part = NULL;
	p->slab_pending = (ref->flags & SIFT_PAR_PART_L0_SLAB) != 0;
	p->unit_list = (SiftPlanPartList) {.part_id = ref->part_id,.manifest_gen = ref->manifest_gen,.nparts = 1,.parts = &p->unit_pd};
	p->list = &p->unit_list;
	p->pi = 1;
	if (p->slab_pending)
		return;
	if (p->unit_part == NULL || p->unit_pd.part_gen != ref->desc.part_gen)
	{
		p->unit_pd = ref->desc;
		p->pi = 0;
		begin_part(p);
		p->unit_part = p->part;
	}
	p->part = p->unit_part;
	sift_f2_pull_prune_range(&p->prune, ref->g_base + first, ref->g_base + first + n);
}

/* the leaves a granule is first read for: the _tid and the lengths of the arrays, which say which rows are live and where the elements are before any column is read */
static int
head_chunks(const SiftF2Pull *p, int *chunks)
{
	int			nc = 0;

	chunks[nc++] = SIFT_COL_TID_IDX;
	for (int g = 0; g < p->spec.ngroups; g++)
		if (p->spec.group[g].leaf >= 0)
			chunks[nc++] = p->spec.group[g].leaf + 1;
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
	for (int g = 0; g < p->spec.ngroups; g++)
		for (int c = 0; p->grp[g].n_elems > 0 && c < p->spec.group[g].nchild; c++)
			chunks[nc++] = p->spec.group[g].child[c].leaf + 1;
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
void
sift_f2_pull_poison_granule(SiftF2Pull *p)
{
	memset(p->poison, 0xff, ((p->n + 63) / 64) * sizeof(uint64));
}

/* a value a vector of a column cannot hold: the rows of the granule are the consumer's when it reads arrays, else the scan has no answer */
void
sift_f2_pull_unholdable(SiftF2Pull *p)
{
	if (p->spec.ngroups == 0)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a column of the index holds a value its vector cannot"),
				errhint("Read the column through a scan that decodes exceptions.")));
	sift_f2_pull_poison_granule(p);
}

/* the `->>` text of a boolean of a bool leaf */
SiftStrView
sift_f2_pull_bool_text(int64 b)
{
	return b != 0 ? sift_vstr_make("true", 4) : sift_vstr_make("false", 5);
}

static void
put_patch(SiftF2Pull *p, int col, uint16 j, const SiftColDeltaPatch *pt)
{
	SiftVec    *v = &p->vec[col];
	const bool	isnull = (pt->flags & SIFT_CDCOL_IS_NULL) != 0;
	const int64 i = isnull ? 0 : pt->val.i;

	if ((pt->flags & SIFT_CDCOL_HAS_TAG) != 0 && pt->tag != 0 && pt->tag != SIFT_COLTAG_JNULL)
		sift_f2_pull_unholdable(p);
	switch (v->type)
	{
		case SVEC_BOOL:
			((uint8 *) v->data)[j] = i != 0;
			break;
		case SVEC_I32:
			((int32 *) v->data)[j] = (int32) i;
			break;
		case SVEC_I64:
		case SVEC_NUM64:
			((int64 *) v->data)[j] = i;
			break;
		case SVEC_F64:
			((double *) v->data)[j] = isnull ? 0 : pt->val.f;
			break;
		case SVEC_STRVIEW:
			((SiftStrView *) v->data)[j] = isnull ? (SiftStrView) {0} : p->bools[col] != NULL ? sift_f2_pull_bool_text(i) : sift_vstr_make(pt->val.s, pt->vlen);
			break;
		default:
			sift_f2_pull_unholdable(p);
	}
	if (isnull)
		v->valid[j >> 6] &= ~(UINT64CONST(1) << (j & 63));
	else
		v->valid[j >> 6] |= UINT64CONST(1) << (j & 63);
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
		put_patch(p, i, pt->row_ordinal - off, pt);
	}
}

/* an element column held as a dictionary in this granule: its codes, whole, and the dictionary they name; false when the chunk is no dictionary */
static bool
decode_child_codes(SiftF2Pull *p, int g, int c)
{
	PullGroup  *gr = &p->grp[g];
	const uint32 n = gr->n_elems;
	uint32	   *codes = MemoryContextAllocAligned(p->gcx, Max((Size) n * sizeof(uint32), 1) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	bool	   *isnull = palloc(Max(n, 1));
	SiftGranuleDict *dict = palloc(sizeof(SiftGranuleDict));

	if (!sift_granule_str_codes(&p->gv, p->spec.group[g].child[c].leaf, codes, isnull, dict))
		return false;
	gr->child_valid[c] = palloc0(((n + 63) / 64 + 1) * sizeof(uint64));
	for (uint32 e = 0; e < n; e++)
		gr->child_valid[c][e >> 6] |= (uint64) !isnull[e] << (e & 63);
	gr->child_data[c] = codes;
	gr->child_dict[c] = dict;
	gr->child_type[c] = SVEC_DICT32;
	return true;
}

/* the rows of the granule whose array holds an element the exceptions of its column name: the consumer reads those from the heap, the others from the leaves */
static void
poison_parents(SiftF2Pull *p, const PullGroup *gr, const SiftVariantExcStream *exc)
{
	uint32		r = 0;

	for (uint32 w = 0; w < (gr->n_elems + 63) / 64; w++)
		for (uint64 bits = exc->exc_mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint32 e = w * 64 + (uint32) __builtin_ctzll(bits);

			while (gr->lay.offsets[r + 1] <= e)
				r++;
			p->poison[r >> 6] |= UINT64CONST(1) << (r & 63);
		}
}

/* an element column decoded batch by batch into whole-granule arrays of the cells of its type, for the elements of the runs of the group: the others are zero and not valid */
static void
decode_child_vec(SiftF2Pull *p, int g, int c, SiftVecType type)
{
	PullGroup  *gr = &p->grp[g];
	const Size	es = sift_vec_cell_size(type);
	SiftVec		tmp = {.type = type,.scale = p->spec.group[g].child[c].scale};
	uint64		tv[SIFT_VEC_WORDS];

	tmp.data = palloc_aligned(SIFT_VEC_LEN * sizeof(SiftStrView) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	tmp.valid = tv;
	gr->child_data[c] = palloc_extended(Max((Size) gr->n_elems * es, 1) + SIFT_VEC_PAD, gr->every_elem ? 0 : MCXT_ALLOC_ZERO);
	gr->child_valid[c] = palloc0(((gr->n_elems + 63) / 64 + 1) * sizeof(uint64));
	gr->child_type[c] = type;
	if (gr->nruns > 0)
		sift_vec_cursor_open(&p->gv, p->spec.group[g].child[c].leaf, type, &p->child_cur[p->nchild_cur++]);
	for (int k = 0; k < gr->nruns; k++)
		for (uint32 at = gr->runs[k][0]; at < gr->runs[k][1]; at += SIFT_VEC_LEN)
		{
			const uint16 cnt = (uint16) Min(SIFT_VEC_LEN, gr->runs[k][1] - at);
			const SiftVariantExcStream *exc = NULL;

			sift_vec_cursor_decode(&p->child_cur[p->nchild_cur - 1], at, cnt, &tmp, &exc);
			if (exc != NULL && k == 0 && at == gr->runs[0][0])
				poison_parents(p, gr, exc);
			memcpy((char *) gr->child_data[c] + (Size) at * es, tmp.data, (Size) cnt * es);
			for (uint16 i = 0; i < cnt; i++)
				if (tmp.all_valid || ((tv[i >> 6] >> (i & 63)) & 1))
					gr->child_valid[c][(at + i) >> 6] |= UINT64CONST(1) << ((at + i) & 63);
		}
}

/* the value column of the leaf of the kind column c, decoded before it: what is not a value is a JSON null where the chunk keeps that kind, else missing */
static void
decode_child_kinds(SiftF2Pull *p, int g, int c)
{
	PullGroup  *gr = &p->grp[g];
	const SiftF2PullGroup *sg = &p->spec.group[g];
	const uint32 n = gr->n_elems;
	const int	leaf = sg->child[c].leaf;
	int64	   *kind = palloc_aligned(Max((Size) n * sizeof(int64), 1) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	uint8	   *tag = palloc(Max(n, 1));
	const uint64 *value = NULL;

	for (int v = 0; v < c; v++)
		if (sg->child[v].leaf == leaf && !sg->child[v].kind)
			value = gr->child_valid[v];
	SIFT_INVARIANT(value != NULL, "the column of the values of an element leaf precedes its kinds");
	sift_granule_tags(&p->gv, leaf, tag);
	for (uint32 e = 0; e < n; e++)
		kind[e] = ((value[e >> 6] >> (e & 63)) & 1) ? SIFT_ELEM_KIND_VALUE : tag[e] == SIFT_COLTAG_JNULL ? SIFT_ELEM_KIND_JNULL : SIFT_ELEM_KIND_ABSENT;
	gr->child_data[c] = kind;
	gr->child_valid[c] = palloc_aligned(((n + 63) / 64 + 1) * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	memset(gr->child_valid[c], 0xff, ((n + 63) / 64) * sizeof(uint64));
	gr->child_type[c] = SVEC_I64;
}

static void
decode_children(SiftF2Pull *p, int g)
{
	PullGroup  *gr = &p->grp[g];
	const SiftF2PullGroup *sg = &p->spec.group[g];

	for (int c = 0; c < sg->nchild; c++)
	{
		const bool	dict = sg->child[c].type == SVEC_DICT32;

		gr->child_dict[c] = NULL;
		if (gr->n_elems == 0)
		{
			gr->child_type[c] = dict ? SVEC_STRVIEW : sg->child[c].type;
			gr->child_data[c] = palloc0(SIFT_VEC_PAD);
			gr->child_valid[c] = palloc0(sizeof(uint64));
		}
		else if (sg->child[c].kind)
			decode_child_kinds(p, g, c);
		else if (dict && decode_child_codes(p, g, c))
			p->stats.child_dict_granules++;
		else
		{
			decode_child_vec(p, g, c, dict ? SVEC_STRVIEW : sg->child[c].type);
			p->stats.child_str_granules += dict;
		}
	}
}

/* a leaf of a key of a jsonb document the consumer reads holds, on some rows, a value of another type than its own (the payload of its exception): those rows are poison, the consumer reads them from the heap */
static bool
key_leaf_col(const SiftF2Pull *p, int i)
{
	return p->key_leaf[p->spec.col[i].leaf];
}

static void
poison_key_rows(SiftF2Pull *p)
{
	uint8	   *tag = palloc(Max(p->n, 1));

	for (int i = 0; i < p->spec.ncols; i++)
	{
		if (!key_leaf_col(p, i))
			continue;
		sift_granule_tags(&p->gv, p->spec.col[i].leaf, tag);
		for (uint32 r = 0; r < p->n; r++)
			p->poison[r >> 6] |= (uint64) (tag[r] != 0 && tag[r] != SIFT_COLTAG_JNULL) << (r & 63);
	}
}

/* the poison rows of the keys, and the runs of the elements of the rows the predicates of the rows leave (the consumer keeps no other) that each group decodes */
static void
elem_rows(SiftF2Pull *p)
{
	uint64	   *rows = palloc0((SIFT_CFILT_WORDS + 1) * sizeof(uint64));

	poison_key_rows(p);
	memcpy(rows, p->live, ((p->n + 63) / 64) * sizeof(uint64));
	sift_f2_pull_prune_rows(&p->prune, &p->lg, &p->gv, p->g, rows);
	for (int g = 0; g < p->spec.ngroups; g++)
		if (p->grp[g].has_layout)
			p->grp[g].every_elem = sift_f2_pull_elem_runs(&p->grp[g].lay, rows, p->n, &p->grp[g].runs, &p->grp[g].nruns);
}

/* the layout of array column g: a row is poison when the granule keeps no layout of it, or the row's document holds no array there */
static void
begin_group(SiftF2Pull *p, int g)
{
	PullGroup  *gr = &p->grp[g];
	const int	gl = p->spec.group[g].leaf;
	int64	   *lens = palloc(p->n * sizeof(int64));
	bool	   *isnull = palloc(p->n);
	uint8	   *tag = palloc(p->n);

	gr->doc_valid = palloc0(((p->n + 63) / 64 + 1) * sizeof(uint64));
	gr->has_layout = gl >= 0 && sift_granule_array_layout(&p->gv, gl, &gr->lay);
	gr->n_elems = 0;
	if (!gr->has_layout)
	{
		sift_f2_pull_poison_granule(p);
		return;
	}
	sift_granule_ints(&p->gv, gl, lens, isnull);
	sift_granule_tags(&p->gv, gl, tag);
	for (uint32 r = 0; r < p->n; r++)
	{
		p->poison[r >> 6] |= (uint64) (tag[r] != 0) << (r & 63);
		gr->doc_valid[r >> 6] |= (uint64) !isnull[r] << (r & 63);
	}
	gr->n_elems = gr->lay.n_elems;
}

static void
begin_layout(SiftF2Pull *p)
{
	p->poison = palloc0(((p->n + 63) / 64 + 1) * sizeof(uint64));
	for (int g = 0; g < p->spec.ngroups; g++)
		begin_group(p, g);
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
	return p->spec.nelem_pred > 0 && p->grp[0].has_layout && sift_f2_pull_prune_elems(&p->prune, p->g, p->grp[0].n_elems);
}

/* no element: every row has empty arrays, and the elements are not read; the rows a layout cannot hold are still the consumer's */
static void
empty_layout(SiftF2Pull *p)
{
	const uint32 *zeros = palloc0((p->n + 2) * sizeof(uint32));

	for (int g = 0; g < p->spec.ngroups; g++)
	{
		p->grp[g].lay = (SiftArrayLayout) {.n_parents = p->n,.n_elems = 0,.block_elem_off = zeros,.lengths = zeros,.offsets = zeros};
		p->grp[g].n_elems = 0;
	}
}

/*
 * A granule of the part: the _tid and the lengths first, then the rest. When the zones of the elements rule every element out the granule is not read, but for the rows the layout cannot hold (they
 * come with no element); false when it is not read at all.
 */
static bool
begin_granule(SiftF2Pull *p)
{
	int			chunks[2 + SIFT_F2_PULL_MAX_GROUPS + SIFT_F2_PULL_MAX_COLS + SIFT_F2_PULL_MAX_MASKS + SIFT_F2_PULL_MAX_GROUPS * SIFT_F2_PULL_MAX_CHILD];
	const SiftDvGran *dg = p->dv != NULL ? sift_dv_granule(p->dv, p->g) : NULL;
	uint64		dead[SIFT_DV_MASK_WORDS];
	bool		any_dead = false;
	bool		no_elems;
	MemoryContext old;
	uint64	   *tids;

	close_cursors(p);
	MemoryContextReset(p->gcx);
	memset(p->xtag, 0, sizeof(p->xtag));
	old = MemoryContextSwitchTo(p->gcx);
	p->is_slab = false;
	sift_lazy_granule_open(p->index, p->part, p->g, &p->lg);
	sift_lazy_granule_load(p->index, &p->lg, chunks, head_chunks(p, chunks));
	p->gv = sift_granule_open_part(p->index, p->part, p->lg.rec, p->lg.gd.data_len, &p->lg.gd);
	p->n = p->lg.gd.nrows;
	p->off = 0;
	tids = palloc(Max(p->n, 1) * sizeof(uint64));
	sift_granule_tids(&p->gv, tids);
	p->gm = p->cd != NULL ? sift_col_delta_gran_map(p->cd, p->g) : NULL;
	if (p->gm != NULL)
		sift_cdi_remap_tids(p->gm, tids, p->n);
	p->tids = tids;
	if (dg != NULL)
		any_dead = sift_dv_mask64(dg, p->snap, dead) > 0;
	finish_live(p, any_dead ? dead : NULL);
	if (p->spec.ngroups > 0)
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
	if (p->spec.ngroups > 0)
		elem_rows(p);
	for (int g = 0; g < p->spec.ngroups; g++)
	{
		p->stats.child_chunks += p->grp[g].n_elems > 0 ? p->spec.group[g].nchild : 0;
		if (p->grp[g].has_layout)
			decode_children(p, g);
	}
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
/* check-memory: allow bounded to one micro-slab: SIFT_L0_SLAB_MAX_PAGES pages */
	p->rb = sift_rowbuf_new(&f->spec);
	sift_l0_slab_read(p->index, p->e->part_id, p->e->l0_slab_blk, p->rb, &f->spec, p->snap, &pages, &npages);
	p->is_slab = true;
	p->gm = NULL;
	p->n = p->rb->n;
	p->off = 0;
	p->tids = p->rb->tid;
	finish_live(p, NULL);
	p->poison = palloc((((p->n + 63) / 64) + 1) * sizeof(uint64));
	memset(p->poison, 0xff, (((p->n + 63) / 64) + 1) * sizeof(uint64));
	for (int g = 0; g < p->spec.ngroups; g++)
		p->grp[g] = (PullGroup) {.doc_valid = p->poison};
	MemoryContextSwitchTo(old);
}

/* Whether the sink of the spec answered the granule from what its part stores */
static bool
granule_answered(const SiftF2Pull *p)
{
	const SiftDvGran *dg = p->dv != NULL ? sift_dv_granule(p->dv, p->g) : NULL;

	return p->spec.gran_sink != NULL &&
		p->spec.gran_sink(p->spec.gran_arg, &(SiftF2GranOffer) {.rel = p->index,.part = p->part,.desc = p->pd,.schema = sift_col_rel(p->index)->schema,.g = p->g,.gd = &p->dir[p->g],.dead = dg,.snap = p->snap,
							.patch = p->cd != NULL ? sift_col_delta_gran_map(p->cd, p->g) : NULL,.pend_hit = p->px != NULL && sift_pdel_idx_may_hit_granule(p->px, &p->tix[p->g]),.px = p->px,.vis = SIFT_GVIS_HEAP,.rows_pass = sift_f2_pull_prune_all(&p->prune, p->g)});
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
			if (sift_f2_pull_prune_part(&p->prune, p->e, &p->list->parts[p->pi]))
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
				((SiftStrView *) v->data)[j] = !ok ? (SiftStrView) {0} : p->bools[i] != NULL ? sift_f2_pull_bool_text(p->rb->i[c][off + j]) : sift_vstr_make(p->rb->s[c][off + j], p->rb->slen[c][off + j]);
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

/* the rows of the slice whose key a column delta of the snapshot adds or removes, over what the granule says of it */
static void
patch_mask(SiftF2Pull *p, int m, uint16 off, uint16 cnt)
{
	uint32		lo = 0,
				hi = 0;

	if (p->gm == NULL)
		return;
	sift_col_delta_patch_slice(p->gm, (int16) (p->spec.mask[m].leaf + 1), &lo, &hi);
	for (uint32 k = lo; k < hi; k++)
	{
		const SiftColDeltaPatch *pt = &p->gm->patches[k];
		uint16		j;
		uint64		bit;

		if (pt->row_ordinal < off || pt->row_ordinal >= off + cnt)
			continue;
		j = pt->row_ordinal - off;
		bit = UINT64CONST(1) << (j & 63);
		if (sift_key_row_present((pt->flags & SIFT_CDCOL_IS_NULL) != 0, (pt->flags & SIFT_CDCOL_HAS_TAG) != 0 ? pt->tag : 0))
			p->mbits[m][j >> 6] |= bit;
		else
			p->mbits[m][j >> 6] &= ~bit;
	}
}

/* the rows [off, off + cnt) of column i of the granule: a text column of a bool leaf is decoded as its booleans, which make its texts */
static void
decode_col(SiftF2Pull *p, int i, uint16 off, uint16 cnt, const SiftVariantExcStream **exc)
{
	SiftVec    *v = &p->vec[i];
	SiftVec		b = *v;

	if (p->bools[i] == NULL)
	{
		sift_vec_cursor_decode(&p->cur[i], off, cnt, v, exc);
		return;
	}
	b.type = SVEC_I64;
	b.data = p->bools[i];
	sift_vec_cursor_decode(&p->cur[i], off, cnt, &b, exc);
	for (uint16 j = 0; j < cnt; j++)
		((SiftStrView *) v->data)[j] = b.all_valid || ((b.valid[j >> 6] >> (j & 63)) & 1) ? sift_f2_pull_bool_text(p->bools[i][j]) : (SiftStrView) {0};
	memset((SiftStrView *) v->data + cnt, 0, SIFT_VEC_PAD);
	b.type = v->type;
	b.data = v->data;
	*v = b;
}

static void
index_fill_slice(SiftF2Pull *p, uint16 off, uint16 cnt)
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

				sift_vec_cursor_open(&p->gv, p->spec.col[i].leaf, p->bools[i] != NULL ? SVEC_I64 : p->vec[i].type, &p->cur[i]);
				MemoryContextSwitchTo(old);
				p->cur_on[i] = true;
			}
			decode_col(p, i, off, cnt, &exc);
			if (exc != NULL)
				sift_f2_pull_unholdable(p);
			patch_col(p, i, off, cnt);
		}
	}
	for (int m = 0; m < p->spec.nmasks; m++)
	{
		if (p->is_slab)
			fill_slab_mask(p, m, off, cnt);
		else
		{
			fill_granule_mask(p, m, off, cnt);
			patch_mask(p, m, off, cnt);
		}
		p->masks[m] = (SiftVexprMaskIn) {.bits = p->mbits[m],.valid = NULL};
	}
}

static void
fill_elems(SiftF2Pull *p, int g, uint16 off, uint16 cnt)
{
	static const uint32 zero_offsets[2] = {0, 0};
	const PullGroup *gr = &p->grp[g];
	SiftF2ElemStream *es = &p->es[g];

	if (gr->has_layout)
		es->layout = (SiftArrayLayout) {.n_parents = cnt,.n_elems = gr->lay.n_elems,.block_elem_off = gr->lay.block_elem_off + off / 64,.lengths = gr->lay.lengths + off,.offsets = gr->lay.offsets + off};
	else
		es->layout = (SiftArrayLayout) {.n_parents = 0,.n_elems = 0,.block_elem_off = zero_offsets,.lengths = zero_offsets,.offsets = zero_offsets};
	es->n_elems = gr->n_elems;
	for (int c = 0; gr->has_layout && c < p->spec.group[g].nchild; c++)
	{
		es->child_data[c] = gr->child_data[c];
		es->child_valid[c] = gr->child_valid[c];
		es->child_type[c] = gr->child_type[c];
		es->child_dict[c] = gr->child_dict[c];
	}
	es->poison = p->poison + off / 64;
	es->doc_valid = gr->doc_valid + off / 64;
}

bool
sift_f2_pull_holds_payload(uint8 tag)
{
	return tag >= SIFT_COLTAG_STRING && tag <= SIFT_COLTAG_ARRAY;
}

static const char *
index_payload(SiftF2Pull *p, int i, uint32 at, uint32 *len)
{
	const int	c = p->spec.col[i].leaf;

	if (p->is_slab)
	{
		if (p->rb->tag == NULL || p->rb->tag[c] == NULL || p->rb->xpay[c] == NULL || !sift_f2_pull_holds_payload(p->rb->tag[c][at]))
			return NULL;
		*len = p->rb->xplen[c][at];
		return p->rb->xpay[c][at];
	}
	if (p->xtag[i] == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(p->gcx);

		p->xtag[i] = palloc(p->n);
		sift_granule_tags(&p->gv, c, p->xtag[i]);
		MemoryContextSwitchTo(old);
	}
	return sift_f2_pull_holds_payload(p->xtag[i][at]) ? sift_granule_exc_payload(&p->gv, c, (int) at, len) : NULL;
}

static const SiftF2PullHost index_host = {.advance = advance,.fill_slice = index_fill_slice,.payload = index_payload};

const char *
sift_f2_pull_payload(SiftF2Pull *p, int i, uint16 row, uint32 *len)
{
	const SiftVec *v = &p->vec[i];

	if (v->valid == NULL || ((v->valid[row >> 6] >> (row & 63)) & 1))
		return NULL;
	return p->host->payload(p, i, (uint32) p->slice_off + row, len);
}

const SiftF2ExtBatch *
sift_f2_pull_next(SiftF2Pull *p)
{
	uint16		cnt;
	uint16		off;

	while (p->off >= p->n)
	{
		CHECK_FOR_INTERRUPTS();
		if (!p->host->advance(p))
			return NULL;
	}
	off = (uint16) p->off;
	cnt = (uint16) Min(SIFT_VEC_LEN, p->n - p->off);
	p->off += cnt;
	p->slice_off = off;
	p->host->fill_slice(p, off, cnt);
	p->out = (SiftF2ExtBatch) {.n = cnt,.col = p->vec,.masks = p->masks,.tid = p->tids + off,.live = p->live + off / 64,.visible = p->host->visible};
	if (p->spec.ngroups > 0)
	{
		for (int g = 0; g < p->spec.ngroups; g++)
			fill_elems(p, g, off, cnt);
		p->out.elem = p->es;
	}
	return &p->out;
}
