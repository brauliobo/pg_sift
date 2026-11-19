/*
 * core/dirchain.c - the chained directory (core/page.h, SiftChainRootData): reading it and writing the pages of an update.
 *
 * A chain is immutable: an update writes the leaves whose entries changed and a new root, each page once and logged before anything refers to it, and
 * the caller (store_update) flips the metapage in one record; what the old chain does not share with the new one is released through the free list.
 * Every function runs with the metapage and the directory locked (shared to read, exclusive to write), the same locks that guard block 1.
 */
#include "core/host.h"

#include "storage/bufmgr.h"
#include "access/xlog.h"

#include "core/store_int.h"
#include "core/dirchain.h"
#include "core/range.h"
#include "core/restrict.h"

#ifdef SIFT_TESTING
int			sift_test_dir_flat_max = 0;
int			sift_test_dir_leaf_max = 0;
int			sift_test_dir_root_max = 0;
#endif

static uint32
leaf_cap(void)
{
#ifdef SIFT_TESTING
	if (sift_test_dir_leaf_max > 0)
		return Min(SIFT_CHAIN_LEAF_CAP, (uint32) sift_test_dir_leaf_max);
#endif
	return SIFT_CHAIN_LEAF_CAP;
}

uint32
sift_dirchain_leaf_cap(void)
{
	return leaf_cap();
}

static uint32
root_cap(void)
{
#ifdef SIFT_TESTING
	if (sift_test_dir_root_max > 0)
		return Min(SIFT_CHAIN_ROOT_CAP, (uint32) sift_test_dir_root_max);
#endif
	return SIFT_CHAIN_ROOT_CAP;
}

int
sift_dir_flat_max(void)
{
#ifdef SIFT_TESTING
	if (sift_test_dir_flat_max > 0)
		return Min(SIFT_DIR_FLAT_MAX, sift_test_dir_flat_max);
#endif
	return SIFT_DIR_FLAT_MAX;
}

/* A chain page in a buffer locked by the caller: the layout, the type and the version it was written under */
static void
check_chain_page(Relation index, Buffer buf, SiftPageType type, uint64 gen)
{
	Page		page = BufferGetPage(buf);
	SiftPageOpaque op;

	if (!sift_page_layout_ok(page))
		corrupt(index, "Block %u of the directory chain has an invalid page layout.", BufferGetBlockNumber(buf));
	op = SiftPageGetOpaque(page);
	if (op->type == type && op->gen == gen && op->flags == 0 && op->reserved == 0)
		return;
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("sift directory recycled during scan, retry"),
				 errdetail("Index \"%s\", block %u.", RelationGetRelationName(index), BufferGetBlockNumber(buf)),
				 errhint("Set hot_standby_feedback = on on the standby to avoid this.")));
	corrupt(index, "Block %u of the directory chain: expected page type %d version %llu, found type %d version %llu.",
			BufferGetBlockNumber(buf), type, ULL(gen), op->type, ULL(op->gen));
}

static void
chain_add_block(SiftDirChain *c, BlockNumber blk)
{
	if (c->nblocks == c->maxblocks)
	{
		c->maxblocks = Max(16, 2 * c->maxblocks);
		c->blocks = c->blocks ? repalloc(c->blocks, c->maxblocks * sizeof(BlockNumber)) : palloc(c->maxblocks * sizeof(BlockNumber));
	}
	c->blocks[c->nblocks++] = blk;
}

static void
check_refs(Relation index, const SiftChainRootData *r, Buffer buf, uint32 level, BlockNumber blk, bool empty_ok)
{
	if (r->level != level || (r->nrefs == 0 && !empty_ok) || r->nrefs > root_cap() || r->nsegs > SIFT_DIR_WIDE_MAX ||
		SiftPageGetUsed(BufferGetPage(buf)) != SIFT_CHAIN_ROOT_USED(r->nrefs))
		corrupt(index, "Directory page %u is of level %u and holds %u references and %u entries in %u bytes, this build reads level %u.", blk, r->level,
				r->nrefs, r->nsegs, SiftPageGetUsed(BufferGetPage(buf)), level);
}

static bool
bound_is_zero(const uint8 *lo)
{
	for (int i = 0; i < SIFT_BOUND_BYTES; i++)
		if (lo[i] != 0)
			return false;
	return true;
}

