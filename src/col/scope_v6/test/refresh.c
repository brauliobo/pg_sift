/*
 * col/scope_v6/test/refresh.c - the scenarios of sift._test_scope_v6 (col/scope_v6/test/core.c) about the refresh of a summary: the nodes of a key that
 * changed are built again and the others copied as they are, the level-0 slices that no node names any more leave the pool, and a compaction of
 * entity parts moves the refs of the nodes that held the old parts.
 */
/* check-memory: allow-allocs the arrays follow the rows of a synthetic table of a few thousand rows that a test builds */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "core/cmp.h"
#include "col/scope_v6/int.h"
#include "col/scope_v6/test/world.h"

#define OUTL(t, ...)	do { appendStringInfo(&(t)->out, __VA_ARGS__); appendStringInfoChar(&(t)->out, '\n'); } while (0)

extern void scope_v6_scenario_refresh(TCheck *t);
extern void scope_v6_scenario_compact(TCheck *t);

static uint64
reads_of(const SiftScopeBuildStats *s)
{
	return s->cube_reads + s->row_decodes;
}

/* Every slice that holds a cube is named by exactly one node, no node names a tombstone, the pool is the cubes and the last slice is not a tombstone */
static uint32
pool_check(TCheck *t, const SiftScopeMeta *m, const char *label, uint32 *tombs)
{
	uint8	   *named = palloc0(Max(m->hdr.nl0_slices, 1));
	uint64		bytes = 0,
				end = 0;
	uint32		live = 0;

	*tombs = 0;
	for (uint32 k = 0; k < m->hdr.nl1_nodes; k++)
	{
		SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];
		uint32		n = sift_scope_node_refs(m, &m->nodes[k], refs);

		for (uint32 i = 0; i < n; i++)
			if (SIFT_SCOPE_REF_SLICED(refs[i].slice_id))
			{
				TCHK(t, refs[i].slice_id < m->hdr.nl0_slices, "%s: node %u names slice %u of %u", label, k, refs[i].slice_id, m->hdr.nl0_slices);
				if (refs[i].slice_id < m->hdr.nl0_slices)
					named[refs[i].slice_id]++;
			}
	}
	for (uint32 i = 0; i < m->hdr.nl0_slices; i++)
	{
		bytes += m->slices[i].cube_len;
		end = Max(end, (uint64) m->slices[i].cube_off + m->slices[i].cube_len);
		if (m->slices[i].cube_len > 0)
		{
			live++;
			TCHK(t, named[i] == 1, "%s: slice %u is named %u times", label, i, named[i]);
		}
		else
		{
			(*tombs)++;
			TCHK(t, named[i] == 0, "%s: tombstone %u is named", label, i);
		}
	}
	TCHK(t, end == bytes && m->l0.len >= bytes, "%s: the pool ends at %llu, its slices hold %llu", label, (unsigned long long) end, (unsigned long long) bytes);
	TCHK(t, m->hdr.nl0_slices == 0 || m->slices[m->hdr.nl0_slices - 1].cube_len > 0, "%s: the last slice is a tombstone", label);
	pfree(named);
	return live;
}

static bool
node_bytes_equal(const SiftScopeMeta *ma, const SiftScopeL1Node *a, const SiftScopeMeta *mb, const SiftScopeL1Node *b)
{
	return a->ref_len == b->ref_len && a->all_cube_len == b->all_cube_len && memcmp(ma->l1.p + a->ref_off, mb->l1.p + b->ref_off, a->ref_len) == 0 &&
		memcmp(ma->l1.p + a->all_cube_off, mb->l1.p + b->all_cube_off, a->all_cube_len) == 0;
}

