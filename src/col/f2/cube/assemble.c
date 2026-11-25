/*
 * col/f2/cube/assemble.c - the second half of a refresh (refresh.c decides what goes): builds the nodes that go again, joins them with the nodes
 * that stay, and writes the new object.
 *
 * A key of the plans keeps every level-1 node of the old object that the plan does not drop, as bytes. The granules of the dropped nodes, and of
 * the members that came in, are built again by the same build as the first one over a pair map of those keys alone, limited to the granules of
 * their masks. The key's root is the sum of the cubes of its nodes (old and new), its month map the old one and the new one with the ordinals of
 * the granules moved to where their nodes now are, and its ring the entries of the old ring that are not on a dropped granule with the entries of
 * the new one, as far down as both are complete (a ring that is not full says so: SIFT_L2_TOPK_PARTIAL, and the read goes on in the month map).
 * Every other key is copied as bytes. A level-0 slice is named by exactly one node, so the slices that die are those of the dropped nodes; their
 * bytes leave the pool and their numbers stay, until the dead are more than the live: then the slices are numbered again and the refs of every
 * node that has one follow.
 */
/* check-memory: allow-allocs the arrays follow the keys, level-1 nodes and slices of one summary, which the refresh holds by design */
#include "core/host.h"

#include "core/cmp.h"
#include "col/f2/cube/int.h"
#include "col/f2/cube/wire.h"

#define NO_ORD	UINT32_MAX

static int
u32_cmp(const void *a, const void *b)
{
	return sift_cmp_u32(a, b);
}

static bool
in_sorted(const uint32 *v, uint32 n, uint32 x)
{
	return n > 0 && bsearch(&x, v, n, sizeof(uint32), u32_cmp) != NULL;
}

void
sift_plan_drops(const SiftScopeMeta *old, SiftKeyPlan *p)
{
	uint32		cap = 0;

	p->drop = NULL;
	p->dead_slices = NULL;
	p->ndrop = p->ndead = 0;
	if (p->old == NULL)
		return;
	for (uint32 i = 0; i < p->old->l1_count; i++)
	{
		const SiftScopeL1Node *nd = &old->nodes[p->old->l1_first + i];
		SiftScopeGranRef *refs;

		if (!p->dirty_node[i])
			continue;
		refs = palloc(sizeof(*refs) * nd->nrefs);
		sift_scope_node_refs(old, nd, refs);
		if (p->ndrop + nd->nrefs > cap)
		{
			cap = Max(cap * 2, p->ndrop + nd->nrefs);
			p->drop = p->drop == NULL ? palloc(sizeof(uint32) * cap) : repalloc(p->drop, sizeof(uint32) * cap);
			p->dead_slices = p->dead_slices == NULL ? palloc(sizeof(uint32) * cap) : repalloc(p->dead_slices, sizeof(uint32) * cap);
		}
		for (uint32 q = 0; q < nd->nrefs; q++)
		{
			p->drop[p->ndrop++] = refs[q].global_granule_idx;
			if (SIFT_SCOPE_REF_SLICED(refs[q].slice_id))
				p->dead_slices[p->ndead++] = refs[q].slice_id;
		}
		pfree(refs);
	}
	if (p->ndrop > 0)
		qsort(p->drop, p->ndrop, sizeof(uint32), u32_cmp);
}

/* ---------------------------------------------------------------------------
 * 1. A key copied
 * ------------------------------------------------------------------------- */

