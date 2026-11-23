/*
 * agg/gpu_f2_plan.c - the parts of a Format 2.0 relation that the device reads, for a sift_col index and a sift_table alike (both are a SiftF2Store, agg/gpu_v6.h).
 *
 * The plan lists the granules of the parts whose rows the snapshot sees without a test of each row (col/gpu.h): sealed inserters, and for an index whose heap decides visibility, every heap
 * block the part's addresses span all-visible. The rows the snapshot sees deleted, by the deletion vector of the part and by the pending deletes of its partition, become the mask of dead
 * rows of their granules (agg/gpu_dead.h), so a part that has deletes is read at the speed of one that has none. A part the device cannot read stays with the CPU, a part at a time: a level 0
 * part, a part whose heap blocks are not all visible, a part whose inserters are not settled, and a part that a column delta of its partition patches in a column the query reads. The CPU
 * scan then reads these and the slab of the partition, and its groups join those of the device. Nothing but the manifest, the zones, the directories and the addresses of the granules a
 * pending delete may name is read to plan; a granule's chunks are read when it is loaded, and not at all for a chunk the device holds.
 */
#include "postgres.h"

#include "access/visibilitymap.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/gpu_dead.h"
#include "agg/gpu_v6.h"
#include "col/f2/f2.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_gbuf.h"
#include "col/f2/l0_stage.h"
#include "col/gpu.h"
#include "core/cmp.h"
#include "table/f2/f2_col_delta.h"
#include "table/f2/f2_tbl.h"

#define ZONE_IN_SORT_MIN	8

/* The visibility map of the heap of the index, read for the blocks of a part when the part is asked about: one pinned page of it at a time */
typedef struct HeapVm
{
	Relation	heap;
	Buffer		vmbuf;
} HeapVm;

typedef struct PlanPart
{
	SiftF2PartPin *pin;
	uint32		part_id;
	uint64		part_gen;
	uint32		g0;				/* the plan's granule of the part's first */
	uint32		ng;
	uint32		phys[SIFT_VK_MAXCOLS];
	SiftZone   *zone[SIFT_VK_MAXCOLS];	/* ng zones of each column */
	uint64	  **pend;			/* per granule the rows a pending delete of the partition deletes for the snapshot, NULL for a granule none names */
} PlanPart;

/* What admitting the parts of one partition depends on */
typedef struct Admit
{
	const SiftGpuQuery *q;
	const SiftPartitionEntry *e;
	const SiftPendDelIndex *pdel;	/* the pending deletes of the partition the snapshot sees, NULL for none */
	HeapVm	   *vm;
	FullTransactionId xmin;
	uint64		att_bloom;		/* of the attributes the query reads, as the column deltas keep them */
} Admit;

typedef struct SiftGpuPlanPriv
{
	Relation	rel;
	SiftF2Store *store;
	Snapshot	snap;
	int			leaf[SIFT_VK_MAXCOLS];
	PlanPart   *part;
	uint32		npart;
	uint32	   *part_of;		/* per granule */
	uint64	   *gens;			/* of the parts the device reads, ascending */
	uint64		manifest;		/* the manifest generation the parts were read at */
	uint64		col_mask[4];	/* the leaves of the column slots */
	uint64		sig;			/* the leaves in slot order, then of each part its id, generation, granules and physical columns: what the descriptors depend on */
	int64	  **in_sorted;		/* per IN list of the query of 8 values or more: its values ascending (NULL for a shorter one) */
	MemoryContext plan_cx;		/* everything the plan holds but itself and this struct: freed in one step by sift_gpu_f2_free, whether the scan goes on the device or declines */
	MemoryContext load_cx;
	uint32		loaded_lo;
	uint32		loaded_hi;
	bool		released;
	MemoryContextCallback cb;
} Priv;

static BlockNumber
block_of(uint64 addr)
{
	ItemPointerData tid;

	sift_f2_tid_set(&tid, addr);
	return ItemPointerGetBlockNumber(&tid);
}

#define VM_MAPSIZE			(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))
#define VM_BLOCKS_PER_BYTE	4			/* 2 bits a heap block, the first of them all-visible */
#define VM_BLOCKS_PER_PAGE	((BlockNumber) VM_MAPSIZE * VM_BLOCKS_PER_BYTE)
#define VM_WORD_BLOCKS		32			/* the heap blocks of 8 bytes of the map */
#define VM_VISIBLE_BITS		UINT64CONST(0x5555555555555555)

