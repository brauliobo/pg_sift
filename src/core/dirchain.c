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

/*
 * The entries of the chain whose root is root, written under version, into *segs (allocated for the entries and room more), and the chain itself.
 * The caller holds the metapage and the directory, so no update runs; the pages are read once each, under a share lock.
 */
int
sift_dirchain_read(Relation index, BlockNumber root, uint64 version, int room, SiftSegEntry **segs, SiftDirChain *chain)
{
	Buffer		buf = read_locked(index, root, BUFFER_LOCK_SHARE);
	SiftChainRootData *r = (SiftChainRootData *) SiftPageGetData(BufferGetPage(buf));
	SiftChainRef *top;
	uint32		ntop;
	uint32		nsegs;
	uint32		level;
	int			n = 0;
	bool		in_partitions = false;

	check_chain_page(index, buf, SIFT_PT_DIRROOT, version);
	level = r->level;
	if (level < 1 || level > SIFT_CHAIN_LEVELS_MAX)
		corrupt(index, "Directory root %u is of level %u, this build reads up to level %d.", root, level, SIFT_CHAIN_LEVELS_MAX);
	check_refs(index, r, buf, level, root, level == 1);
	ntop = r->nrefs;
	nsegs = r->nsegs;
	top = palloc(ntop * sizeof(SiftChainRef));
	memcpy(top, r->ref, ntop * sizeof(SiftChainRef));
	*chain = (SiftDirChain) {.root = root,.inner = palloc((level > 1 ? ntop : 1) * sizeof(BlockNumber)),.leaf = palloc(Max(level == 1 ? ntop : Min(nsegs, ntop * root_cap()), 1) * sizeof(SiftChainRef))};
	chain_add_block(chain, root);
	UnlockReleaseBuffer(buf);
	*segs = palloc(Max(nsegs + room, 1) * sizeof(SiftSegEntry));
	for (uint32 t = 0; t < ntop; t++)
	{
		const SiftChainRef *inner = &top[t];
		SiftChainRef *refs = top + t;
		uint32		nrefs = 1;
		uint32		below = 0;

		if (level > 1)
		{
			SiftChainRootData *ir;

			if (inner->lineage != 0)
				corrupt(index, "Directory interior reference %u of root %u has the lineage " UINT64_FORMAT ".", t, root, inner->lineage);
			buf = read_locked(index, inner->blk, BUFFER_LOCK_SHARE);
			check_chain_page(index, buf, SIFT_PT_DIRROOT, inner->gen);
			ir = (SiftChainRootData *) SiftPageGetData(BufferGetPage(buf));
			check_refs(index, ir, buf, 1, inner->blk, false);
			if (ir->nsegs != inner->n || memcmp(ir->ref[0].lo, inner->lo, SIFT_BOUND_BYTES) != 0)
				corrupt(index, "Directory interior page %u holds %u entries from another bound, its root says %u.", inner->blk, ir->nsegs, inner->n);
			nrefs = ir->nrefs;
			refs = palloc(nrefs * sizeof(SiftChainRef));
			memcpy(refs, ir->ref, nrefs * sizeof(SiftChainRef));
			UnlockReleaseBuffer(buf);
			chain->inner[chain->ninner++] = inner->blk;
			chain_add_block(chain, inner->blk);
		}
		for (uint32 i = 0; i < nrefs; i++)
		{
			const SiftChainRef *ref = &refs[i];
			SiftChainLeafData *leaf;

			if (ref->n == 0 || ref->n > SIFT_CHAIN_LEAF_CAP || (uint32) n + ref->n > nsegs)
				corrupt(index, "Directory leaf %u of root %u holds %u of the %u entries.", chain->nleaves, root, ref->n, nsegs);
			if (ref->lineage == 0 ? in_partitions || !bound_is_zero(ref->lo) :
				chain->nleaves > 0 && in_partitions && memcmp(chain->leaf[chain->nleaves - 1].lo, ref->lo, SIFT_BOUND_BYTES) > 0)
				corrupt(index, "Directory leaf %u of root %u breaks the order of its partitions (lineage " UINT64_FORMAT ").", chain->nleaves, root, ref->lineage);
			in_partitions |= ref->lineage != 0;
			buf = read_locked(index, ref->blk, BUFFER_LOCK_SHARE);
			check_chain_page(index, buf, SIFT_PT_DIRLEAF, ref->gen);
			leaf = (SiftChainLeafData *) SiftPageGetData(BufferGetPage(buf));
			if (leaf->n != ref->n || SiftPageGetUsed(BufferGetPage(buf)) != SIFT_CHAIN_LEAF_USED(leaf->n))
				corrupt(index, "Directory leaf %u holds %u entries in %u bytes, its root says %u.", ref->blk, leaf->n,
						SiftPageGetUsed(BufferGetPage(buf)), ref->n);
			memcpy(*segs + n, leaf->segs, ref->n * sizeof(SiftSegEntry));
			n += ref->n;
			below += ref->n;
			chain->leaf[chain->nleaves++] = *ref;
			UnlockReleaseBuffer(buf);
			chain_add_block(chain, ref->blk);
		}
		if (level > 1)
		{
			if (below != inner->n)
				corrupt(index, "Directory interior page %u says %u entries, its leaves hold %u.", inner->blk, inner->n, below);
			pfree(refs);
		}
	}
	pfree(top);
	if ((uint32) n != nsegs)
		corrupt(index, "Directory root %u says %u entries, its leaves hold %d.", root, nsegs, n);
	return n;
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

/* Entries [from, to) in leaves of as even a size as the leaf capacity allows */
static void
put_leaves(ChainOut *o, const SiftSegEntry *segs, int from, int to)
{
	int			n = to - from;
	int			leaves = (n + leaf_cap() - 1) / leaf_cap();

	for (int i = 0; i < leaves; i++)
	{
		int			lo = from + (int) ((int64) n * i / leaves);
		int			hi = from + (int) ((int64) n * (i + 1) / leaves);
		Page		page;
		SiftChainLeafData *leaf;
		BlockNumber blk = put_page(o, SIFT_PT_DIRLEAF, &page);

		leaf = (SiftChainLeafData *) SiftPageGetData(page);
		leaf->n = hi - lo;
		memcpy(leaf->segs, segs + lo, (hi - lo) * sizeof(SiftSegEntry));
		SiftPageSetUsed(page, SIFT_CHAIN_LEAF_USED(leaf->n));
		o->ref[o->nref++] = (SiftChainRef) {.blk = blk,.n = hi - lo,.gen = o->version};
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

/* The pages over the leaves o->ref lists: the root, or a root over interior pages of as even a number of references as a page holds when the leaves are more than a root holds */
static void
put_top(ChainOut *o, Relation index, int n, BlockNumber *rootblk)
{
	if ((uint32) o->nref <= root_cap())
	{
		*rootblk = put_refs(o, o->ref, 0, o->nref, 1);
		return;
	}
	{
		int			pages = (o->nref + root_cap() - 1) / root_cap();
		SiftChainRef *inner = palloc(pages * sizeof(SiftChainRef));

		if ((uint32) pages > root_cap())
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift index \"%s\" cannot hold %d segments", RelationGetRelationName(index), n),
					 errhint("Run sift.merge() on the index.")));
		for (int i = 0; i < pages; i++)
		{
			int			from = (int) ((int64) o->nref * i / pages),
						to = (int) ((int64) o->nref * (i + 1) / pages);

			inner[i] = (SiftChainRef) {.blk = put_refs(o, o->ref, from, to, 1),.gen = o->version};
			for (int k = from; k < to; k++)
				inner[i].n += o->ref[k].n;
			memcpy(inner[i].lo, o->ref[from].lo, SIFT_BOUND_BYTES);
		}
		*rootblk = put_refs(o, inner, 0, pages, 2);
		pfree(inner);
	}
}