static void
rec_from_meta(const SiftScopeMeta *m, const SiftScopeL2Root *root, SiftKeyRec *rec)
{
	memset(rec, 0, sizeof(*rec));
	rec->root = *root;
	rec->nnodes = root->l1_count;
	rec->nodes = palloc(sizeof(SiftKeyNodeRec) * Max(root->l1_count, 1));
	for (uint32 i = 0; i < root->l1_count; i++)
	{
		const SiftScopeL1Node *nd = &m->nodes[root->l1_first + i];

		rec->nodes[i].node = *nd;
		rec->nodes[i].block = m->l1.p + nd->ref_off;
		rec->nodes[i].block_len = sift_block_end(nd) - nd->ref_off;
		rec->nodes[i].block_off = nd->ref_off;
		rec->nodes[i].from = m;
	}
	SIFT_INVARIANT(!m->topk_packed, "scope refresh: the rings of the summary were not unpacked");
	if (root->flags & SIFT_L2_HAS_TOPK)
		rec->topk = m->topk.p + root->topk_off;
	rec->topk_wide = (m->hdr.flags & SIFT_SMETA_TOPK_WIDE) != 0;
	if (root->flags & SIFT_L2_HAS_MONTHMAP)
		rec->monthmap = m->monthmap.p + root->monthmap_off;
	if (!(root->flags & SIFT_L2_SINGLE_L1))
	{
		uint32		end = root->nmonths > 0 ? root->month_dir_off + root->nmonths * (uint32) sizeof(SiftScopeMonthCell) : root->all_cube_off + root->all_cube_len;

		rec->l2block = m->l2.p + root->all_cube_off;
		rec->l2block_len = end - root->all_cube_off;
		rec->l2block_off = root->all_cube_off;
	}
}

/* ---------------------------------------------------------------------------
 * 2. A key joined: its nodes that stay and its nodes that were built again
 * ------------------------------------------------------------------------- */

typedef struct NodeSrc
{
	const SiftScopeMeta *meta;
	const SiftScopeL1Node *nd;
	uint32		origin;			/* 0: the old object, 1: the rebuilt one */
	uint32		idx;			/* the node's place among those of its key in its own object */
} NodeSrc;

static int
node_src_cmp(const void *a, const void *b)
{
	const NodeSrc *x = a,
			   *y = b;

	if (x->nd->member_lo != y->nd->member_lo)
		return x->nd->member_lo < y->nd->member_lo ? -1 : 1;
	if (x->origin != y->origin)
		return x->origin < y->origin ? -1 : 1;
	return x->idx < y->idx ? -1 : x->idx > y->idx;
}

/* The sum of the cubes of the nodes: all time, and each month */
static void
nodes_cubes(const SiftScopeBuildSpec *spec, const NodeSrc *ns, uint32 n, SiftScopeCubeAcc *all, SiftScopeMonthTable *months)
{
	sift_scube_init(all, spec->shape);
	sift_smonth_init(months);
	for (uint32 i = 0; i < n; i++)
	{
		const SiftScopeL1Node *nd = ns[i].nd;
		SiftScopeCubeAcc c;

		SIFT_INVARIANT(sift_scope_node_cube(ns[i].meta, nd, SIFT_SCOPE_NO_MONTH, &c), "scope refresh: a node whose cube does not read");
		sift_scube_merge(all, &c, NULL);
		sift_scube_free(&c);
		for (uint32 j = 0; j < nd->nmonths; j++)
		{
			SiftScopeMonthCell cell;

			memcpy(&cell, ns[i].meta->l1.p + nd->month_dir_off + (Size) j * sizeof(cell), sizeof(cell));
			SIFT_INVARIANT(sift_scope_node_cube(ns[i].meta, nd, cell.month_id, &c), "scope refresh: a month cube of a node that does not read");
			sift_smonth_merge_cube(months, spec->shape, cell.month_id, &c, NULL);
			sift_scube_free(&c);
		}
	}
}