/* The two nodes name the same granules and the slices they name hold the same cubes, whatever the numbers of the slices */
static bool
node_alike(const SiftScopeMeta *ma, const SiftScopeL1Node *a, const SiftScopeMeta *mb, const SiftScopeL1Node *b)
{
	SiftScopeGranRef ra[SIFT_SCOPE_L1_CAP],
				rb[SIFT_SCOPE_L1_CAP];
	uint32		n = sift_scope_node_refs(ma, a, ra);

	if (n != sift_scope_node_refs(mb, b, rb) || a->all_cube_len != b->all_cube_len ||
		memcmp(ma->l1.p + a->all_cube_off, mb->l1.p + b->all_cube_off, a->all_cube_len) != 0)
		return false;
	for (uint32 i = 0; i < n; i++)
	{
		SiftScopeCubeAcc ca,
					cb;
		bool		same;

		if (ra[i].global_granule_idx != rb[i].global_granule_idx || (ra[i].slice_id == SIFT_SCOPE_PURE_GRANULE) != (rb[i].slice_id == SIFT_SCOPE_PURE_GRANULE) || (ra[i].slice_id == SIFT_SCOPE_INLINE_SCAN) != (rb[i].slice_id == SIFT_SCOPE_INLINE_SCAN))
			return false;
		if (!SIFT_SCOPE_REF_SLICED(ra[i].slice_id))
			continue;
		if (!sift_scope_slice_cube(ma, ra[i].slice_id, &ca) || !sift_scope_slice_cube(mb, rb[i].slice_id, &cb))
			return false;
		same = sift_scube_equal(&ca, &cb);
		sift_scube_free(&ca);
		sift_scube_free(&cb);
		if (!same)
			return false;
	}
	return true;
}

/* The nodes of the key in new that are byte for byte nodes of the key in old */
static uint32
nodes_kept(const SiftScopeMeta *old, const SiftScopeL2Root *o, const SiftScopeMeta *new, const SiftScopeL2Root *n)
{
	uint32		kept = 0;

	for (uint32 j = 0; j < n->l1_count; j++)
		for (uint32 i = 0; i < o->l1_count; i++)
			if (node_bytes_equal(old, &old->nodes[o->l1_first + i], new, &new->nodes[n->l1_first + j]))
			{
				kept++;
				break;
			}
	return kept;
}

static uint32
nodes_holding(const SiftScopeMeta *m, const SiftScopeL2Root *r, int64 member)
{
	uint32		c = 0;

	for (uint32 i = 0; i < r->l1_count; i++)
		c += sift_scope_node_has_member(m, &m->nodes[r->l1_first + i], member);
	return c;
}

/* A member of the key that one of its nodes alone holds, and that has rows */
static bool
lone_member(const TWorld *w, const SiftScopeMeta *m, const SiftScopeL2Root *r, int64 *member)
{
	for (uint32 p = 0; p < w->npairs; p++)
		if (w->pairs[p].key == r->scope_key && nodes_holding(m, r, w->pairs[p].member) == 1)
		{
			*member = w->pairs[p].member;
			return true;
		}
	return false;
}

static void
set_pairs(TWorld *w, const SiftScopeMemberKey *pairs, uint32 n)
{
	w->pairs = memcpy(palloc(sizeof(*pairs) * Max(n, 1)), pairs, sizeof(*pairs) * n);
	w->npairs = n;
}

/* The slices a key names, and the owner of the slice with the highest number */
static uint32
slices_of_key(const SiftScopeMeta *m, const SiftScopeL2Root *r)
{
	uint32		c = 0;

	for (uint32 i = 0; i < r->l1_count; i++)
	{
		SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];
		uint32		n = sift_scope_node_refs(m, &m->nodes[r->l1_first + i], refs);

		for (uint32 j = 0; j < n; j++)
			c += SIFT_SCOPE_REF_SLICED(refs[j].slice_id);
	}
	return c;
}

static const SiftScopeL2Root *
owner_of_last_slice(const SiftScopeMeta *m)
{
	uint32		last = m->hdr.nl0_slices - 1;

	for (uint32 k = 0; k < m->hdr.nkeys; k++)
		for (uint32 i = 0; i < m->roots[k].l1_count; i++)
		{
			SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];
			uint32		n = sift_scope_node_refs(m, &m->nodes[m->roots[k].l1_first + i], refs);

			for (uint32 j = 0; j < n; j++)
				if (refs[j].slice_id == last)
					return &m->roots[k];
		}
	return NULL;
}

