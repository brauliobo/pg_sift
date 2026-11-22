/*
 * col/scope_v6/build.c - builds the scope summary in one pass over the granules of the entity parts, with no temporary file.
 *
 * Plain CREATE INDEX calls sift_scope_build_on_granule for every granule the part writer finishes, while its arrays and its pre-aggregate cube
 * are in cache; CREATE INDEX CONCURRENTLY calls sift_scope_build_pass2 over the parts already written, where a granule is read from its zone
 * map and its cube only. A granule of one member is answered by merging its cube into the cube of each key that owns the member: no row is
 * read. A granule of several members is decoded for the member, time and cube leaves only and cut into one thin cube for each key.
 *
 * What a key accumulates is bounded: one open level-1 node (at most SIFT_SCOPE_L1_CAP granules), the running level-2 cube and a ring of
 * SIFT_SCOPE_TOPK_CAP rows. When the accumulators of all keys pass the memory budget the key with the largest open node is flushed (a heap
 * finds it without a scan); when the flushed nodes' month cubes of level 2 are what holds the memory, the least recently touched keys write
 * theirs out as bytes and read them back when the object is laid out.
 */
/* check-memory: allow-allocs the arrays follow the keys of the pair map, the level-1 nodes and the granules of one build, not the rows of a table */
#include "core/host.h"

#include "core/cmp.h"
#include "core/memtrack.h"
#include "col/scope_v6/build_int.h"

SiftScopeBuild *
sift_scope_build_begin(const SiftScopeBuildSpec *spec, SiftScopePairMap *pm)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "scope summary build");
	SiftScopeBuild *b = MemoryContextAllocZero(cx, sizeof(*b));

	SIFT_INVARIANT(spec->ntopk_rings <= SIFT_SCOPE_TOPK_RINGS_CAP && (spec->ntopk_rings <= 1 || spec->time_leaf >= 0),
				   "scope build: %u rings with time leaf %d", spec->ntopk_rings, spec->time_leaf);
	for (uint32 r = 1; r < Max(spec->ntopk_rings, 1); r++)
		SIFT_INVARIANT(spec->extra_leaf[r - 1] >= 0, "scope build: ring %u has no order leaf", r);
	b->mcxt = cx;
	b->spec = *spec;
	b->pm = pm;
	b->nrings = Max(spec->ntopk_rings, 1);
	SIFT_INVARIANT(!(spec->flags & SIFT_SMETA_CLUSTERED_11) || !(spec->flags & SIFT_SMETA_CLUSTERED_PP), "scope build: both clusterings");
	SIFT_INVARIANT(!(spec->flags & SIFT_SMETA_MULTISET) || !(spec->flags & SIFT_SMETA_CLUSTERED), "scope build: a multiset scope is not clustered");
	SIFT_INVARIANT(!(spec->flags & SIFT_SMETA_NULL_FILTERED) || spec->null_filter_leaf >= 0, "scope build: a null filter on leaf %d", spec->null_filter_leaf);
	b->spec.flags = b->nrings > 1 ? b->spec.flags | SIFT_SMETA_MULTI_TOPK : b->spec.flags & ~SIFT_SMETA_MULTI_TOPK;
	if (b->spec.flags & SIFT_SMETA_CLUSTERED)
		sift_homes_init(&b->homes, cx);
	b->budget = spec->mem_budget > 0 ? spec->mem_budget : SIFT_SCOPE_BUILD_MEM_DEF;
	b->acc = MemoryContextAllocZero(cx, sizeof(KeyAcc *) * Max(pm->nkeys, 1));
	b->slot_of_key = MemoryContextAlloc(cx, sizeof(uint32) * Max(pm->nkeys, 1));
	memset(b->slot_of_key, 0xFF, sizeof(uint32) * Max(pm->nkeys, 1));
	b->open_size = MemoryContextAllocZero(cx, sizeof(Size) * Max(pm->nkeys, 1));
	{
		MemoryContext old = MemoryContextSwitchTo(cx);

		initStringInfo(&b->l0pool);
		initStringInfo(&b->l1pool);
		sift_mheap_init(&b->open, pm->nkeys, b->open_size);
		MemoryContextSwitchTo(old);
	}
	sift_sdict_init(&b->dict, cx);
	return b;
}