static int
cmp_u64(const void *a, const void *b)
{
	return pg_cmp_u64(*(const uint64 *) a, *(const uint64 *) b);
}

/* A lineage names one partition: no two leaves of a chain carry the same */
static void
check_lineages(Relation index, const SiftDirChain *chain, BlockNumber root)
{
	uint64	   *ids = palloc(Max(chain->nleaves, 1) * sizeof(uint64));
	int			k = 0;

	for (uint32 i = 0; i < chain->nleaves; i++)
		if (chain->leaf[i].lineage != 0)
			ids[k++] = chain->leaf[i].lineage;
	qsort(ids, k, sizeof(uint64), cmp_u64);
	for (int i = 1; i < k; i++)
		if (ids[i] == ids[i - 1])
			corrupt(index, "Directory root %u lists the partition " UINT64_FORMAT " twice.", root, ids[i]);
	pfree(ids);
}

/* The lower bounds of n references, unpacked */
static SiftBound *
ref_bounds(Relation index, const SiftChainRef *ref, uint32 n)
{
	SiftBound  *b = palloc(Max(n, 1) * sizeof(SiftBound));

	for (uint32 i = 0; i < n; i++)
		if (!sift_bound_unpack(ref[i].lo, &b[i]))
			corrupt(index, "The lower bound of directory reference %u is no bound.", i);
	return b;
}

static const SiftBound *
bound_of(const void *bounds, uint32 i)
{
	return &((const SiftBound *) bounds)[i];
}

/* The span [*from, *to) of the n references, ascending by their lower bounds, whose ranges may hold a key of the range (core/range.h) */
static void
ref_span(Relation index, const SiftChainRef *ref, uint32 n, const SiftKeyRange *keys, uint32 *from, uint32 *to)
{
	SiftBound  *b = ref_bounds(index, ref, n);

	sift_range_span(b, bound_of, n, &keys->keys, keys->shared, from, to);
	pfree(b);
}

/* How many of the first references have the empty bound: the entries of no partition, and the partition that starts at the lowest key */
static uint32
zero_bounds(const SiftChainRef *ref, uint32 n)
{
	uint32		z = 0;

	while (z < n && bound_is_zero(ref[z].lo))
		z++;
	return z;
}

/*
 * The entries of the chain whose root is root, written under version, into *segs (allocated for the entries and room more), and the chain itself.
 * The caller holds the metapage and the directory, so no update runs; the pages are read once each, under a share lock.
 * With a range of keys only the entries of no partition and of the partitions that may hold a key of the range are read (the chain then lists those leaves): the root
 * says by their bounds which interior pages and leaves, and the others are not touched.
 */