void
scope_v6_scenario_refresh(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 2400, .maxrows = 40, .long_pct = 8, .granule_rows = 96, .nparts = 2, .months = 6, .nkeys = 4, .keys_per_member = 2};
	SiftScopeBuildSpec spec;
	StringInfoData obj,
				step1,
				step2,
				full;
	SiftScopeMeta m0,
				m1,
				m2,
				mfull;
	SiftScopePairMap *pm0,
			   *pm1,
			   *pm2;
	SiftScopeDirty *dd;
	SiftScopeBuildStats st_ref,
				st_full;
	SiftScopeGranSource src;
	const SiftScopeL2Root *big = NULL,
			   *last_owner;
	int64		member = 0;
	SiftScopeMemberKey *keep;
	uint32		nkeep = 0,
				nd,
				tombs,
				kept,
				others_kept = 0,
				others_nodes = 0,
				rebuilt,
				owned,
				before_nodes;
	bool		renumber_expected;

	tw_gen(&w, t, &sp);
	pm0 = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	tw_open(t, &obj, &m0);
	pool_check(t, &m0, "built", &tombs);
	for (uint32 k = 0; k < m0.hdr.nkeys; k++)
		if (big == NULL || m0.roots[k].l1_count > big->l1_count)
			big = &m0.roots[k];
	TCHK(t, big != NULL && big->l1_count >= 5 && lone_member(&w, &m0, big, &member), "a key of several nodes with a member that one holds: %u nodes", big == NULL ? 0 : big->l1_count);
	before_nodes = big->l1_count;

	keep = palloc(sizeof(*keep) * w.npairs);
	for (uint32 p = 0; p < w.npairs; p++)
		if (!(w.pairs[p].key == big->scope_key && w.pairs[p].member == member))
			keep[nkeep++] = w.pairs[p];
	set_pairs(&w, keep, nkeep);
	pm1 = tw_pm(&w);
	nd = sift_spm_diff(pm0, pm1, &dd);
	TCHK(t, nd == 1 && dd[0].key == big->scope_key && dd[0].nmembers == 1 && dd[0].members[0] == member, "one key changed, with one member");

	spec.meta_epoch = 2;
	src = tw_source(&w);
	w.cur_pm = pm1;
	initStringInfo(&step1);
	sift_scope_refresh_dirty(&m0, &spec, pm1, dd, nd, &src, &step1, &st_ref);
	tw_open(t, &step1, &m1);
	tw_check_meta(t, &w, &m1, pm1, "one member out");
	pool_check(t, &m1, "one member out", &tombs);
	{
		const SiftScopeL2Root *nb = sift_scope_l2_find(&m1, big->scope_key);

		TCHK(t, nb != NULL, "the key is still there");
		kept = nb == NULL ? 0 : nodes_kept(&m0, big, &m1, nb);
		rebuilt = nb == NULL ? 0 : nb->l1_count - Min(kept, nb->l1_count);
		TCHK(t, kept >= 1 && rebuilt >= 1 && kept + 4 >= before_nodes, "the nodes of a dirty key that hold none of its changed members are copied: %u kept, %u built again of %u", kept, rebuilt, before_nodes);
	}
	for (uint32 k = 0; k < m0.hdr.nkeys; k++)
	{
		const SiftScopeL2Root *o = &m0.roots[k],
				   *n = sift_scope_l2_find(&m1, o->scope_key);

		if (o->scope_key == big->scope_key || n == NULL)
			continue;
		others_nodes += o->l1_count;
		others_kept += nodes_kept(&m0, o, &m1, n);
	}
	TCHK(t, others_nodes > 0 && others_kept == others_nodes, "the nodes of the other keys are copied as they are: %u of %u", others_kept, others_nodes);
	{
		SiftScopeBuildSpec fs = spec;

		initStringInfo(&full);
		tw_build(&w, &fs, tw_pm(&w), false, &full, &st_full);
		w.cur_pm = pm1;
		tw_open(t, &full, &mfull);
		TCHK(t, reads_of(&st_ref) * 4 < reads_of(&st_full), "the refresh reads %llu granules, a rebuild %llu", (unsigned long long) reads_of(&st_ref), (unsigned long long) reads_of(&st_full));
	}

	last_owner = owner_of_last_slice(&m1);
	TCHK(t, last_owner != NULL, "the last slice has an owner");
	if (last_owner != NULL)
	{
		uint32		nl0 = m1.hdr.nl0_slices;

		owned = slices_of_key(&m1, last_owner);
		nkeep = 0;
		for (uint32 p = 0; p < w.npairs; p++)
			if (w.pairs[p].key == last_owner->scope_key)
				keep[nkeep++] = w.pairs[p];
		set_pairs(&w, keep, nkeep);
		pm2 = tw_pm(&w);
		nd = sift_spm_diff(pm1, pm2, &dd);
		TCHK(t, nd == pm1->nkeys - 1, "every other key lost its members: %u", nd);
		spec.meta_epoch = 3;
		w.cur_pm = pm2;
		src = tw_source(&w);
		initStringInfo(&step2);
		sift_scope_refresh_dirty(&m1, &spec, pm2, dd, nd, &src, &step2, &st_ref);
		tw_open(t, &step2, &m2);
		tw_check_meta(t, &w, &m2, pm2, "all but the owner of the last slice out");
		pool_check(t, &m2, "all but the owner of the last slice out", &tombs);
		renumber_expected = nl0 - owned > owned;
		if (renumber_expected)
			TCHK(t, tombs == 0 && m2.hdr.nl0_slices == owned, "more dead slices than live ones: they are numbered again, %u slices left of %u (%u live)", m2.hdr.nl0_slices, nl0, owned);
		else
			TCHK(t, tombs == nl0 - owned && m2.hdr.nl0_slices == nl0, "fewer dead slices than live ones: the numbers stay, %u tombstones", tombs);
		TCHK(t, sift_scope_l2_find(&m2, last_owner->scope_key) != NULL && m2.hdr.nkeys == 1, "the key that kept its members is the only one");
		{
			const SiftScopeL2Root *n = sift_scope_l2_find(&m2, last_owner->scope_key);

			for (uint32 i = 0; n != NULL && i < n->l1_count; i++)
			{
				const SiftScopeL1Node *a = &m1.nodes[last_owner->l1_first + i],
						   *b = &m2.nodes[n->l1_first + i];
				SiftScopeGranRef ra[SIFT_SCOPE_L1_CAP],
							rb[SIFT_SCOPE_L1_CAP];
				uint32		cnt = sift_scope_node_refs(&m1, a, ra);

				TCHK(t, cnt == sift_scope_node_refs(&m2, b, rb) && a->all_cube_len == b->all_cube_len, "a node of the key that kept its members is the same node");
				for (uint32 j = 0; j < cnt && cnt == sift_scope_node_refs(&m2, b, rb); j++)
				{
					SiftScopeCubeAcc ca,
								cb;

					TCHK(t, ra[j].global_granule_idx == rb[j].global_granule_idx && (ra[j].slice_id == SIFT_SCOPE_PURE_GRANULE) == (rb[j].slice_id == SIFT_SCOPE_PURE_GRANULE), "a ref still names its granule");
					if (SIFT_SCOPE_REF_SLICED(ra[j].slice_id) && SIFT_SCOPE_REF_SLICED(rb[j].slice_id))
					{
						TCHK(t, sift_scope_slice_cube(&m1, ra[j].slice_id, &ca) && sift_scope_slice_cube(&m2, rb[j].slice_id, &cb) && sift_scube_equal(&ca, &cb), "a slice kept its cube under its new number");
					}
				}
			}
		}
		OUTL(t, "refresh dirty_key_nodes_kept_as_they_are=%d other_keys_copied=%d dead_slices_left_pool=1", kept >= 1, others_kept == others_nodes);
		OUTL(t, "refresh slices_numbered_again_when_dead_outnumber_live=%d no_orphan_slice=1 refresh_reads_fewer_granules=%d", renumber_expected, reads_of(&st_ref) < reads_of(&st_full));
	}
}