uint32
sift_scope_build_add_part(SiftScopeBuild *b, uint64 part_gen, uint32 ngranules)
{
	uint32		base = b->total_granules;
	IN_BUILD(b);

	b->parts = b->parts == NULL ? palloc(sizeof(*b->parts) * 8) : b->nparts % 8 == 0 ? repalloc(b->parts, sizeof(*b->parts) * (b->nparts + 8)) : b->parts;
	b->parts[b->nparts++] = (SiftScopePartMapEntry) {part_gen, base, ngranules};
	b->total_granules += ngranules;
	OUT_BUILD();
	return base;
}

const uint32 *
sift_scope_dict_map(SiftScopeBuild *b, uint32 n, const char *const *strs)
{
	uint32	   *map = MemoryContextAlloc(b->mcxt, sizeof(uint32) * Max(n, 1));

	for (uint32 i = 0; i < n; i++)
		map[i] = sift_sdict_intern(&b->dict, strs[i], strlen(strs[i]));
	return map;
}

void
sift_scope_build_seed_dict(SiftScopeBuild *b, const SiftScopeMeta *old)
{
	SIFT_INVARIANT(sift_sdict_seed(&b->dict, old->dict.p, old->dict.len), "scope summary: a damaged dictionary");
}

void
sift_scope_build_set_mask(SiftScopeBuild *b, const SiftScopeGranMask *mask)
{
	b->mask = mask;
}