/* Whether the heap blocks first .. last are all-visible: the pages of the map are read for the blocks asked, 32 blocks at a time, and the first block that is not ends it */
static bool
heap_vm_clean(HeapVm *vm, BlockNumber first, BlockNumber last)
{
	BlockNumber b = first;

	while (b <= last)
	{
		const BlockNumber page_end = Min((uint64) last, ((uint64) b / VM_BLOCKS_PER_PAGE + 1) * VM_BLOCKS_PER_PAGE - 1);
		const uint8 *map;

		if ((b & 0xFFFF) == 0)
			CHECK_FOR_INTERRUPTS();
		if ((visibilitymap_get_status(vm->heap, b, &vm->vmbuf) & VISIBILITYMAP_ALL_VISIBLE) == 0 || !BufferIsValid(vm->vmbuf))
			return false;
		map = (const uint8 *) PageGetContents(BufferGetPage(vm->vmbuf));
		for (; b <= page_end; b++)
		{
			const uint32 at = (uint32) (b % VM_BLOCKS_PER_PAGE);

			if (at % VM_WORD_BLOCKS == 0 && b + VM_WORD_BLOCKS - 1 <= page_end)
			{
				uint64		w;

				memcpy(&w, map + at / VM_BLOCKS_PER_BYTE, sizeof(w));
				if ((w & VM_VISIBLE_BITS) != VM_VISIBLE_BITS)
					return false;
				b += VM_WORD_BLOCKS - 1;
			}
			else if (((map[at / VM_BLOCKS_PER_BYTE] >> (2 * (at % VM_BLOCKS_PER_BYTE))) & VISIBILITYMAP_ALL_VISIBLE) == 0)
				return false;
		}
	}
	return true;
}

static bool
part_heap_clean(HeapVm *vm, const SiftPartDesc *d)
{
	return d->tid_max != 0 && heap_vm_clean(vm, block_of(d->tid_min), block_of(d->tid_max));
}

static bool
refuse(const char **why, const char *msg)
{
	*why = msg;
	return false;
}

static bool
columns_ok(const SiftGpuQuery *q, const SiftF2PartPin *pin, uint32 *phys, const char **why)
{
	const SiftF2ColMap *map = pin->col_map;

	for (int c = 0; c < q->ncols; c++)
	{
		int			l = q->leaf[c];

		if (!sift_gpu_leaf_ok(q->store->kinds[l], q->store->hints[l]))
			return refuse(why, "a column is not an integer, a decimal or a float the device decodes");
		phys[c] = l;
		if (map == NULL)
			continue;
		if (l >= map->cur_ncols || map->mode[l] != SIFT_CMAP_DIRECT || map->phys_col[l] < 0)
			return refuse(why, "the part was written before a column was added or changed");
		phys[c] = map->phys_col[l];
	}
	return true;
}

static bool
read_zones(const SiftGpuQuery *q, PlanPart *pp, const char **why)
{
	const SiftPart *part = pp->pin->part;
	const SiftSecDirEntry *zs = sift_part_section(part, SEC_ZONES);

	if (zs == NULL || zs->len != (uint64) part->hdr.ncols * pp->ng * sizeof(SiftZone))
		return refuse(why, "the part keeps no zones");
	for (int c = 0; c < q->ncols; c++)
	{
		pp->zone[c] = palloc(pp->ng * sizeof(SiftZone));
		sift_part_read_section_bytes(q->rel, part, zs, (uint64) pp->phys[c] * pp->ng * sizeof(SiftZone), (uint64) pp->ng * sizeof(SiftZone), pp->zone[c]);
	}
	return true;
}

static bool
inserters_settled(const SiftPartDesc *d, const SiftF2PartPin *pin, FullTransactionId xmin, const char **why)
{
	for (uint32 g = 0; g < pin->part->hdr.ngranules; g++)
		if (!sift_gpu_granule_eligible(d, &pin->gdir[g], xmin))
			return refuse(why, "the inserters of some rows of the part are not settled for the snapshot");
	return true;
}