static int
chain_read(Relation index, BlockNumber root, uint64 version, int room, SiftSegEntry **segs, SiftDirChain *chain, const SiftKeyRange *keys)
{
	Buffer		buf = read_locked(index, root, BUFFER_LOCK_SHARE);
	SiftChainRootData *r = (SiftChainRootData *) SiftPageGetData(BufferGetPage(buf));
	SiftChainRef *top;
	SiftChainRef *refs;
	uint32		ntop;
	uint32		nrefs = 0;
	uint32		nsegs;
	uint32		level;
	uint32		pages_from = 0,
				pages_to,
				z,
				k0,
				from = 0,
				to;
	uint32		leaf_end = 0;
	int			n = 0;
	bool		in_partitions = false;
	SiftChainLeafData *leaf = NULL;

	check_chain_page(index, buf, SIFT_PT_DIRROOT, version);
	level = r->level;
	if (level < 1 || level > SIFT_CHAIN_LEVELS_MAX)
		corrupt(index, "Directory root %u is of level %u, this build reads up to level %d.", root, level, SIFT_CHAIN_LEVELS_MAX);
	check_refs(index, r, buf, level, root, level == 1);
	ntop = r->nrefs;
	nsegs = r->nsegs;
	top = palloc(ntop * sizeof(SiftChainRef));
	memcpy(top, r->ref, ntop * sizeof(SiftChainRef));
	*chain = (SiftDirChain) {.root = root,.nall = nsegs,.inner = palloc((level > 1 ? ntop : 1) * sizeof(SiftDirInner))};
	chain_add_block(chain, root);
	UnlockReleaseBuffer(buf);
	*segs = palloc(Max(nsegs + room, 1) * sizeof(SiftSegEntry));
	z = level > 1 ? zero_bounds(top, ntop) : 0;
	pages_to = ntop;
	if (keys != NULL && level > 1)
	{
		ref_span(index, top + z, ntop - z, keys, &pages_from, &pages_to);
		pages_from += z;
		pages_to += z;
	}
	refs = level == 1 ? top : palloc(Max(Min(nsegs, ntop * root_cap()), 1) * sizeof(SiftChainRef));
	nrefs = level == 1 ? ntop : 0;
	for (uint32 t = 0; level > 1 && t < ntop; t++)
	{
		const SiftChainRef *inner = &top[t];
		SiftChainRootData *ir;
		uint32		below = 0;

		if (t >= z && (t < pages_from || t >= pages_to))
			continue;
		if (inner->lineage != 0)
			corrupt(index, "Directory interior reference %u of root %u has the lineage " UINT64_FORMAT ".", t, root, inner->lineage);
		buf = read_locked(index, inner->blk, BUFFER_LOCK_SHARE);
		check_chain_page(index, buf, SIFT_PT_DIRROOT, inner->gen);
		ir = (SiftChainRootData *) SiftPageGetData(BufferGetPage(buf));
		check_refs(index, ir, buf, 1, inner->blk, false);
		if (ir->nsegs != inner->n || memcmp(ir->ref[0].lo, inner->lo, SIFT_BOUND_BYTES) != 0)
			corrupt(index, "Directory interior page %u holds %u entries from another bound, its root says %u.", inner->blk, ir->nsegs, inner->n);
		for (uint32 i = 0; i < ir->nrefs; i++)
			below += ir->ref[i].n;
		if (below != inner->n)
			corrupt(index, "Directory interior page %u says %u entries, its leaves hold %u.", inner->blk, inner->n, below);
		memcpy(refs + nrefs, ir->ref, ir->nrefs * sizeof(SiftChainRef));
		chain->inner[chain->ninner++] = (SiftDirInner) {.ref = *inner,.leaf0 = nrefs};
		nrefs += ir->nrefs;
		UnlockReleaseBuffer(buf);
		chain_add_block(chain, inner->blk);
	}
	k0 = 0;
	while (k0 < nrefs && refs[k0].lineage == 0)
		k0++;
	to = nrefs - k0;
	if (keys != NULL)
		ref_span(index, refs + k0, nrefs - k0, keys, &from, &to);
	chain->leaf = palloc(Max(nrefs, 1) * sizeof(SiftChainRef));
	buf = InvalidBuffer;
	for (uint32 i = 0; i < nrefs; i++)
	{
		const SiftChainRef *ref = &refs[i];

		if (i >= k0 && (i - k0 < from || i - k0 >= to))
			continue;
		if (ref->n == 0 || ref->n > SIFT_CHAIN_LEAF_CAP || (uint32) n + ref->n > nsegs)
			corrupt(index, "Directory reference %u of root %u holds %u of the %u entries.", chain->nleaves, root, ref->n, nsegs);
		if (ref->lineage == 0 ? in_partitions || !bound_is_zero(ref->lo) :
			chain->nleaves > 0 && in_partitions && memcmp(chain->leaf[chain->nleaves - 1].lo, ref->lo, SIFT_BOUND_BYTES) > 0)
			corrupt(index, "Directory reference %u of root %u breaks the order of its partitions (lineage " UINT64_FORMAT ").", chain->nleaves, root, ref->lineage);
		in_partitions |= ref->lineage != 0;
		if (chain->nleaves == 0 || chain->leaf[chain->nleaves - 1].blk != ref->blk)
		{
			if (BufferIsValid(buf))
				UnlockReleaseBuffer(buf);
			buf = read_locked(index, ref->blk, BUFFER_LOCK_SHARE);
			check_chain_page(index, buf, SIFT_PT_DIRLEAF, ref->gen);
			leaf = (SiftChainLeafData *) SiftPageGetData(BufferGetPage(buf));
			if (SiftPageGetUsed(BufferGetPage(buf)) != SIFT_CHAIN_LEAF_USED(leaf->n))
				corrupt(index, "Directory leaf %u holds %u entries in %u bytes.", ref->blk, leaf->n, SiftPageGetUsed(BufferGetPage(buf)));
			chain_add_block(chain, ref->blk);
			leaf_end = keys == NULL ? 0 : ref->at;
		}
		if ((keys == NULL ? ref->at != leaf_end : ref->at < leaf_end) || (uint32) ref->at + ref->n > leaf->n)
			corrupt(index, "Directory leaf %u of root %u holds %u entries, a reference takes %u from %u.", ref->blk, root, leaf->n, ref->n, ref->at);
		memcpy(*segs + n, leaf->segs + ref->at, ref->n * sizeof(SiftSegEntry));
		n += ref->n;
		leaf_end = ref->at + ref->n;
		chain->leaf[chain->nleaves++] = *ref;
		if (keys == NULL && (i + 1 == nrefs || refs[i + 1].blk != ref->blk) && leaf_end != leaf->n)
			corrupt(index, "Directory leaf %u holds %u entries, its references take %u.", ref->blk, leaf->n, leaf_end);
	}
	if (BufferIsValid(buf))
		UnlockReleaseBuffer(buf);
	pfree(top);
	if (level > 1)
		pfree(refs);
	check_lineages(index, chain, root);
	if (keys == NULL && (uint32) n != nsegs)
		corrupt(index, "Directory root %u says %u entries, its leaves hold %d.", root, nsegs, n);
	return n;
}