/* The key takes the granule: always, unless a mask limits the granules of each key */
static bool
key_takes(const SiftScopeBuild *b, uint32 ord, uint32 gidx)
{
	uint32		lo,
				hi;

	if (b->mask == NULL || (gidx >= b->mask->all_lo && gidx < b->mask->all_hi))
		return true;
	lo = b->mask->off[ord];
	hi = b->mask->off[ord + 1];
	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (b->mask->gidx[mid] == gidx)
			return true;
		if (b->mask->gidx[mid] < gidx)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

/* ---------------------------------------------------------------------------
 * 1. Level-1 nodes
 * ------------------------------------------------------------------------- */

static KeyAcc *
key_acc(SiftScopeBuild *b, uint32 ord)
{
	KeyAcc	   *k = b->acc[ord];

	if (k == NULL)
	{
		SiftScopeShape sh = b->spec.shape;

		k = b->acc[ord] = palloc0(sizeof(*k));
		k->ring = palloc0(sizeof(SiftTopKHeap) * b->nrings);
		k->ord = ord;
		k->scope_key = b->pm->scope_keys[ord];
		sift_scube_init(&k->l1_all, sh);
		sift_scube_init(&k->l2_all, sh);
		sift_smonth_init(&k->l1_months);
		sift_smonth_init(&k->l2_months);
		b->nactive++;
	}
	else if (k->in_lru)
		sift_sbld_lru_front(b, k);
	return k;
}

static void
member_note(KeyAcc *k, int64 member, bool coded)
{
	if (k->nrefs == 0 && k->mcount == 0)
		k->mlo = k->mhi = member;
	k->mlo = Min(k->mlo, member);
	k->mhi = Max(k->mhi, member);
	if (k->mcount == 0 || member != k->last_member)
		k->mcount++;
	k->last_member = member;
	if (coded)
	{
		if (k->nmembers == k->capmembers)
		{
			k->capmembers = k->capmembers == 0 ? 64 : k->capmembers * 2;
			k->members = k->members == NULL ? palloc(k->capmembers * sizeof(int64)) : repalloc(k->members, k->capmembers * sizeof(int64));
		}
		k->members[k->nmembers++] = member;
	}
}

static void
node_append(SiftScopeBuild *b, const NodeRec *n)
{
	if (b->nnodes == b->capnodes)
	{
		b->capnodes = b->capnodes == 0 ? 64 : b->capnodes * 2;
		b->nodes = b->nodes == NULL ? palloc(b->capnodes * sizeof(NodeRec)) : repalloc(b->nodes, b->capnodes * sizeof(NodeRec));
	}
	b->nodes[b->nnodes++] = *n;
}

static void
write_l1_node(SiftScopeBuild *b, KeyAcc *k)
{
	NodeRec		n;
	SiftScopeL1Node *d = &n.d;
	StringInfo	p = &b->l1pool;
	bool		coded = (b->spec.flags & SIFT_SMETA_CODED_MEM) && !k->members_unknown;

	memset(&n, 0, sizeof(n));
	d->nrefs = k->nrefs;
	d->flags = k->impure ? SIFT_L1_HAS_IMPURE : SIFT_L1_ALL_PURE;
	d->member_lo = k->mlo;
	d->member_hi = k->mhi;
	d->row_count = k->l1_all.row_count;
	d->time_min = k->l1_all.has_time ? k->l1_all.time_min : 0;
	d->time_max = k->l1_all.has_time ? k->l1_all.time_max : 0;
	d->member_count = k->mcount;
	d->ref_off = p->len;
	sift_put_refs(p, k->refs, k->nrefs);
	d->ref_len = p->len - d->ref_off;
	if (coded && k->nmembers > 0)
	{
		qsort(k->members, k->nmembers, sizeof(int64), sift_cmp_i64);
		{
			uint32		u = 0;

			for (uint32 i = 0; i < k->nmembers; i++)
				if (u == 0 || k->members[u - 1] != k->members[i])
					k->members[u++] = k->members[i];
			k->nmembers = u;
			d->member_count = u;
		}
		d->bloom_off = p->len;
		if (k->nmembers <= SIFT_SCOPE_EXACT_MEMBERS)
		{
			appendBinaryStringInfo(p, (char *) k->members, k->nmembers * sizeof(int64));
			d->bloom_len = k->nmembers * sizeof(int64);
		}
		else
		{
			uint32		nb = sift_bloom_blocks(k->nmembers);
			char	   *bits = palloc0((Size) nb * 32);

			for (uint32 i = 0; i < k->nmembers; i++)
				sift_bloom_add(bits, nb, k->members[i]);
			appendBinaryStringInfo(p, bits, (Size) nb * 32);
			d->bloom_len = nb * 32;
			d->flags |= SIFT_L1_HAS_BLOOM;
			pfree(bits);
		}
	}
	d->all_cube_off = p->len;
	sift_scube_write(p, &k->l1_all);
	d->all_cube_len = p->len - d->all_cube_off;
	d->nmonths = k->l1_months.n;
	if (d->nmonths > 0)
	{
		SiftScopeMonthCell *cells = palloc(sizeof(*cells) * d->nmonths);

		d->flags |= SIFT_L1_HAS_MONTHS;
		for (uint32 i = 0; i < d->nmonths; i++)
		{
			uint32		at = p->len;

			sift_scube_write(p, &k->l1_months.e[i].cube);
			cells[i] = (SiftScopeMonthCell) {k->l1_months.e[i].month_id, at, p->len - at};
		}
		d->month_dir_off = p->len;
		appendBinaryStringInfo(p, (char *) cells, d->nmonths * sizeof(*cells));
		pfree(cells);
	}
	n.block_len = p->len - d->ref_off;
	n.pm_ord = k->ord;
	n.first_arrival = k->first_arrival;
	node_append(b, &n);
	b->stats.l1_flushes++;
	k->member_total += d->member_count;
}

static void
open_node_reset(SiftScopeBuild *b, KeyAcc *k, bool release)
{
	k->first_arrival = k->narrivals;
	k->nrefs = 0;
	k->impure = false;
	k->mcount = 0;
	k->nmembers = 0;
	k->members_unknown = false;
	if (!release)
	{
		sift_scube_clear(&k->l1_all);
		sift_smonth_clear(&k->l1_months);
		return;
	}
	sift_scube_free(&k->l1_all);
	sift_scube_init(&k->l1_all, b->spec.shape);
	sift_smonth_free(&k->l1_months);
	if (k->members != NULL)
		pfree(k->members);
	k->members = NULL;
	k->capmembers = 0;
}

/*
 * Writes the open node of a key to the level-1 pool and folds its cubes into the key's running level-2 cubes. release: the arrays of the open
 * node go back to the allocator (an eviction wants the bytes back; a full node is followed by another that fills the same arrays)
 */
void
sift_sbld_flush_l1(SiftScopeBuild *b, KeyAcc *k, bool release)
{
	if (k->nrefs == 0)
		return;
	write_l1_node(b, k);
	sift_scube_merge(&k->l2_all, &k->l1_all, NULL);
	sift_smonth_merge(&k->l2_months, b->spec.shape, &k->l1_months, NULL);
	open_node_reset(b, k, release);
}

/* The last open node of a key: a key that has no other node has no level-2 block, so its cubes are not folded into one */
static void
flush_last_node(SiftScopeBuild *b, KeyAcc *k)
{
	if (k->nrefs == 0)
		return;
	if (k->first_arrival != 0)
	{
		sift_sbld_flush_l1(b, k, true);
		return;
	}
	write_l1_node(b, k);
	open_node_reset(b, k, true);
}

/* ---------------------------------------------------------------------------
 * 2. A granule
 * ------------------------------------------------------------------------- */

static bool
has_time(const SiftScopeBuild *b)
{
	return b->spec.time_leaf >= 0;
}

static void
ref_append(KeyAcc *k, uint32 gidx, uint32 slice_id)
{
	if (k->nrefs == 0)
		k->first_arrival = k->narrivals;
	k->refs[k->nrefs++] = (SiftScopeGranRef) {gidx, slice_id};
	if (slice_id != SIFT_SCOPE_PURE_GRANULE)
		k->impure = true;
	k->narrivals++;
}

static void
months_note(KeyAcc *k, const SiftScopeMonthTable *months, uint32 ordinal)
{
	for (uint32 i = 0; i < months->n; i++)
		sift_mmap_add(&k->mmap, months->e[i].month_id, ordinal);
}

static void
close_ref(SiftScopeBuild *b, KeyAcc *k)
{
	if (k->nrefs == SIFT_SCOPE_L1_CAP)
		sift_sbld_flush_l1(b, k, false);
	sift_sbld_account(b, k);
}

static void
tail_offer(SiftScopeBuild *b, KeyAcc *k, uint32 ring, const SiftScopeTail *t, uint32 nt, uint32 gidx)
{
	if (b->spec.flags & SIFT_SMETA_MULTISET)
		return;
	for (uint32 i = 0; i < nt; i++)
		sift_topk_offer(&k->ring[ring], t[i].time, gidx, t[i].row_ord);
}

/* The zone of the order leaf of a ring in a granule: ring 0 is the time leaf */
static const SiftScopeZone *
ring_zone(const SiftScopeGranHead *h, uint32 ring)
{
	return ring == 0 ? &h->time : &h->extra_topk[ring - 1];
}

/* A granule that belongs wholly to the keys owners[0..n): its cube merges into each, no row is read */
static void
granule_whole(SiftScopeBuild *b, const SiftScopeGranSource *src, uint32 gidx, const SiftScopeGranHead *h, const uint32 *owners, uint32 n)
{
	SiftScopeGranCube gc;
	SiftScopeTail tail[SIFT_SCOPE_TOPK_RINGS_CAP][SIFT_SCOPE_TOPK_CAP];
	uint32		nt[SIFT_SCOPE_TOPK_RINGS_CAP] = {0};
	bool		tail_read[SIFT_SCOPE_TOPK_RINGS_CAP] = {false},
				cube_read = false,
				coded = (b->spec.flags & SIFT_SMETA_CODED_MEM) != 0;
	bool		single = h->member.has_min_max && h->member.nnull == 0 && h->member.min == h->member.max;

	memset(&gc, 0, sizeof(gc));
	for (uint32 i = 0; i < n; i++)
	{
		KeyAcc	   *k;

		if (!key_takes(b, owners[i], gidx))
			continue;
		if (!cube_read)
		{
			SRC_CALL(b, src->cube(src->arg, gidx, &gc));
			b->stats.cube_reads++;
			cube_read = true;
		}
		k = key_acc(b, owners[i]);
		sift_scube_merge(&k->l1_all, gc.all, gc.map);
		if (has_time(b) && gc.months != NULL)
		{
			sift_smonth_merge(&k->l1_months, b->spec.shape, gc.months, gc.map);
			months_note(k, gc.months, k->narrivals);
		}
		if (single)
			member_note(k, h->member.min, coded);
		else
		{
			k->mlo = k->nrefs == 0 ? h->member.min : Min(k->mlo, h->member.min);
			k->mhi = k->nrefs == 0 ? h->member.max : Max(k->mhi, h->member.max);
			k->mcount += Max(h->member_distinct, 1);
			k->members_unknown = true;
		}
		for (uint32 r = 0; r < b->nrings; r++)
		{
			const SiftScopeZone *z = ring_zone(h, r);

			if (!has_time(b) || !z->has_min_max || !sift_topk_wants(&k->ring[r], z->max))
				continue;
			if (!tail_read[r])
			{
				SRC_CALL(b, nt[r] = src->tail_ring(src->arg, gidx, r, SIFT_SCOPE_TOPK_CAP, tail[r]));
				tail_read[r] = true;
				b->stats.tail_reads++;
			}
			tail_offer(b, k, r, tail[r], nt[r], gidx);
		}
		ref_append(k, gidx, SIFT_SCOPE_PURE_GRANULE);
		close_ref(b, k);
	}
}

typedef struct Slot
{
	uint32		ord;
	uint32		n;
	uint32		cap;
	uint32	   *rows;
} Slot;

/* A granule of several members: the rows of each key become one thin cube */
static void
granule_cut(SiftScopeBuild *b, const SiftScopeGranSource *src, uint32 gidx, const SiftScopeGranHead *h, uint32 skip_ord)
{
	SiftScopeRows rows;
	const SiftScopeDictMap *map = NULL;
	Slot	   *slots = NULL;
	uint32		ns = 0,
				capslots = 0;
	int64		prev = 0;
	uint32		lo = 0,
				hi = 0;
	bool		have = false,
				coded = (b->spec.flags & SIFT_SMETA_CODED_MEM) != 0;

	memset(&rows, 0, sizeof(rows));
	rows.want_filter = (b->spec.flags & SIFT_SMETA_NULL_FILTERED) != 0;
	for (uint32 r = 1; r < b->nrings; r++)
		if (!(b->spec.flags & SIFT_SMETA_MULTISET) && h->extra_topk[r - 1].has_min_max)
			rows.want_extra |= 1U << (r - 1);
	SRC_CALL(b, src->rows(src->arg, gidx, &rows, &map));
	b->stats.row_decodes++;
	for (uint32 r = 0; r < rows.n; r++)
	{
		int64		m;

		if ((rows.member_null != NULL && rows.member_null[r]) || (rows.excluded != NULL && rows.excluded[r]))
			continue;
		m = rows.member[r];
		if (!have || m != prev)
		{
			have = true;
			prev = m;
			if (!sift_spm_find(b->pm, m, &lo, &hi))
				lo = hi = 0;
		}
		for (uint32 i = lo; i < hi; i++)
		{
			uint32		ord = b->pm->key_ordinals[i];
			Slot	   *s;

			if (ord == skip_ord || !key_takes(b, ord, gidx))
				continue;
			if (b->slot_of_key[ord] == UINT32_MAX)
			{
				if (ns == capslots)
				{
					capslots = capslots == 0 ? 8 : capslots * 2;
					slots = slots == NULL ? palloc(capslots * sizeof(Slot)) : repalloc(slots, capslots * sizeof(Slot));
				}
				b->slot_of_key[ord] = ns;
				slots[ns++] = (Slot) {ord, 0, 0, NULL};
			}
			s = &slots[b->slot_of_key[ord]];
			if (s->n == s->cap)
			{
				s->cap = s->cap == 0 ? 64 : s->cap * 2;
				s->rows = s->rows == NULL ? palloc(s->cap * sizeof(uint32)) : repalloc(s->rows, s->cap * sizeof(uint32));
			}
			s->rows[s->n++] = r;
		}
	}
	for (uint32 si = 0; si < ns; si++)
	{
		Slot	   *s = &slots[si];
		KeyAcc	   *k = key_acc(b, s->ord);
		SiftScopeCubeAcc thin;
		SiftScopeMonthTable tm;
		uint32		slice = SIFT_SCOPE_PURE_GRANULE;

		b->slot_of_key[s->ord] = UINT32_MAX;
		sift_scube_init(&thin, b->spec.shape);
		sift_smonth_init(&tm);
		sift_scube_add_rows(&thin, &rows, s->rows, s->n, map);
		if (has_time(b))
			sift_smonth_add_rows(&tm, b->spec.shape, &rows, s->rows, s->n, map);
		if (s->n != h->nrows && s->n < sift_scope_min_slice_rows(h->nrows))
			slice = SIFT_SCOPE_INLINE_SCAN;
		else if (s->n != h->nrows)
		{
			SiftScopeL0SliceEntry e = {gidx, s->n, b->l0pool.len, 0, thin.has_time ? thin.time_min : 0, thin.has_time ? thin.time_max : 0};

			sift_scube_write(&b->l0pool, &thin);
			e.cube_len = b->l0pool.len - e.cube_off;
			if (b->nslices == b->capslices)
			{
				b->capslices = b->capslices == 0 ? 64 : b->capslices * 2;
				b->slices = b->slices == NULL ? palloc(b->capslices * sizeof(*b->slices)) : repalloc(b->slices, b->capslices * sizeof(*b->slices));
			}
			b->slices[b->nslices++] = e;
			slice = b->spec.slice_base + b->nslices - 1;
		}
		sift_scube_merge(&k->l1_all, &thin, NULL);
		sift_smonth_merge(&k->l1_months, b->spec.shape, &tm, NULL);
		months_note(k, &tm, k->narrivals);
		for (uint32 i = 0; i < s->n; i++)
		{
			uint32		r = s->rows[i];

			if (rows.member_null != NULL && rows.member_null[r])
				continue;
			member_note(k, rows.member[r], coded);
			if (has_time(b) && !(b->spec.flags & SIFT_SMETA_MULTISET) && (rows.time_null == NULL || !rows.time_null[r]) &&
				sift_topk_wants(&k->ring[0], rows.time[r]))
				sift_topk_offer(&k->ring[0], rows.time[r], gidx, r);
			for (uint32 q = 1; q < b->nrings; q++)
				if ((rows.want_extra >> (q - 1) & 1) && (rows.extra_null[q - 1] == NULL || !rows.extra_null[q - 1][r]) &&
					sift_topk_wants(&k->ring[q], rows.extra[q - 1][r]))
					sift_topk_offer(&k->ring[q], rows.extra[q - 1][r], gidx, r);
		}
		ref_append(k, gidx, slice);
		close_ref(b, k);
		sift_scube_free(&thin);
		sift_smonth_free(&tm);
		pfree(s->rows);
	}
	if (slots != NULL)
		pfree(slots);
}

/* The rows of a granule the scope holds: all of them, or with a null filter those whose filter leaf is NULL */
static uint32
live_rows(const SiftScopeBuild *b, const SiftScopeGranHead *h)
{
	if (!(b->spec.flags & SIFT_SMETA_NULL_FILTERED))
		return h->nrows;
	SIFT_INVARIANT(h->has_filter_zone, "scope build: a granule of a null filtered scope has the zone of its filter leaf");
	return h->filter.nnull;
}

static void
note_homes(SiftScopeBuild *b, const SiftScopeGranSource *src, uint32 gidx, const SiftScopeGranHead *h)
{
	sift_homes_runs(&b->homes, src, gidx, h, b->caller);
	sift_homes_keep(&b->homes);
}

void
sift_scope_build_on_granule(SiftScopeBuild *b, const SiftScopeGranSource *src, uint32 gidx)
{
	SiftScopeGranHead h;
	uint32		lo,
				hi;
	IN_BUILD(b);

	memset(&h, 0, sizeof(h));
	SRC_CALL(b, src->head(src->arg, gidx, &h));
	b->stats.granules++;
	if (h.nrows > 0 && (b->spec.flags & SIFT_SMETA_CLUSTERED_PP))
		note_homes(b, src, gidx, &h);
	if (h.nrows == 0 || live_rows(b, &h) == 0)
		b->stats.skipped++;
	else if (live_rows(b, &h) < h.nrows)
	{
		b->stats.impure++;
		granule_cut(b, src, gidx, &h, UINT32_MAX);
	}
	else if (h.member.has_min_max && h.member.nnull == 0 && h.member.min == h.member.max)
	{
		if (sift_spm_find(b->pm, h.member.min, &lo, &hi))
		{
			b->stats.pure++;
			granule_whole(b, src, gidx, &h, b->pm->key_ordinals + lo, hi - lo);
		}
		else
			b->stats.skipped++;
	}
	else if ((b->spec.flags & SIFT_SMETA_CLUSTERED) && h.has_key_zone && h.key.nnull == 0 && h.key.min == h.key.max)
	{
		int32		ord = h.key.min == SIFT_SCOPE_UNASSIGNED_KEY ? -1 : sift_spm_key_ordinal(b->pm, h.key.min);
		uint32		o = (uint32) ord;
		uint32		fit;

		if (b->spec.flags & SIFT_SMETA_CLUSTERED_11)
			sift_homes_runs(&b->homes, src, gidx, &h, b->caller);
		fit = sift_homes_fit(&b->homes, b->pm, o);

		if (ord >= 0 && !(fit & SIFT_HOMES_LACK))
		{
			b->stats.pure++;
			granule_whole(b, src, gidx, &h, &o, 1);
		}
		if (fit != 0)
		{
			b->stats.impure++;
			granule_cut(b, src, gidx, &h, (fit & SIFT_HOMES_LACK) ? UINT32_MAX : o);
		}
		else if (ord < 0)
			b->stats.skipped++;
	}
	else if (h.member.has_min_max && !sift_spm_range_hits(b->pm, h.member.min, h.member.max))
		b->stats.skipped++;
	else
	{
		b->stats.impure++;
		granule_cut(b, src, gidx, &h, UINT32_MAX);
	}
	sift_sbld_evict_if_over(b);
	OUT_BUILD();
}

void
sift_scope_build_pass2(SiftScopeBuild *b, const SiftScopeGranSource *src)
{
	for (uint32 g = 0; g < b->total_granules; g++)
		sift_scope_build_on_granule(b, src, g);
}

void
sift_scope_build_note_homes(SiftScopeBuild *b, const SiftScopeGranSource *src, const uint32 *gidx, uint32 n)
{
	IN_BUILD(b);

	SIFT_INVARIANT(b->spec.flags & SIFT_SMETA_CLUSTERED_PP, "scope build: homes are noted for a scope clustered by primary key");
	for (uint32 i = 0; i < n; i++)
	{
		SiftScopeGranHead h;

		SIFT_INVARIANT(gidx[i] < b->total_granules, "scope build: granule %u is outside the parts", gidx[i]);
		memset(&h, 0, sizeof(h));
		SRC_CALL(b, src->head(src->arg, gidx[i], &h));
		b->stats.route_granules++;
		if (h.nrows > 0)
			note_homes(b, src, gidx[i], &h);
	}
	OUT_BUILD();
}

void
sift_scope_build_pass_list(SiftScopeBuild *b, const SiftScopeGranSource *src, const uint32 *gidx, uint32 n)
{
	for (uint32 i = 0; i < n; i++)
	{
		SIFT_INVARIANT(gidx[i] < b->total_granules && (i == 0 || gidx[i - 1] < gidx[i]), "scope build: a granule list that is not ascending or out of the parts");
		sift_scope_build_on_granule(b, src, gidx[i]);
	}
}

/* ---------------------------------------------------------------------------
 * 3. The end: sort the nodes, lay out the keys, write the object
 * ------------------------------------------------------------------------- */

static int
node_cmp(const void *a, const void *b)
{
	const NodeRec *x = a,
			   *y = b;

	if (x->pm_ord != y->pm_ord)
		return x->pm_ord < y->pm_ord ? -1 : 1;
	if (x->d.member_lo != y->d.member_lo)
		return x->d.member_lo < y->d.member_lo ? -1 : 1;
	return x->first_arrival < y->first_arrival ? -1 : x->first_arrival > y->first_arrival;
}

void
sift_scope_build_end(SiftScopeBuild *b, StringInfo out, SiftScopeBuildStats *stats)
{
	uint32		nrecs = 0,
				n = 0;
	SiftKeyRec *recs;
	SiftEmit	em;
	StringInfoData dict;
	bool		wide = b->total_granules > SIFT_TOPK_COMPACT_GRANS;
	MemoryContext old = MemoryContextSwitchTo(b->mcxt);

	for (uint32 i = 0; i < b->pm->nkeys; i++)
		if (b->acc[i] != NULL)
			flush_last_node(b, b->acc[i]);
	qsort(b->nodes, b->nnodes, sizeof(NodeRec), node_cmp);
	recs = palloc0(sizeof(*recs) * Max(b->nactive, 1));
	for (uint32 i = 0; i < b->pm->nkeys; i++)
	{
		KeyAcc	   *k = b->acc[i];
		SiftKeyRec *r;
		uint32		first = n;
		uint32	   *remap = palloc(sizeof(uint32) * Max(k == NULL ? 1 : k->narrivals, 1));
		uint32		final = 0;
		StringInfoData mm,
					l2;

		if (k == NULL)
			continue;
		r = &recs[nrecs++];
		while (n < b->nnodes && b->nodes[n].pm_ord == i)
			n++;
		r->nnodes = n - first;
		r->nodes = palloc(sizeof(SiftKeyNodeRec) * r->nnodes);
		for (uint32 j = 0; j < r->nnodes; j++)
		{
			const NodeRec *nd = &b->nodes[first + j];

			r->nodes[j].node = nd->d;
			r->nodes[j].block = b->l1pool.data + nd->d.ref_off;
			r->nodes[j].block_len = nd->block_len;
			r->nodes[j].block_off = nd->d.ref_off;
			for (uint32 q = 0; q < nd->d.nrefs; q++)
				remap[nd->first_arrival + q] = final++;
		}
		r->root.scope_key = k->scope_key;
		r->root.member_count = (uint32) Min(k->member_total, UINT32_MAX);
		r->root.key_epoch = b->spec.meta_epoch;
		if (r->nnodes == 1)
		{
			const SiftScopeL1Node *only = &r->nodes[0].node;

			r->root.row_count = only->row_count;
			r->root.time_min = only->time_min;
			r->root.time_max = only->time_max;
			r->root.flags |= SIFT_L2_SINGLE_L1;
			r->root.nmonths = r->nodes[0].node.nmonths;
			if (r->nodes[0].node.nmonths > 0)
				r->root.flags |= SIFT_L2_HAS_MONTHS;
		}
		else
		{
			r->root.row_count = k->l2_all.row_count;
			r->root.time_min = k->l2_all.has_time ? k->l2_all.time_min : 0;
			r->root.time_max = k->l2_all.has_time ? k->l2_all.time_max : 0;
			sift_sbld_unspill(b, k);
			if (k->l2_months.n > 0)
				r->root.flags |= SIFT_L2_HAS_MONTHS;
			initStringInfo(&l2);
			sift_l2_block(&l2, &r->root, &k->l2_all, &k->l2_months);
			r->l2block = l2.data;
			r->l2block_len = l2.len;
			r->l2block_off = 0;
			r->root.all_cube_off = 0;
		}
		{
			bool		any = false;

			for (uint32 q = 0; q < b->nrings; q++)
				any |= k->ring[q].n > 0;
			if (any)
			{
				StringInfoData slot;

				initStringInfo(&slot);
				sift_rings_write(k->ring, b->nrings, 0, wide, &slot, &r->root);
				r->topk = slot.data;
				r->topk_wide = wide;
			}
		}
		if (k->mmap.n > 0 && final > 1)
		{
			initStringInfo(&mm);
			sift_mmap_write(&k->mmap, k->narrivals, remap, &mm);
			r->monthmap = mm.data;
			r->root.monthmap_len = mm.len;
			r->root.flags |= SIFT_L2_HAS_MONTHMAP;
		}
		pfree(remap);
		sift_scube_free(&k->l2_all);
		sift_smonth_free(&k->l2_months);
		sift_mmap_free(&k->mmap);
	}
	initStringInfo(&dict);
	sift_sdict_write(&b->dict, &dict);
	memset(&em, 0, sizeof(em));
	em.hdr.scope_ord = b->spec.scope_ord;
	em.hdr.njournals = (b->spec.flags & SIFT_SMETA_TWO_HOP) ? 3 : 1;
	em.hdr.flags = b->spec.flags;
	em.hdr.member_leaf = b->spec.member_leaf;
	em.hdr.time_leaf = b->spec.time_leaf;
	em.hdr.nparts = b->nparts;
	em.hdr.total_granules = b->total_granules;
	em.hdr.max_part_gen = b->nparts > 0 ? b->parts[b->nparts - 1].part_gen : 0;
	em.hdr.schema_hash = b->spec.schema_hash;
	em.hdr.meta_epoch = b->spec.meta_epoch;
	memcpy(em.hdr.stamps, b->spec.stamps, sizeof(em.hdr.stamps));
	if (b->spec.flags & SIFT_SMETA_NULL_FILTERED)
		em.hdr.reserved[1] = (uint64) b->spec.null_filter_leaf;
	if (b->nrings > 1)
	{
		SiftScopeTopKDesc d = {(uint8) b->nrings, (uint8) ((1U << b->nrings) - 1), {-1, -1, -1}};

		for (uint32 q = 1; q < b->nrings; q++)
			d.extra_leaf[q - 1] = b->spec.extra_leaf[q - 1];
		memcpy(&em.hdr.reserved[0], &d, sizeof(d));
	}
	if (b->homes.nhomes > 0)
	{
		StringInfoData route;

		initStringInfo(&route);
		sift_ent_route_write(&route, b->homes.homes, b->homes.nhomes, em.hdr.max_part_gen);
		em.route = route.data;
		em.route_len = route.len;
	}
	em.parts = b->parts;
	em.l0_pool = b->l0pool.data;
	em.l0_pool_len = b->l0pool.len;
	em.l0_entries = b->slices;
	em.l0_nentries = b->nslices;
	em.dict = dict.data;
	em.dict_len = dict.len;
	MemoryContextSwitchTo(old);
	sift_emit_object(out, &em, recs, nrecs);
	if (stats != NULL)
		*stats = b->stats;
	MemoryContextDelete(b->mcxt);
}