/* Ring ring of a joined key: the entries of both rings as far down as both are complete; true when it stopped short of the largest rows */
static bool
join_ring(const SiftScopeMeta *old, const SiftScopeL2Root *oroot, const SiftScopeMeta *fresh, const SiftScopeL2Root *froot, const SiftKeyPlan *plan, uint32 ring, SiftTopKHeap *h)
{
	SiftScopeNewest cand[2 * SIFT_SCOPE_TOPK_CAP],
				slot[SIFT_SCOPE_TOPK_CAP],
				limit = {0};
	uint32		n = 0;
	bool		limited = false,
				none_valid = false;

	if (oroot != NULL)
	{
		uint32		k = sift_scope_topk_get_ring(old, oroot, ring, slot);

		if (k == SIFT_SCOPE_TOPK_CAP || (sift_scope_ring_present(old, oroot, ring) && sift_scope_ring_partial(oroot, ring)))
		{
			limited = true;
			none_valid = k == 0;
			limit = slot[k > 0 ? k - 1 : 0];
		}
		for (uint32 i = 0; i < k; i++)
			if (!in_sorted(plan->drop, plan->ndrop, slot[i].gidx))
				cand[n++] = slot[i];
	}
	if (froot != NULL)
	{
		uint32		k = sift_scope_topk_get_ring(fresh, froot, ring, slot);

		if (k == SIFT_SCOPE_TOPK_CAP || (k > 0 && sift_scope_ring_present(fresh, froot, ring) && sift_scope_ring_partial(froot, ring)))
		{
			if (!limited || sift_newest_cmp(&slot[k - 1], &limit) > 0)
				limit = slot[k - 1];
			limited = true;
		}
		for (uint32 i = 0; i < k; i++)
			cand[n++] = slot[i];
	}
	for (uint32 i = 0; i < n && !none_valid; i++)
		if (!limited || sift_newest_cmp(&cand[i], &limit) >= 0)
			sift_topk_offer(h, cand[i].time, cand[i].gidx, cand[i].row_ord);
	return limited && h->n < SIFT_SCOPE_TOPK_CAP;
}

/* The rings of a joined key: a key whose rings are all empty and complete has none; with extra rings, an empty one that stopped short says so */
static void
join_rings(const SiftScopeMeta *old, const SiftScopeL2Root *oroot, const SiftScopeMeta *fresh, const SiftScopeL2Root *froot, const SiftKeyPlan *plan, bool wide, SiftKeyRec *rec)
{
	SiftTopKHeap h[SIFT_SCOPE_TOPK_RINGS_CAP] = {{0}};
	uint32		nrings = old->ntopk_rings,
				partial = 0;
	bool		any = false;
	StringInfoData slot;

	for (uint32 r = 0; r < nrings; r++)
	{
		bool		part = join_ring(old, oroot, fresh, froot, plan, r, &h[r]);

		partial |= (uint32) part << r;
		any |= h[r].n > 0 || (nrings > 1 && part);
	}
	if (any)
	{
		initStringInfo(&slot);
		sift_rings_write(h, nrings, partial, wide, &slot, &rec->root);
		rec->topk = slot.data;
		rec->topk_wide = wide;
	}
	for (uint32 r = 0; r < nrings; r++)
		sift_topk_release(&h[r]);
}

static void
map_months(const SiftScopeMeta *m, const SiftScopeL2Root *r, const uint32 *map, uint32 nmap, SiftMonthMapAcc *acc)
{
	SiftScopeMonthBucket *bk;
	uint32	   *ords;
	uint32		nb;

	if (r == NULL || sift_scope_monthmap_ngranules(m, r) == 0)
		return;
	nb = sift_scope_monthmap_nmonths(m, r);
	bk = palloc(sizeof(*bk) * nb);
	ords = palloc(sizeof(uint32) * Max(nmap, 1));
	nb = sift_scope_monthmap_buckets(m, r, bk, nb);
	for (uint32 i = 0; i < nb; i++)
	{
		uint32		ng = sift_scope_monthmap_bucket_granules(m, r, &bk[i], ords, nmap);

		for (uint32 g = 0; g < ng; g++)
			if (ords[g] < nmap && map[ords[g]] != NO_ORD)
				sift_mmap_add(acc, bk[i].month_id, map[ords[g]]);
	}
	pfree(bk);
	pfree(ords);
}