/* The rows of each granule that a pending delete the snapshot sees names, a mask for the granules it may name (the bloom of the granule's addresses tells most that it does not) */
static uint64 **
pending_masks(Relation rel, const SiftF2PartPin *pin, const SiftPendDelIndex *px)
{
	uint32		ng = pin->part->hdr.ngranules;
	uint64	  **masks = palloc0(Max(ng, 1) * sizeof(uint64 *));

	for (uint32 g = 0; g < ng; g++)
	{
		uint64	   *mask,
				   *tids;

		if (!sift_pdel_idx_may_hit_granule(px, &pin->tidx[g]))
			continue;
		tids = sift_part_read_tids(rel, pin->part, pin->gdir, g);
		mask = palloc0(SIFT_DV_MASK_WORDS * sizeof(uint64));
		if (sift_gpu_dead_add_tids(mask, tids, pin->gdir[g].nrows, px) > 0)
			masks[g] = mask;
		else
			pfree(mask);
		pfree(tids);
	}
	return masks;
}

/* The column deltas of the partition patch the part in a column the query reads: its chunks do not hold what the snapshot sees */
static bool
col_delta_patches(const Admit *a, const SiftF2PartPin *pin)
{
	SiftTblVis	vis;
	SiftColDeltaPartMap *map;
	bool		patched;

	if ((a->e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0)
		return false;
	sift_tbl_f2_vis_begin(&vis, a->q->rel, a->q->snap);
	map = sift_col_delta_load_part_map(a->q->rel, a->e, pin->part_gen, pin->part->hdr.ngranules, &vis, 0);
	patched = sift_col_delta_part_touches(map, a->att_bloom);
	sift_col_delta_part_map_free(map);
	return patched;
}

/* The part as the device reads it, or false and why; the pin it took is given back then */
static bool
admit(const Admit *a, const SiftPartDesc *d, PlanPart *pp, const char **why)
{
	SiftVisClass cls;
	SiftF2PartPin *pin;
	const SiftGpuQuery *q = a->q;

	if (!sift_gpu_part_eligible(d, a->xmin, &cls, why))
		return false;
	if (a->vm != NULL && !part_heap_clean(a->vm, d))
		return refuse(why, "the heap blocks of the part are not all visible");
	pin = sift_f2_pin_open(q->rel, q->store, d, (uint32) a->e->part_id);
	pp->pin = pin;
	pp->part_id = (uint32) a->e->part_id;
	pp->part_gen = d->part_gen;
	pp->ng = pin->part->hdr.ngranules;
	if (col_delta_patches(a, pin))
		refuse(why, "the part has column deltas on a column the query reads");
	else if (columns_ok(q, pin, pp->phys, why) && read_zones(q, pp, why) && (cls != SIFT_VIS_PER_ROW || inserters_settled(d, pin, a->xmin, why)))
	{
		pp->pend = a->pdel != NULL ? pending_masks(q->rel, pin, a->pdel) : NULL;
		return true;
	}
	sift_f2_pin_release(pin);
	return false;
}

static uint32
width_of(const SiftZone *z, SiftCellKind kind)
{
	uint64		range;

	if (kind == SIFT_CELL_F8)
		return 8;
	if ((z->flags & SIFT_ZONE_NONE) || z->min > z->max)
		return 0;
	range = (uint64) z->max - (uint64) z->min;
	return range == 0 ? 0 : range <= 0xFF ? 1 : range <= 0xFFFF ? 2 : range <= 0xFFFFFFFFu ? 4 : 8;
}

static void
fold_zone(SiftGpuPlan *p, int c, const SiftZone *z, uint32 nrows, SiftCellKind kind, bool *seen)
{
	if (kind == SIFT_CELL_F8)
	{
		p->zrange[c] = false;
		p->znulls[c] = true;
		return;
	}
	p->znulls[c] |= z->nnull > 0;
	if (z->nnull >= nrows)
		return;
	if ((z->flags & SIFT_ZONE_NONE) || z->min > z->max)
	{
		p->zrange[c] = false;
		return;
	}
	p->zmin[c] = seen[c] ? Min(p->zmin[c], z->min) : z->min;
	p->zmax[c] = seen[c] ? Max(p->zmax[c], z->max) : z->max;
	seen[c] = true;
}

/* the first index of the ascending vals[0 .. n-1] holding a value of at least v */
static uint32
lower_bound_i64(const int64 *vals, uint32 n, int64 v)
{
	uint32		lo = 0,
				hi = n;

	while (lo < hi)
	{
		const uint32 mid = lo + (hi - lo) / 2;

		if (vals[mid] < v)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static bool
zone_rules_out(const SiftGpuQuery *q, const Priv *pv, const PlanPart *pp, uint32 gl)
{
	for (int i = 0; i < q->nzone; i++)
	{
		const SiftGpuZoneCmp *t = &q->zone[i];
		const SiftZone *z = &pp->zone[t->col][gl];

		if ((z->flags & SIFT_ZONE_NONE) || z->min > z->max)
			return true;
		if ((t->op == SCMP_EQ && (t->val < z->min || t->val > z->max)) || (t->op == SCMP_LT && z->min >= t->val) ||
			(t->op == SCMP_LE && z->min > t->val) || (t->op == SCMP_GT && z->max <= t->val) || (t->op == SCMP_GE && z->max < t->val))
			return true;
	}
	for (int i = 0; i < q->nin; i++)
	{
		const SiftGpuIn *in = &q->in[i];
		const SiftZone *z = &pp->zone[in->col][gl];
		bool		any = false;

		if (in->negate)
			continue;
		if (pv->in_sorted[i] != NULL)
		{
			const uint32 at = lower_bound_i64(pv->in_sorted[i], in->n, z->min);

			any = at < in->n && pv->in_sorted[i][at] <= z->max;
		}
		else
			for (uint32 j = 0; j < in->n && !any; j++)
				any = in->vals[j] >= z->min && in->vals[j] <= z->max;
		if (!any)
			return true;
	}
	return false;
}

static void
fill_granule(SiftGpuPlan *p, const SiftGpuQuery *q, const PlanPart *pp, uint32 gl, bool *seen)
{
	Priv	   *pv = p->priv;
	uint32		i = pp->g0 + gl;
	const SiftGranuleDesc *gd = &pp->pin->gdir[gl];
	const SiftDvGran *dg = pp->pin->dv != NULL ? sift_dv_granule(pp->pin->dv, gl) : NULL;
	SiftVkGranule *g = &p->gran[i];
	const uint64 *pend = pp->pend != NULL ? pp->pend[gl] : NULL;
	uint32		ndead = 0;

	g->id = i;
	g->nrows = gd->nrows;
	if (dg != NULL || pend != NULL)
	{
		uint64	   *mask = palloc0(SIFT_DV_MASK_WORDS * sizeof(uint64));

		if (dg != NULL)
			(void) sift_dv_mask64(dg, q->snap, mask);
		for (int w = 0; pend != NULL && w < SIFT_DV_MASK_WORDS; w++)
			mask[w] |= pend[w];
		ndead = sift_gpu_dead_count(mask);
		if (ndead > 0)
			g->dead = (const uint32 *) mask;
		else
			pfree(mask);
	}
	p->dead_rows += ndead;
	pv->part_of[i] = (uint32) (pp - pv->part);
	p->rows += gd->nrows - ndead;
	if (!zone_rules_out(q, pv, pp, gl))
		p->scan_rows += gd->nrows - ndead;
	p->enc_bytes += (uint64) gd->data_len * q->ncols / Max(pp->pin->part->hdr.ncols, 1);
	for (int c = 0; c < q->ncols; c++)
	{
		SiftCellKind kind = q->store->kinds[q->leaf[c]];
		const SiftZone *z = &pp->zone[c][gl];

		g->col[c] = &p->chunk[(uint64) i * q->ncols + c];
		p->chunk[(uint64) i * q->ncols + c].key = sift_gpu_chunk_key(&q->rel->rd_locator, pp->part_id, pp->part_gen, pp->phys[c], gl);
		p->est_chunk[i] += width_of(z, kind) * gd->nrows + gd->nrows / 8;
		fold_zone(p, c, z, gd->nrows, kind, seen);
	}
}

static int
gen_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

static void
release_pins(Priv *pv)
{
	if (pv->released)
		return;
	pv->released = true;
	for (uint32 i = 0; i < pv->npart; i++)
		sift_f2_pin_release(pv->part[i].pin);
}

static void
release_cb(void *arg)
{
	release_pins(arg);
}

static void
fold_signature(Priv *pv, int ncols)
{
	uint64		sig = 0;

	for (int c = 0; c < ncols; c++)
	{
		pv->col_mask[(pv->leaf[c] >> 6) & 3] |= (uint64) 1 << (pv->leaf[c] & 63);
		sig = hash_combine64(sig, (uint64) pv->leaf[c]);
	}
	for (uint32 i = 0; i < pv->npart; i++)
	{
		const PlanPart *pp = &pv->part[i];

		sig = hash_combine64(hash_combine64(hash_combine64(sig, pp->part_id), pp->part_gen), pp->ng);
		for (int c = 0; c < ncols; c++)
			sig = hash_combine64(sig, pp->phys[c]);
	}
	pv->sig = sig;
}

SiftVkTabKey
sift_gpu_f2_tab_key(const SiftGpuPlan *p, uint32 g0, uint32 g1)
{
	const Priv *pv = p->priv;
	SiftVkTabKey key = {.spc = pv->rel->rd_locator.spcOid,.db = pv->rel->rd_locator.dbOid,.rel = pv->rel->rd_locator.relNumber,.manifest_gen = pv->manifest,
	.granules = hash_combine64(hash_combine64(pv->sig, g0), g1)};

	memcpy(key.col_mask, pv->col_mask, sizeof(key.col_mask));
	return key;
}

static void
build_arrays(SiftGpuPlan *p, const SiftGpuQuery *q)
{
	Priv	   *pv = p->priv;
	uint32		n = 0;

	for (uint32 i = 0; i < pv->npart; i++)
	{
		pv->part[i].g0 = n;
		n += pv->part[i].ng;
	}
	p->ngran = n;
	p->gran = palloc0(Max(n, 1) * sizeof(SiftVkGranule));
	p->chunk = palloc0((Size) Max(n, 1) * q->ncols * sizeof(SiftVkChunk));
	p->est_chunk = palloc0(Max(n, 1) * sizeof(uint32));
	pv->part_of = palloc(Max(n, 1) * sizeof(uint32));
	pv->gens = palloc(Max(pv->npart, 1) * sizeof(uint64));
}

/* MEM[MEM-VK-12]: gpu_f2_plan heap_vm (the visibility map is read page by page as the granules ask for it, never as a bitmap of the heap: was up to 4MiB built before the plan was known to be eligible) */
/* The pending deletes of the partition the snapshot sees; a sift_table folds its own and a sift_col index offers the addresses */
static SiftPendDelIndex *
pending_index(const SiftGpuQuery *q, const SiftPartitionEntry *e)
{
	uint64	   *dead;
	SiftPendDelIndex *px;
	int			n;

	if (e->pend_del_blk == InvalidBlockNumber)
		return NULL;
	if (q->heap == NULL)
		return sift_tbl_f2_pending_index(q->rel, e, q->snap);
	dead = sift_f2_pending_dead(q->rel, e, q->snap, &n);
	/* MEM[MEM-VK-11]: pending_index dead array (the addresses are copied into the index, then freed; was kept until the plan's context went) */
	px = n > 0 ? sift_pdel_idx_build(dead, n) : NULL;
	if (dead != NULL)
		pfree(dead);
	return px;
}

static void
plan_parts(SiftGpuPlan *p, const Admit *proto, const SiftPlanSnapshot *view)
{
	Priv	   *pv = p->priv;
	uint32		cap = 16;

	pv->part = palloc(cap * sizeof(PlanPart));
	pv->manifest = view->manifest_version;
	for (int i = 0; i < view->npartitions; i++)
	{
		SiftPendDelIndex *px = pending_index(proto->q, &view->partitions[i]);
		Admit		a = *proto;

		a.e = &view->partitions[i];
		a.pdel = px;
		for (int j = 0; j < view->partlists[i].nparts; j++)
		{
			const SiftPartDesc *d = &view->partlists[i].parts[j];
			const char *why = NULL;

			p->nparts++;
			if (d->nrows == 0)
				continue;
			if (pv->npart == cap)
				pv->part = repalloc(pv->part, (cap *= 2) * sizeof(PlanPart));
			pv->part[pv->npart] = (PlanPart) {0};
			if (admit(&a, d, &pv->part[pv->npart], &why))
				pv->npart++;
			else
				strlcpy(p->why, why, sizeof(p->why));
		}
		if (px != NULL)
			sift_pdel_idx_free(px);
	}
	p->ndevice = pv->npart;
}

static uint64
att_bloom(const SiftGpuQuery *q)
{
	uint64		bloom = 0;

	for (int c = 0; c < q->ncols; c++)
		bloom |= sift_cd_att_bit(q->store->leaf_attno[q->leaf[c]]);
	return bloom;
}

SiftGpuPlan *
sift_gpu_f2_plan(const SiftGpuQuery *q, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftGpuPlan *p = palloc0(sizeof(SiftGpuPlan));
	Priv	   *pv = palloc0(sizeof(Priv));
	HeapVm		vm = {.vmbuf = InvalidBuffer};
	bool		vet = false;
	bool		seen[SIFT_VK_MAXCOLS] = {false};

	pv->plan_cx = sift_mem_allocset_create(mcxt, "sift gpu plan");
	MemoryContextSwitchTo(pv->plan_cx);
	p->priv = pv;
	p->ncols = q->ncols;
	pv->rel = q->rel;
	pv->store = q->store;
	pv->snap = q->snap;
	memcpy(pv->leaf, q->leaf, sizeof(pv->leaf));
	pv->in_sorted = palloc0(Max(q->nin, 1) * sizeof(int64 *));
	for (int i = 0; i < q->nin; i++)
		if (q->in[i].n >= ZONE_IN_SORT_MIN)
		{
			pv->in_sorted[i] = palloc(q->in[i].n * sizeof(int64));
			memcpy(pv->in_sorted[i], q->in[i].vals, q->in[i].n * sizeof(int64));
			sift_sort_i64(pv->in_sorted[i], q->in[i].n);
		}
	pv->load_cx = sift_mem_allocset_create(pv->plan_cx, "sift gpu chunks");
	pv->cb = (MemoryContextCallback) {.func = release_cb,.arg = pv};
	MemoryContextRegisterResetCallback(mcxt, &pv->cb);
	for (int c = 0; c < q->ncols; c++)
		p->zrange[c] = true;
	if (sift_stage_has_pending(q->rel))
		strlcpy(p->why, "this transaction has rows or deletes of its own staged", sizeof(p->why));
	else
	{
		if (q->store->host->needs_heap_vet != NULL)
		{
			SiftColMetaPageData meta = sift_col_meta_read(q->rel);

			vet = q->store->host->needs_heap_vet(q->store, &meta);
		}
		if (vet && q->heap == NULL)
			strlcpy(p->why, "the heap of the index is not at hand", sizeof(p->why));
		else
		{
			if (vet)
				vm = (HeapVm) {.heap = q->heap,.vmbuf = InvalidBuffer};
			plan_parts(p, &(Admit) {.q = q,.vm = vet ? &vm : NULL,.xmin = sift_vis_snap_xmin(q->snap),.att_bloom = att_bloom(q)}, sift_plan_cache_get(q->rel, mcxt));
		}
	}
	build_arrays(p, q);
	for (uint32 i = 0; i < pv->npart; i++)
	{
		pv->gens[i] = pv->part[i].part_gen;
		for (uint32 g = 0; g < pv->part[i].ng; g++)
			fill_granule(p, q, &pv->part[i], g, seen);
	}
	qsort(pv->gens, pv->npart, sizeof(uint64), gen_cmp);
	fold_signature(pv, q->ncols);
	pv->loaded_lo = p->ngran;
	if (BufferIsValid(vm.vmbuf))
		ReleaseBuffer(vm.vmbuf);
	MemoryContextSwitchTo(old);
	return p;
}

void
sift_gpu_f2_keys(const SiftGpuPlan *p, SiftVkKey *out)
{
	for (uint64 i = 0; i < (uint64) p->ngran * p->ncols; i++)
		out[i] = p->chunk[i].key;
}

void
sift_gpu_f2_locate(const SiftGpuPlan *p, uint32 i, uint32 *part_id, uint64 *part_gen, uint32 *granule)
{
	const PlanPart *pp = &p->priv->part[p->priv->part_of[i]];

	*part_id = pp->part_id;
	*part_gen = pp->part_gen;
	*granule = i - pp->g0;
}

bool
sift_gpu_f2_covers(const SiftGpuPlan *p, uint64 part_gen)
{
	return bsearch(&part_gen, p->priv->gens, p->priv->npart, sizeof(uint64), gen_cmp) != NULL;
}

static void
clear_chunk(SiftVkChunk *ch)
{
	SiftVkKey	key = ch->key;

	memset(ch, 0, sizeof(*ch));
	ch->key = key;
}

/* The chunks of granule g that the device lacks, read and handed over as stored; adds their bytes to *bytes */
static bool
load_granule(SiftGpuPlan *p, uint32 g, const uint8 *resident, uint64 *bytes, const char **why)
{
	Priv	   *pv = p->priv;
	const PlanPart *pp = &pv->part[pv->part_of[g]];
	uint32		gl = g - pp->g0;
	int			chunks[SIFT_VK_MAXCOLS],
				col[SIFT_VK_MAXCOLS],
				n = 0;
	SiftLazyGranule lg;
	SiftGranuleView v;

	for (int c = 0; c < (int) p->ncols; c++)
	{
		SiftVkChunk *ch = &p->chunk[(uint64) g * p->ncols + c];

		clear_chunk(ch);
		if (resident == NULL || !resident[(uint64) g * p->ncols + c])
		{
			chunks[n] = pp->phys[c] + 1;
			col[n++] = c;
		}
	}
	if (n == 0)
		return true;
	sift_lazy_granule_open(pv->rel, pp->pin->part, gl, &lg);
	if (lg.gd.nrows != p->gran[g].nrows)
		return refuse(why, "a granule of the part is not the one planned");
	sift_lazy_granule_load(pv->rel, &lg, chunks, n);
	v = sift_granule_open_part(pv->rel, pp->pin->part, lg.rec, lg.gd.data_len, &lg.gd);
	for (int k = 0; k < n; k++)
	{
		SiftVkChunk *ch = &p->chunk[(uint64) g * p->ncols + col[k]];

		if (!sift_gpu_chunk_fill(&v, (int) pp->phys[col[k]], pv->store->kinds[pv->leaf[col[k]]], &pp->zone[col[k]][gl], ch, why))
			return false;
		*bytes += ch->len;
	}
	return true;
}

bool
sift_gpu_f2_load(SiftGpuPlan *p, uint32 g0, uint32 gmax, uint64 max_bytes, const uint8 *resident, uint32 *g1, const char **why)
{
	Priv	   *pv = p->priv;
	MemoryContext old = MemoryContextSwitchTo(pv->load_cx);
	uint64		bytes = 0;
	uint32		g = g0;
	bool		ok = true;

	pv->loaded_lo = Min(pv->loaded_lo, g0);
	for (; g < gmax && ok && (g == g0 || bytes < max_bytes); g++)
		ok = load_granule(p, g, resident, &bytes, why);
	pv->loaded_hi = Max(pv->loaded_hi, g);
	*g1 = g;
	MemoryContextSwitchTo(old);
	return ok;
}

void
sift_gpu_f2_unload(SiftGpuPlan *p)
{
	Priv	   *pv = p->priv;

	for (uint64 i = (uint64) pv->loaded_lo * p->ncols; i < (uint64) pv->loaded_hi * p->ncols; i++)
		p->chunk[i].bytes = NULL;
	MemoryContextReset(pv->load_cx);
	pv->loaded_lo = p->ngran;
	pv->loaded_hi = 0;
}

/* MEM[MEM-VK-11]: gpu plan teardown (plan_cx deleted with SiftGpuPlan.gran/chunk/est_chunk, Priv.part and its zones and pending indexes, the IN lists and the chunk context: nothing of the plan outlives a decline) */
void
sift_gpu_f2_free(SiftGpuPlan *p)
{
	Priv	   *pv = p->priv;

	release_pins(pv);
	MemoryContextDelete(pv->plan_cx);
	pv->plan_cx = pv->load_cx = NULL;
	pv->part = NULL;
	pv->part_of = NULL;
	pv->gens = NULL;
	pv->in_sorted = NULL;
	p->gran = NULL;
	p->chunk = NULL;
	p->est_chunk = NULL;
}