/*
 * Writes the chain of segs (n entries) that follows old (the chain being replaced, NULL when the directory was block 1 alone), whose entries were
 * oldsegs: a leaf of old that is at least half full and whose entries are found in segs as they were, together and in order, stays (a smaller one is
 * written again with the entries beside it, so that the leaves of a directory that grows by single entries stay half full and the root does not fill with small ones); the entries between two such leaves go to
 * new leaves; with below, a leaf at or above it is written again, and so is every page the update writes, below it. The metapage is locked exclusively (the blocks come from the free list under it). The blocks of old that the new chain does not
 * keep go to release (room for old->nleaves + 1); returns the new root, which nothing refers to yet.
 */
BlockNumber
sift_dirchain_write(Relation index, Buffer metabuf, uint64 version, const SiftDirChain *old, const SiftSegEntry *oldsegs,
					const SiftSegEntry *segs, int n, BlockNumber below, SiftExtent *release, int *nrelease)
{
	ChainOut	o = {.index = index,.metabuf = metabuf,.version = version,.below = below};
	GenPos	   *by_gen = palloc(Max(n, 1) * sizeof(GenPos));
	bool	   *kept = palloc0(((old ? old->nleaves : 0) + 1) * sizeof(bool));
	int			pos = 0;
	int			off = 0;
	BlockNumber rootblk;

	o.ref = palloc((n / leaf_cap() + 2 * (old ? old->nleaves : 0) + 2) * sizeof(SiftChainRef));
	for (int i = 0; i < n; i++)
		by_gen[i] = (GenPos) {segs[i].gen, i};
	qsort(by_gen, n, sizeof(GenPos), cmp_genpos);
	for (uint32 k = 0; old && k < old->nleaves; off += old->leaf[k].n, k++)
	{
		const SiftChainRef *ref = &old->leaf[k];
		GenPos		key = {oldsegs[off].gen, 0};
		const GenPos *hit = bsearch(&key, by_gen, n, sizeof(GenPos), cmp_genpos);

		if (ref->n * 2 < leaf_cap() || hit == NULL || hit->pos < pos || hit->pos + ref->n > n || (BlockNumberIsValid(below) && ref->blk >= below) ||
			memcmp(segs + hit->pos, oldsegs + off, ref->n * sizeof(SiftSegEntry)) != 0)
			continue;
		put_leaves(&o, segs, pos, hit->pos);
		o.ref[o.nref++] = *ref;
		kept[k] = true;
		pos = hit->pos + ref->n;
	}
	put_leaves(&o, segs, pos, n);
	put_top(&o, index, n, &rootblk);
	flush_pending(&o);
	if (old)
	{
		release[(*nrelease)++] = (SiftExtent) {old->root, 1};
		for (uint32 k = 0; k < old->ninner; k++)
			release[(*nrelease)++] = (SiftExtent) {old->inner[k], 1};
		for (uint32 k = 0; k < old->nleaves; k++)
			if (!kept[k])
				release[(*nrelease)++] = (SiftExtent) {old->leaf[k].blk, 1};
	}
	pfree(by_gen);
	pfree(kept);
	pfree(o.ref);
	return rootblk;
}