/* The month map of a joined key: the granules of its nodes, where their nodes are now */
static void
join_monthmap(const SiftScopeMeta *old, const SiftScopeL2Root *oroot, const SiftScopeMeta *fresh, const SiftScopeL2Root *froot, const NodeSrc *ns, uint32 n, SiftKeyRec *rec)
{
	uint32		nold = oroot == NULL ? 0 : sift_scope_key_refs(old, oroot, NULL),
				nfresh = froot == NULL ? 0 : sift_scope_key_refs(fresh, froot, NULL),
				total = 0;
	uint32	   *omap = palloc(sizeof(uint32) * Max(nold, 1)),
			   *fmap = palloc(sizeof(uint32) * Max(nfresh, 1)),
			   *obase = palloc(sizeof(uint32) * Max(oroot == NULL ? 0 : oroot->l1_count, 1)),
			   *fbase = palloc(sizeof(uint32) * Max(froot == NULL ? 0 : froot->l1_count, 1));
	SiftMonthMapAcc acc = {0};
	StringInfoData mm;

	memset(omap, 0xFF, sizeof(uint32) * Max(nold, 1));
	memset(fmap, 0xFF, sizeof(uint32) * Max(nfresh, 1));
	for (uint32 i = 0, at = 0; oroot != NULL && i < oroot->l1_count; i++)
	{
		obase[i] = at;
		at += old->nodes[oroot->l1_first + i].nrefs;
	}
	for (uint32 i = 0, at = 0; froot != NULL && i < froot->l1_count; i++)
	{
		fbase[i] = at;
		at += fresh->nodes[froot->l1_first + i].nrefs;
	}
	for (uint32 i = 0; i < n; i++)
	{
		uint32	   *map = ns[i].origin == 0 ? omap : fmap;
		uint32		from = ns[i].origin == 0 ? obase[ns[i].idx] : fbase[ns[i].idx];

		for (uint32 q = 0; q < ns[i].nd->nrefs; q++)
			map[from + q] = total + q;
		total += ns[i].nd->nrefs;
	}
	map_months(old, oroot, omap, nold, &acc);
	map_months(fresh, froot, fmap, nfresh, &acc);
	if (acc.n > 0 && total > 1)
	{
		for (uint32 i = 0; i < acc.n; i++)
			qsort(acc.e[i].ord, acc.e[i].n, sizeof(uint32), u32_cmp);
		initStringInfo(&mm);
		sift_mmap_write(&acc, total, NULL, &mm);
		rec->monthmap = mm.data;
		rec->root.monthmap_len = mm.len;
		rec->root.flags |= SIFT_L2_HAS_MONTHMAP;
	}
	sift_mmap_free(&acc);
	pfree(omap);
	pfree(fmap);
	pfree(obase);
	pfree(fbase);
}