int
sift_dirchain_read(Relation index, BlockNumber root, uint64 version, int room, SiftSegEntry **segs, SiftDirChain *chain)
{
	return chain_read(index, root, version, room, segs, chain, NULL);
}

int
sift_dirchain_read_range(Relation index, BlockNumber root, uint64 version, const SiftKeyRange *keys, SiftSegEntry **segs, SiftDirChain *chain)
{
	return chain_read(index, root, version, 0, segs, chain, keys);
}

int
sift_dirchain_parts(const SiftDirChain *chain, SiftDirPart **parts)
{
	int			np = 0,
				at = 0;

	*parts = palloc(Max(chain->nleaves, 1) * sizeof(SiftDirPart));
	for (uint32 k = 0; k < chain->nleaves; at += chain->leaf[k].n, k++)
		if (chain->leaf[k].lineage != 0)
		{
			SiftDirPart *p = &(*parts)[np++];

			p->lineage = chain->leaf[k].lineage;
			p->first = at;
			p->n = chain->leaf[k].n;
			memcpy(p->lo, chain->leaf[k].lo, SIFT_BOUND_BYTES);
		}
	return np;
}

typedef struct GenPos
{
	uint64		gen;
	int			pos;
} GenPos;

static int
cmp_genpos(const void *a, const void *b)
{
	return pg_cmp_u64(((const GenPos *) a)->gen, ((const GenPos *) b)->gen);
}

/* The new leaves and root of one update, in the order they are written */
typedef struct ChainOut
{
	Relation	index;
	Buffer		metabuf;
	uint64		version;
	BlockNumber below;			/* the pages go below this block (InvalidBlockNumber: anywhere) */
	SiftChainRef *ref;
	int			nref;
	Buffer		pending[MAX_GENERIC_XLOG_PAGES];
	Page		pending_page[MAX_GENERIC_XLOG_PAGES];
	PGIOAlignedBlock images[MAX_GENERIC_XLOG_PAGES];
	int			npending;
} ChainOut;

static void
flush_pending(ChainOut *o)
{
	if (o->npending == 0)
		return;
	log_pages(o->index, o->pending, o->pending_page, o->npending);
	for (int i = 0; i < o->npending; i++)
		UnlockReleaseBuffer(o->pending[i]);
	o->npending = 0;
}

/* A new page of the chain: a block from the free list or the end of the file, its image in the next slot of the record; the page is laid out by fill */
static BlockNumber
put_page(ChainOut *o, SiftPageType type, Page *page)
{
	uint32		got;
	BlockNumber blk;

	if (o->npending == MAX_GENERIC_XLOG_PAGES)
		flush_pending(o);
	if (BlockNumberIsValid(o->below))
	{
		blk = alloc_blocks_below(o->index, o->metabuf, 1, 1, o->below, &got);
		if (!BlockNumberIsValid(blk))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("sift index \"%s\" has no free block below block %u for its directory", RelationGetRelationName(o->index), o->below),
					 errhint("The pages of the directory move once the blocks released by earlier steps are reusable.")));
	}
	else
		blk = alloc_blocks(o->index, o->metabuf, 1, 1, &got);
	o->pending[o->npending] = new_locked(o->index, blk);
	*page = o->pending_page[o->npending] = o->images[o->npending].data;
	sift_page_init(*page, type, o->version);
	o->npending++;
	return blk;
}