/* ---------------------------------------------------------------------------
 * compact: the entity parts of a snapshot were compacted into new ones
 * ------------------------------------------------------------------------- */

static void
check_rings_after(TCheck *t, TWorld *w, const SiftScopeMeta *m, SiftScopePairMap *pm, const char *label)
{
	SiftScopeEntity e = tw_entity(w);

	w->cur_pm = pm;
	for (uint32 k = 0; k < pm->nkeys; k++)
	{
		int64		key = pm->scope_keys[k];
		const SiftScopeL2Root *r = sift_scope_l2_find(m, key);

		for (int filtered = 0; r != NULL && filtered < 2; filtered++)
		{
			SiftScopeNewest truth[100],
						got[100],
						ring[SIFT_SCOPE_TOPK_CAP];
			uint32		nt,
						ng,
						nr,
						from_ring;

			w->filter = filtered;
			nt = tw_truth_newest(w, pm, key, 100, truth);
			ng = sift_scope_latest(m, r, 100, filtered, &e, got, &from_ring);
			TCHK(t, ng == nt && memcmp(got, truth, sizeof(*got) * ng) == 0, "%s: key %lld latest 100 (filtered %d): %u rows, truth %u", label, (long long) key, filtered, ng, nt);
			if (!filtered)
			{
				nr = sift_scope_topk_get(m, r, ring);
				TCHK(t, nr <= nt && memcmp(ring, truth, sizeof(*ring) * nr) == 0 && (nr == Min(nt, SIFT_SCOPE_TOPK_CAP) || (r->flags & SIFT_L2_TOPK_PARTIAL)),
					 "%s: key %lld ring of %u entries after the compaction", label, (long long) key, nr);
			}
		}
	}
	w->filter = false;
}