/* The record of a key whose plan dropped some of its nodes: false when no node is left */
static bool
join_key(const SiftRefreshJob *job, const SiftScopeMeta *fresh, const SiftKeyPlan *plan, const SiftScopeL2Root *froot, bool wide, SiftKeyRec *rec)
{
	const SiftScopeMeta *old = job->old;
	const SiftScopeL2Root *oroot = plan->old;
	NodeSrc    *ns;
	uint32		n = 0;
	SiftScopeCubeAcc all;
	SiftScopeMonthTable months;
	uint64		members = 0;

	ns = palloc(sizeof(*ns) * ((oroot == NULL ? 0 : oroot->l1_count) + (froot == NULL ? 0 : froot->l1_count) + 1));
	for (uint32 i = 0; oroot != NULL && i < oroot->l1_count; i++)
		if (!plan->dirty_node[i])
			ns[n++] = (NodeSrc) {old, &old->nodes[oroot->l1_first + i], 0, i};
	for (uint32 i = 0; froot != NULL && i < froot->l1_count; i++)
		ns[n++] = (NodeSrc) {fresh, &fresh->nodes[froot->l1_first + i], 1, i};
	if (n == 0)
		return false;
	qsort(ns, n, sizeof(*ns), node_src_cmp);

	memset(rec, 0, sizeof(*rec));
	rec->nnodes = n;
	rec->nodes = palloc(sizeof(SiftKeyNodeRec) * n);
	for (uint32 i = 0; i < n; i++)
	{
		rec->nodes[i].node = *ns[i].nd;
		rec->nodes[i].block = ns[i].meta->l1.p + ns[i].nd->ref_off;
		rec->nodes[i].block_len = sift_block_end(ns[i].nd) - ns[i].nd->ref_off;
		rec->nodes[i].block_off = ns[i].nd->ref_off;
		rec->nodes[i].from = ns[i].meta;
		members += ns[i].nd->member_count;
	}
	nodes_cubes(&job->spec, ns, n, &all, &months);
	rec->root.scope_key = plan->key;
	rec->root.row_count = all.row_count;
	rec->root.member_count = (uint32) Min(members, UINT32_MAX);
	rec->root.time_min = all.has_time ? all.time_min : 0;
	rec->root.time_max = all.has_time ? all.time_max : 0;
	rec->root.key_epoch = job->spec.meta_epoch;
	if (n == 1)
	{
		rec->root.flags |= SIFT_L2_SINGLE_L1;
		rec->root.nmonths = ns[0].nd->nmonths;
		if (ns[0].nd->nmonths > 0)
			rec->root.flags |= SIFT_L2_HAS_MONTHS;
	}
	else
	{
		StringInfoData l2;

		initStringInfo(&l2);
		sift_l2_block(&l2, &rec->root, &all, &months);
		rec->l2block = l2.data;
		rec->l2block_len = l2.len;
		rec->l2block_off = 0;
		rec->root.all_cube_off = 0;
		if (months.n > 0)
			rec->root.flags |= SIFT_L2_HAS_MONTHS;
	}
	join_rings(old, oroot, fresh, froot, plan, wide, rec);
	join_monthmap(old, oroot, fresh, froot, ns, n, rec);
	sift_scube_free(&all);
	sift_smonth_free(&months);
	pfree(ns);
	return true;
}

/* ---------------------------------------------------------------------------
 * 3. Level-0 slices
 * ------------------------------------------------------------------------- */

/* The refs of a node written again with the numbers of its slices changed: the block is the new refs and the rest of the old block, the offsets in it follow */
static void
renumber_node(SiftKeyNodeRec *nr, const uint32 *rank, uint32 base, uint32 live_old)
{
	SiftScopeGranRef *refs = palloc(sizeof(*refs) * nr->node.nrefs);
	StringInfoData blk;
	uint32		old_len = nr->node.ref_len;
	int64		shift;
	char	   *copy;

	sift_scope_node_refs(nr->from, &nr->node, refs);
	for (uint32 i = 0; i < nr->node.nrefs; i++)
		if (SIFT_SCOPE_REF_SLICED(refs[i].slice_id))
			refs[i].slice_id = refs[i].slice_id < base ? rank[refs[i].slice_id] : live_old + (refs[i].slice_id - base);
	initStringInfo(&blk);
	sift_put_refs(&blk, refs, nr->node.nrefs);
	shift = (int64) blk.len - old_len;
	appendBinaryStringInfo(&blk, nr->block + old_len, nr->block_len - old_len);
	copy = blk.data;
	nr->node.ref_len = (uint32) (blk.len - (nr->block_len - old_len));
	if (nr->node.bloom_len > 0)
		nr->node.bloom_off = (uint32) ((int64) nr->node.bloom_off + shift);
	nr->node.all_cube_off = (uint32) ((int64) nr->node.all_cube_off + shift);
	if (nr->node.nmonths > 0)
	{
		nr->node.month_dir_off = (uint32) ((int64) nr->node.month_dir_off + shift);
		for (uint32 i = 0; i < nr->node.nmonths; i++)
		{
			SiftScopeMonthCell c;
			char	   *at = copy + (nr->node.month_dir_off - nr->node.ref_off) + (Size) i * sizeof(c);

			memcpy(&c, at, sizeof(c));
			c.cube_off = (uint32) ((int64) c.cube_off + shift);
			memcpy(at, &c, sizeof(c));
		}
	}
	nr->block = copy;
	nr->block_len = blk.len;
	pfree(refs);
}