/*
 * The groups g[a..b) in one leaf, a reference each. A group is the entries one reference names: a piece of the entries of no partition, or one partition. The groups of a
 * leaf are consecutive in the directory, so their entries are one run of segs.
 */
static void
put_leaf(ChainOut *o, const SiftSegEntry *segs, const SiftDirPart *g, int a, int b)
{
	Page		page;
	SiftChainLeafData *leaf;
	BlockNumber blk = put_page(o, SIFT_PT_DIRLEAF, &page);
	uint32		first = g[a].first;

	leaf = (SiftChainLeafData *) SiftPageGetData(page);
	leaf->n = g[b - 1].first + g[b - 1].n - first;
	memcpy(leaf->segs, segs + first, leaf->n * sizeof(SiftSegEntry));
	SiftPageSetUsed(page, SIFT_CHAIN_LEAF_USED(leaf->n));
	for (int i = a; i < b; i++)
	{
		SiftChainRef *ref = &o->ref[o->nref++];

		*ref = (SiftChainRef) {.blk = blk,.n = g[i].n,.at = g[i].first - first,.gen = o->version,.lineage = g[i].lineage};
		memcpy(ref->lo, g[i].lo, SIFT_BOUND_BYTES);
	}
}

/* The groups g[a..b) in the fewest leaves that take them whole, in order */
static void
put_leaves(ChainOut *o, const SiftSegEntry *segs, const SiftDirPart *g, int a, int b)
{
	while (a < b)
	{
		int			e = a;
		uint32		used = 0;

		while (e < b && used + g[e].n <= leaf_cap())
			used += g[e++].n;
		put_leaf(o, segs, g, a, e);
		a = e;
	}
}

/* A page of references [from, to) of ref, written as a level of its own: the references it holds are n entries below it */
static BlockNumber
put_refs(ChainOut *o, const SiftChainRef *ref, int from, int to, uint32 level)
{
	Page		page;
	SiftChainRootData *r;
	BlockNumber blk = put_page(o, SIFT_PT_DIRROOT, &page);

	r = (SiftChainRootData *) SiftPageGetData(page);
	r->nrefs = to - from;
	r->level = level;
	r->nsegs = 0;
	for (int i = from; i < to; i++)
		r->nsegs += ref[i].n;
	memcpy(r->ref, ref + from, (to - from) * sizeof(SiftChainRef));
	SiftPageSetUsed(page, SIFT_CHAIN_ROOT_USED(r->nrefs));
	return blk;
}

/* New interior pages over the references [from, to) of o->ref, as even as a page holds them, appended to inner */
static void
put_interiors(ChainOut *o, SiftChainRef *inner, int *ninner, int from, int to)
{
	int			n = to - from;
	int			pages = (n + root_cap() - 1) / root_cap();

	for (int i = 0; i < pages; i++)
	{
		int			lo = from + (int) ((int64) n * i / pages),
					hi = from + (int) ((int64) n * (i + 1) / pages);
		SiftChainRef *page = &inner[(*ninner)++];

		*page = (SiftChainRef) {.blk = put_refs(o, o->ref, lo, hi, 1),.gen = o->version};
		for (int k = lo; k < hi; k++)
			page->n += o->ref[k].n;
		memcpy(page->lo, o->ref[lo].lo, SIFT_BOUND_BYTES);
	}
}

/* Where the leaf reference ref stands in o->ref at or after from: -1 when it does not */
static int
ref_at(const ChainOut *o, int from, const SiftChainRef *ref)
{
	for (int i = from; i < o->nref; i++)
		if (o->ref[i].blk == ref->blk && o->ref[i].at == ref->at)
			return i;
	return -1;
}

/* The pages the references [from, to) of o->ref make */
static int
interiors_for(int from, int to)
{
	return (to - from + (int) root_cap() - 1) / (int) root_cap();
}

/*
 * The pages over the leaves o->ref lists: the root, or a root over interior pages when the leaves are more than a root holds. An interior page of old that is at least half full and
 * whose leaves o->ref holds as they were, together and in order, stays (kept_inner); the references between two such pages go to new ones, as the leaves between kept leaves do. When
 * that makes more pages than a root holds, none stays and the pages are as even as they can be.
 */