static void
compact_round(TCheck *t, TWorld *w, SiftScopePairMap *pm, StringInfo obj, SiftScopeMeta *m, uint32 part, uint32 per_mille, uint32 new_rows, uint64 epoch, uint32 *kept_nodes, uint32 *moved_nodes)
{
	SiftScopeBuildSpec spec;
	SiftScopePartSwap swap;
	SiftScopeNewPart added;
	SiftScopeGranSource src;
	SiftScopeBuildStats st;
	StringInfoData fresh;
	SiftScopeMeta mn;
	uint64		old_gen;
	uint32		old_total = m->hdr.total_granules,
				lo = w->part_base[part],
				hi = w->part_base[part] + w->part_n[part],
				tombs;

	tw_kill_part_rows(w, t, part, per_mille);
	tw_compact(w, t, part, new_rows, &swap, &added, &old_gen);
	tw_spec(w, &spec, 0);
	spec.meta_epoch = epoch;
	src = tw_source(w);
	w->cur_pm = pm;
	initStringInfo(&fresh);
	sift_scope_refresh_parts(m, &spec, pm, &swap, &src, &fresh, &st);
	tw_open(t, &fresh, &mn);
	tw_check_meta(t, w, &mn, pm, "compacted");
	pool_check(t, &mn, "compacted", &tombs);
	check_rings_after(t, w, &mn, pm, "compacted");

	TCHK(t, mn.hdr.nparts == m->hdr.nparts + 1 && mn.hdr.total_granules == old_total + added.ngranules, "the new part follows the others: %u parts, %llu granules", mn.hdr.nparts, (unsigned long long) mn.hdr.total_granules);
	TCHK(t, mn.parts[part].part_gen == 0 && mn.parts[part].base_granule_idx == m->parts[part].base_granule_idx && mn.parts[part].ngranules == m->parts[part].ngranules,
		 "the old part keeps its range of granule indexes and is marked as gone");
	TCHK(t, mn.parts[mn.hdr.nparts - 1].part_gen == added.part_gen && mn.parts[mn.hdr.nparts - 1].base_granule_idx == old_total && mn.parts[mn.hdr.nparts - 1].ngranules == added.ngranules,
		 "the part that took the rows is registered after the last granule");
	{
		uint64		gen;
		uint32		id;

		TCHK(t, !sift_scope_gidx_part(&mn, lo, &gen, &id) && sift_scope_gidx_part(&mn, old_total, &gen, &id) && gen == added.part_gen && id == 0, "a granule index of the old part names no part, one of the new part does");
	}
	for (uint32 k = 0; k < mn.hdr.nl1_nodes; k++)
	{
		SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];
		uint32		n = sift_scope_node_refs(&mn, &mn.nodes[k], refs);

		for (uint32 i = 0; i < n; i++)
			TCHK(t, refs[i].global_granule_idx < lo || refs[i].global_granule_idx >= hi, "node %u still names granule %u of the part that went", k, refs[i].global_granule_idx);
	}
	for (uint32 k = 0; k < m->hdr.nkeys; k++)
	{
		const SiftScopeL2Root *o = &m->roots[k],
				   *n = sift_scope_l2_find(&mn, o->scope_key);

		for (uint32 i = 0; n != NULL && i < o->l1_count; i++)
		{
			SiftScopeGranRef refs[SIFT_SCOPE_L1_CAP];
			uint32		cnt = sift_scope_node_refs(m, &m->nodes[o->l1_first + i], refs);
			bool		holds_old = false,
						copied = false;

			for (uint32 j = 0; j < cnt; j++)
				holds_old |= refs[j].global_granule_idx >= lo && refs[j].global_granule_idx < hi;
			for (uint32 j = 0; j < n->l1_count; j++)
				copied |= node_alike(m, &m->nodes[o->l1_first + i], &mn, &mn.nodes[n->l1_first + j]);
			if (holds_old)
				(*moved_nodes)++;
			else
			{
				(*kept_nodes)++;
				TCHK(t, copied, "a node with no granule of the part that went is copied as it was");
			}
		}
	}
	TCHK(t, reads_of(&st) < old_total, "the refresh reads the granules of the new part and of the nodes that moved, not all of the %u: %llu", old_total, (unsigned long long) reads_of(&st));
	*obj = fresh;
	sift_scope_meta_open(m, obj->data, obj->len);
}