/* ---------------------------------------------------------------------------
 * 4. The refresh
 * ------------------------------------------------------------------------- */

void
sift_refresh_run(const SiftRefreshJob *job, const SiftScopeGranSource *src, StringInfo out, SiftScopeBuildStats *stats)
{
	const SiftScopeMeta *old = job->old;
	uint32		nl0 = old->hdr.nl0_slices,
				base,
				tomb = 0,
				nrecs = 0,
				ik = 0,
				ip = 0,
				live_old;
	bool	   *dead = palloc0(Max(nl0, 1));
	bool		renumber,
				wide = job->total_granules > SIFT_TOPK_COMPACT_GRANS;
	uint32	   *rank = palloc(sizeof(uint32) * Max(nl0, 1)),
			   *off = palloc0(sizeof(uint32) * (job->sub->nkeys + 1)),
			   *mgidx;
	const SiftKeyPlan **by_ord = palloc0(sizeof(*by_ord) * Max(job->sub->nkeys, 1));
	SiftScopeBuildSpec bs = job->spec;
	SiftScopeBuild *b;
	StringInfoData part,
				pool;
	SiftScopeMeta fresh;
	const SiftScopeSection no_route = {NULL, 0};
	SiftScopeGranMask mask;
	SiftKeyRec *recs;
	SiftScopeL0SliceEntry *entries;
	uint32		nentries = 0;
	SiftEmit	em;

	SIFT_INVARIANT(Max(job->spec.ntopk_rings, 1) == old->ntopk_rings, "scope refresh: %u rings against the %u of the object", job->spec.ntopk_rings, old->ntopk_rings);
	SIFT_INVARIANT((job->spec.flags & SIFT_SMETA_NULL_FILTERED) == (old->hdr.flags & SIFT_SMETA_NULL_FILTERED) &&
				   ((job->spec.flags & SIFT_SMETA_NULL_FILTERED) == 0 || job->spec.null_filter_leaf == sift_scope_null_filter_leaf(old)),
				   "scope refresh: the null filter of the spec is not the object's");
	for (uint32 i = 0; i < nl0; i++)
		dead[i] = old->slices[i].cube_len == 0;
	for (uint32 p = 0; p < job->nplans; p++)
		for (uint32 i = 0; i < job->plans[p].ndead; i++)
			dead[job->plans[p].dead_slices[i]] = true;
	for (base = nl0; base > 0 && dead[base - 1]; base--)
		;
	for (uint32 i = 0, live = 0; i < base; i++)
	{
		tomb += dead[i];
		rank[i] = live;
		live += !dead[i];
	}
	live_old = base - tomb;
	renumber = tomb > live_old;

	for (uint32 p = 0; p < job->nplans; p++)
	{
		int32		ord = sift_spm_key_ordinal(job->sub, job->plans[p].key);

		if (ord >= 0)
			by_ord[ord] = &job->plans[p];
	}
	for (uint32 o = 0; o < job->sub->nkeys; o++)
		off[o + 1] = off[o] + (by_ord[o] == NULL ? 0 : by_ord[o]->nmask);
	mgidx = palloc(sizeof(uint32) * Max(off[job->sub->nkeys], 1));
	for (uint32 o = 0; o < job->sub->nkeys; o++)
		if (by_ord[o] != NULL && by_ord[o]->nmask > 0)
			memcpy(mgidx + off[o], by_ord[o]->mask, sizeof(uint32) * by_ord[o]->nmask);
	mask = (SiftScopeGranMask) {off, mgidx, job->all_lo, job->all_hi};

	bs.slice_base = base;
	b = sift_scope_build_begin(&bs, job->sub);
	sift_scope_build_seed_dict(b, old);
	for (uint32 i = 0; i < job->nparts; i++)
		sift_scope_build_add_part(b, job->parts[i].part_gen, job->parts[i].ngranules);
	sift_scope_build_set_mask(b, &mask);
	sift_scope_build_pass_list(b, src, job->pass, job->npass);
	if (job->route_rebuild)
		sift_scope_build_note_homes(b, src, job->homes_pass, job->nhomes_pass);
	initStringInfo(&part);
	sift_scope_build_end(b, &part, stats);
	SIFT_INVARIANT(sift_scope_meta_open(&fresh, part.data, part.len), "scope refresh: the rebuilt nodes do not open");

	recs = palloc(sizeof(*recs) * Max(old->hdr.nkeys + fresh.hdr.nkeys, 1));
	while (ik < old->hdr.nkeys || ip < job->nplans)
	{
		const SiftScopeL2Root *o = ik < old->hdr.nkeys ? &old->roots[ik] : NULL;
		const SiftKeyPlan *pl = ip < job->nplans ? &job->plans[ip] : NULL;

		if (pl == NULL || (o != NULL && o->scope_key < pl->key))
		{
			rec_from_meta(old, o, &recs[nrecs++]);
			ik++;
		}
		else
		{
			const SiftScopeL2Root *f = sift_scope_l2_find(&fresh, pl->key);

			if (o != NULL && o->scope_key == pl->key)
				ik++;
			ip++;
			if (join_key(job, &fresh, pl, f, wide, &recs[nrecs]))
				nrecs++;
		}
	}

	if (renumber)
		for (uint32 r = 0; r < nrecs; r++)
			for (uint32 j = 0; j < recs[r].nnodes; j++)
				if (recs[r].nodes[j].node.flags & SIFT_L1_HAS_IMPURE)
					renumber_node(&recs[r].nodes[j], rank, base, live_old);

	initStringInfo(&pool);
	entries = palloc(sizeof(*entries) * Max(base + fresh.hdr.nl0_slices, 1));
	for (uint32 i = 0; i < base; i++)
	{
		SiftScopeL0SliceEntry e = old->slices[i];

		if (dead[i] && renumber)
			continue;
		if (dead[i])
			e = (SiftScopeL0SliceEntry) {0};
		else
			appendBinaryStringInfo(&pool, old->l0.p + e.cube_off, e.cube_len);
		e.cube_off = (uint32) pool.len - e.cube_len;
		entries[nentries++] = e;
	}
	for (uint32 i = 0; i < fresh.hdr.nl0_slices; i++)
	{
		SiftScopeL0SliceEntry e = fresh.slices[i];

		SIFT_INVARIANT((uint64) e.cube_off + e.cube_len <= fresh.l0.len, "scope refresh: a slice outside the pool");
		e.cube_off = pool.len;
		appendBinaryStringInfo(&pool, fresh.l0.p + fresh.slices[i].cube_off, e.cube_len);
		entries[nentries++] = e;
	}

	memset(&em, 0, sizeof(em));
	em.hdr = old->hdr;
	em.hdr.meta_epoch = job->spec.meta_epoch;
	em.hdr.schema_hash = job->spec.schema_hash;
	em.hdr.nparts = job->nparts;
	em.hdr.total_granules = job->total_granules;
	em.hdr.max_part_gen = job->max_part_gen;
	memcpy(em.hdr.stamps, job->spec.stamps, sizeof(em.hdr.stamps));
	em.parts = job->parts;
	em.l0_pool = pool.data;
	em.l0_pool_len = pool.len;
	em.l0_entries = entries;
	em.l0_nentries = nentries;
	em.dict = fresh.dict.p;
	em.dict_len = fresh.dict.len;
	if (old->ent_route.p != NULL || fresh.ent_route.p != NULL)
	{
		StringInfoData route;

		initStringInfo(&route);
		sift_ent_route_merge(job->route_rebuild ? &no_route : &old->ent_route, &fresh.ent_route, job->max_part_gen, &route);
		em.route = route.data;
		em.route_len = route.len;
	}
	sift_emit_object(out, &em, recs, nrecs);
	sift_scope_meta_close(&fresh);
}