static void
put_top(ChainOut *o, Relation index, int n, const SiftDirChain *old, bool *kept_inner, BlockNumber *rootblk)
{
	SiftChainRef *inner;
	int		   *at = palloc(((old ? old->ninner : 0) + 1) * sizeof(int));
	int			ninner = 0,
				pos = 0,
				npages = 0;

	if ((uint32) o->nref <= root_cap())
	{
		*rootblk = put_refs(o, o->ref, 0, o->nref, 1);
		pfree(at);
		return;
	}
	for (uint32 j = 0; old && j < old->ninner; j++)
	{
		const SiftDirInner *oi = &old->inner[j];
		uint32		m = (j + 1 < old->ninner ? old->inner[j + 1].leaf0 : old->nleaves) - oi->leaf0;
		int			found = ref_at(o, pos, &old->leaf[oi->leaf0]);

		at[j] = -1;
		if (m * 2 < root_cap() || found < 0 || (uint32) found + m > (uint32) o->nref || (BlockNumberIsValid(o->below) && oi->ref.blk >= o->below) ||
			memcmp(o->ref + found, old->leaf + oi->leaf0, m * sizeof(SiftChainRef)) != 0)
			continue;
		npages += 1 + interiors_for(pos, found);
		at[j] = found;
		pos = found + m;
	}
	npages += interiors_for(pos, o->nref);
	if ((uint32) npages > root_cap())
	{
		for (uint32 j = 0; old && j < old->ninner; j++)
			at[j] = -1;
		npages = interiors_for(0, o->nref);
	}
	if ((uint32) npages > root_cap())
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift index \"%s\" cannot hold %d segments", RelationGetRelationName(index), n),
				 errhint("Run sift.merge() on the index.")));
	inner = palloc(npages * sizeof(SiftChainRef));
	pos = 0;
	for (uint32 j = 0; old && j < old->ninner; j++)
		if (at[j] >= 0)
		{
			uint32		m = (j + 1 < old->ninner ? old->inner[j + 1].leaf0 : old->nleaves) - old->inner[j].leaf0;

			put_interiors(o, inner, &ninner, pos, at[j]);
			inner[ninner++] = old->inner[j].ref;
			kept_inner[j] = true;
			pos = at[j] + m;
		}
	put_interiors(o, inner, &ninner, pos, o->nref);
	*rootblk = put_refs(o, inner, 0, ninner, 2);
	pfree(inner);
	pfree(at);
}

/* The entries [a, b) of no partition as groups: in as many even pieces as a leaf asks for */
static void
fresh_groups(SiftDirPart *g, int *ng, int a, int b)
{
	int			pieces = (b - a + (int) leaf_cap() - 1) / (int) leaf_cap();

	for (int i = 0; i < pieces; i++)
		g[(*ng)++] = (SiftDirPart) {.first = a + (int64) (b - a) * i / pieces,.n = a + (int64) (b - a) * (i + 1) / pieces - (a + (int64) (b - a) * i / pieces)};
}

/*
 * The groups of the new directory: the first head entries are of no partition, in the groups the old chain made of them while the entries of a group (at least half a leaf) are as they were and
 * in pieces of even size between those, so that a directory that grows or shrinks by single entries keeps its leaves and its root does not fill with small groups; then the partitions.
 */
static SiftDirPart *
dir_groups(const SiftDirChain *old, const SiftSegEntry *oldsegs, const SiftSegEntry *segs, int n, const GenPos *by_gen, int head, const SiftDirPart *parts, int nparts,
		   int *ng)
{
	SiftDirPart *g = palloc0((head / (int) leaf_cap() + 2 * (old ? (int) old->nleaves : 0) + nparts + 3) * sizeof(SiftDirPart));
	int			pos = 0;
	int			off = 0;

	*ng = 0;
	for (uint32 k = 0; old && k < old->nleaves && old->leaf[k].lineage == 0; off += old->leaf[k].n, k++)
	{
		const SiftChainRef *ref = &old->leaf[k];
		GenPos		key = {oldsegs[off].gen, 0};
		const GenPos *hit = bsearch(&key, by_gen, n, sizeof(GenPos), cmp_genpos);

		if (ref->n * 2 < leaf_cap() || hit == NULL || hit->pos < pos || hit->pos + ref->n > head ||
			memcmp(segs + hit->pos, oldsegs + off, ref->n * sizeof(SiftSegEntry)) != 0)
			continue;
		fresh_groups(g, ng, pos, hit->pos);
		g[(*ng)++] = (SiftDirPart) {.first = hit->pos,.n = ref->n};
		pos = hit->pos + ref->n;
	}
	fresh_groups(g, ng, pos, head);
	if (nparts > 0)
		memcpy(g + *ng, parts, nparts * sizeof(SiftDirPart));
	*ng += nparts;
	return g;
}