void
scope_v6_scenario_compact(TCheck *t)
{
	TWorld		w;
	TWorldSpec	sp = {.nmembers = 300, .maxrows = 40, .long_pct = 8, .granule_rows = 16, .nparts = 3, .months = 6, .nkeys = 4, .keys_per_member = 2};
	SiftScopeBuildSpec spec;
	StringInfoData obj;
	SiftScopeMeta m;
	SiftScopePairMap *pm;
	uint32		kept = 0,
				moved = 0;

	tw_gen(&w, t, &sp);
	pm = tw_pm(&w);
	tw_spec(&w, &spec, 0);
	initStringInfo(&obj);
	tw_build(&w, &spec, tw_pm(&w), false, &obj, NULL);
	tw_open(t, &obj, &m);
	compact_round(t, &w, pm, &obj, &m, 1, 300, 24, 2, &kept, &moved);
	compact_round(t, &w, pm, &obj, &m, 0, 150, 40, 3, &kept, &moved);
	TCHK(t, kept > 0 && moved > 0, "some nodes are copied and some moved: %u and %u", kept, moved);
	OUTL(t, "compact rounds=2 nodes_without_old_granules_copied=%d nodes_moved_to_new_part=%d", kept > 0, moved > 0);
	OUTL(t, "compact retired_parts_keep_their_range=1 ring_and_month_map_follow=1 reads_fewer_than_all_granules=1");
}

#endif							/* SIFT_TESTING */