/* The group of g that starts at entry first: -1 when none does */
static int
group_at(const SiftDirPart *g, int ng, int first)
{
	int			lo = 0,
				hi = ng - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (g[mid].first == first)
			return mid;
		if (g[mid].first < first)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/* The group of g that the entries of old reference ref (the entries at oldsegs) are, with all they were: -1 when something of them changed */
static int
group_of(const SiftDirPart *g, int ng, const GenPos *by_gen, int n, const SiftSegEntry *segs, const SiftSegEntry *oldsegs, const SiftChainRef *ref)
{
	GenPos		key = {oldsegs[0].gen, 0};
	const GenPos *hit = bsearch(&key, by_gen, n, sizeof(GenPos), cmp_genpos);
	int			at = hit ? group_at(g, ng, hit->pos) : -1;

	if (at < 0 || g[at].n != ref->n || g[at].lineage != ref->lineage || memcmp(g[at].lo, ref->lo, SIFT_BOUND_BYTES) != 0 ||
		memcmp(segs + g[at].first, oldsegs, ref->n * sizeof(SiftSegEntry)) != 0)
		return -1;
	return at;
}

/* A leaf of the old chain that the new one may keep: its references name the groups g0, g0 + 1, ..., as they were */
typedef struct OldLeaf
{
	uint32		ref0;
	uint32		nref;
	int			g0;
	uint32		size;
	bool		keep;
} OldLeaf;

/* The entries of the groups next to the kept ones, on the side of group at (dir -1: before it and down, +1: from it and up) up to the next kept group */
static uint32
open_entries(const SiftDirPart *g, const int *keep_ref, int ng, int at, int dir)
{
	uint32		n = 0;

	for (; at >= 0 && at < ng && keep_ref[at] < 0; at += dir)
		n += g[at].n;
	return n;
}

/*
 * Which groups keep the reference and the leaf of the old chain (keep_ref: the old reference, or -1): a leaf whose every reference names a group of the new directory as it
 * was, together and in order, and that is not at or above below. A kept leaf that stands next to groups written anew goes into them when it is under half full or when the
 * two fit one leaf, which costs no more pages written and keeps a directory of a hundred partitions in one leaf however its changes came.
 */
static void
plan_keep(const SiftDirChain *old, const SiftSegEntry *oldsegs, const SiftSegEntry *segs, int n, const GenPos *by_gen, const SiftDirPart *g, int ng, BlockNumber below,
		  int *keep_ref)
{
	OldLeaf    *leaves = palloc(Max(old->nleaves, 1) * sizeof(OldLeaf));
	int			nleaves = 0;
	int			off = 0;

	for (uint32 k = 0; k < old->nleaves;)
	{
		OldLeaf		l = {.ref0 = k,.g0 = -1,.keep = !(BlockNumberIsValid(below) && old->leaf[k].blk >= below)};

		for (; k < old->nleaves && old->leaf[k].blk == old->leaf[l.ref0].blk; k++)
		{
			int			at = l.keep ? group_of(g, ng, by_gen, n, segs, oldsegs + off, &old->leaf[k]) : -1;

			l.keep = at >= 0 && (k == l.ref0 || at == l.g0 + (int) (k - l.ref0));
			l.g0 = k == l.ref0 ? at : l.g0;
			l.size += old->leaf[k].n;
			off += old->leaf[k].n;
		}
		l.nref = k - l.ref0;
		leaves[nleaves++] = l;
	}
	for (int c = 0; c < nleaves; c++)
		for (uint32 j = 0; leaves[c].keep && j < leaves[c].nref; j++)
			keep_ref[leaves[c].g0 + j] = leaves[c].ref0 + j;
	for (bool again = true; again;)
	{
		again = false;
		for (int c = 0; c < nleaves; c++)
		{
			OldLeaf    *l = &leaves[c];
			uint32		left = l->keep ? open_entries(g, keep_ref, ng, l->g0 - 1, -1) : 0,
						right = l->keep ? open_entries(g, keep_ref, ng, l->g0 + l->nref, 1) : 0;

			bool		absorbed = (left > 0 && left + l->size <= leaf_cap()) || (right > 0 && right + l->size <= leaf_cap());

			if (!l->keep || (left == 0 && right == 0) || (l->size * 2 >= leaf_cap() && !absorbed))
				continue;
			for (uint32 j = 0; j < l->nref; j++)
				keep_ref[l->g0 + j] = -1;
			l->keep = false;
			again = true;
		}
	}
	pfree(leaves);
}

/*
 * Writes the chain of segs (n entries, the first head of them of no partition, then the nparts partitions) that follows old (the chain being replaced, NULL when the directory
 * was block 1 alone), whose entries were oldsegs. Groups of entries (a piece of the entries of no partition, a partition) are put in leaves whole, several to a leaf, a reference each.
 * A leaf of old stays as it is while all it names is as it was (plan_keep); the groups between two such leaves go to new leaves; interior pages stay as leaves do (put_top). With below,
 * a leaf at or above it is written again, and so is every page the update writes, below it. The metapage is locked exclusively (the blocks come from the free list under it). The
 * blocks of old that the new chain does not keep go to release (room for old->nleaves + old->ninner + 1); returns the new root, which nothing refers to yet.
 */
BlockNumber
sift_dirchain_write(Relation index, Buffer metabuf, uint64 version, const SiftDirChain *old, const SiftSegEntry *oldsegs,
					const SiftSegEntry *segs, int n, const SiftDirPart *parts, int nparts, BlockNumber below, SiftExtent *release, int *nrelease)
{
	ChainOut	o = {.index = index,.metabuf = metabuf,.version = version,.below = below};
	bool	   *kept = palloc0(((old ? old->nleaves : 0) + 1) * sizeof(bool));
	bool	   *kept_inner = palloc0(((old ? old->ninner : 0) + 1) * sizeof(bool));
	GenPos	   *by_gen = palloc(Max(n, 1) * sizeof(GenPos));
	SiftDirPart *g;
	int			ng;
	int		   *keep_ref;
	BlockNumber rootblk;

	for (int i = 0; i < n; i++)
		by_gen[i] = (GenPos) {segs[i].gen, i};
	qsort(by_gen, n, sizeof(GenPos), cmp_genpos);
	g = dir_groups(old, oldsegs, segs, n, by_gen, nparts > 0 ? (int) parts[0].first : n, parts, nparts, &ng);
	keep_ref = palloc(Max(ng, 1) * sizeof(int));
	memset(keep_ref, -1, ng * sizeof(int));
	if (old)
		plan_keep(old, oldsegs, segs, n, by_gen, g, ng, below, keep_ref);
	o.ref = palloc((ng + 1) * sizeof(SiftChainRef));
	for (int i = 0; i < ng;)
	{
		int			j = i;

		if (keep_ref[i] >= 0)
		{
			o.ref[o.nref++] = old->leaf[keep_ref[i]];
			kept[keep_ref[i++]] = true;
			continue;
		}
		while (j < ng && keep_ref[j] < 0)
			j++;
		put_leaves(&o, segs, g, i, j);
		i = j;
	}
	put_top(&o, index, n, old, kept_inner, &rootblk);
	flush_pending(&o);
	if (old)
	{
		release[(*nrelease)++] = (SiftExtent) {old->root, 1};
		for (uint32 k = 0; k < old->ninner; k++)
			if (!kept_inner[k])
				release[(*nrelease)++] = (SiftExtent) {old->inner[k].ref.blk, 1};
		for (uint32 k = 0; k < old->nleaves; k++)
			if (!kept[k] && (k == 0 || old->leaf[k].blk != old->leaf[k - 1].blk))
				release[(*nrelease)++] = (SiftExtent) {old->leaf[k].blk, 1};
	}
	pfree(by_gen);
	pfree(g);
	pfree(keep_ref);
	pfree(kept);
	pfree(kept_inner);
	pfree(o.ref);
	return rootblk;
}
