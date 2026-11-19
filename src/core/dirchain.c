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
#include "utils/hsearch.h"
#include "utils/memutils.h"

#include "core/cachebudget.h"
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
	for (uint32 i = 0; i < nrefs; i++)
	{
		const SiftChainRef *ref = &refs[i];
		SiftChainLeafData *leaf;

		if (i >= k0 && (i - k0 < from || i - k0 >= to))
			continue;
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
		chain->leaf[chain->nleaves++] = *ref;
		UnlockReleaseBuffer(buf);
		chain_add_block(chain, ref->blk);
	}
	pfree(top);
	if (level > 1)
		pfree(refs);
	check_lineages(index, chain, root);
	if (keys == NULL && (uint32) n != nsegs)
		corrupt(index, "Directory root %u says %u entries, its leaves hold %d.", root, nsegs, n);
	return n;
}

/*
 * The whole chain at a version, memoized per backend. A directory update stores a new version and a chain is immutable under one, so the entries of an index at a version are the
 * same wherever they were read: a statement that asks for the directory again (each of its writers does, to find a segment, to open the tombstone log, to add a segment) copies the memo
 * instead of reading and checking every page of the chain. One entry per index, dropped by the budget the backend's caches share (core/cachebudget.h) and by a newer version. The
 * checks and the reports of the directory read the pages (sift_dirchain_read).
 */
typedef struct DirMemo
{
	RelFileLocator rel;			/* hash key */
	BlockNumber root;
	uint64		version;
	uint64		used;
	Size		bytes;
	SiftDirChain chain;			/* leaf, inner and blocks point into data */
	int			nsegs;
	SiftSegEntry *segs;
	char	   *data;
} DirMemo;

static HTAB *memos;
static MemoryContext memo_cx;
static Size memo_held;

static Size
memo_bytes(void)
{
	return memo_held;
}

static DirMemo *
memo_oldest_entry(void)
{
	HASH_SEQ_STATUS seq;
	DirMemo    *e,
			   *oldest = NULL;

	if (memos == NULL)
		return NULL;
	hash_seq_init(&seq, memos);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (oldest == NULL || e->used < oldest->used)
			oldest = e;
	return oldest;
}

static uint64
memo_oldest(void)
{
	DirMemo    *e = memo_oldest_entry();

	return e ? e->used : UINT64_MAX;
}

static void
memo_drop(DirMemo *e)
{
	RelFileLocator rel = e->rel;

	memo_held -= e->bytes;
	pfree(e->data);
	hash_search(memos, &rel, HASH_REMOVE, NULL);
}

static void
memo_evict(void)
{
	memo_drop(memo_oldest_entry());
}

static const SiftCacheKind memo_kind = {memo_bytes, memo_oldest, memo_evict};

static void
memo_init(void)
{
	HASHCTL		ctl = {.keysize = sizeof(RelFileLocator),.entrysize = sizeof(DirMemo)};

	memo_cx = AllocSetContextCreate(TopMemoryContext, "sift cache directories", ALLOCSET_DEFAULT_SIZES);
	ctl.hcxt = memo_cx;
	memos = hash_create("sift chained directories", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	sift_cache_register(&memo_kind);
}

/* Where the arrays of a chain of nsegs entries lie in one allocation: entries, leaves, interior pages, blocks */
static Size
memo_bytes_of(int nsegs, const SiftDirChain *c)
{
	return nsegs * sizeof(SiftSegEntry) + c->nleaves * sizeof(SiftChainRef) + c->ninner * sizeof(SiftDirInner) + c->nblocks * sizeof(BlockNumber);
}

static DirMemo *
memo_keep(Relation index, uint64 version, const SiftSegEntry *segs, int nsegs, const SiftDirChain *chain)
{
	Size		bytes = memo_bytes_of(nsegs, chain);
	DirMemo    *e;
	bool		found;
	char	   *at;

	if (memos == NULL)
		memo_init();
	if ((e = hash_search(memos, &index->rd_locator, HASH_FIND, NULL)) != NULL)
		memo_drop(e);
	if (!sift_cache_room(bytes))
		return NULL;
	e = hash_search(memos, &index->rd_locator, HASH_ENTER, &found);
	e->root = chain->root;
	e->version = version;
	e->used = sift_cache_tick();
	e->bytes = bytes;
	e->nsegs = nsegs;
	e->data = at = MemoryContextAlloc(memo_cx, Max(bytes, 1));
	e->segs = (SiftSegEntry *) at;
	memcpy(at, segs, nsegs * sizeof(SiftSegEntry));
	at += nsegs * sizeof(SiftSegEntry);
	e->chain = *chain;
	e->chain.leaf = (SiftChainRef *) at;
	memcpy(at, chain->leaf, chain->nleaves * sizeof(SiftChainRef));
	at += chain->nleaves * sizeof(SiftChainRef);
	e->chain.inner = (SiftDirInner *) at;
	memcpy(at, chain->inner, chain->ninner * sizeof(SiftDirInner));
	at += chain->ninner * sizeof(SiftDirInner);
	e->chain.blocks = (BlockNumber *) at;
	memcpy(at, chain->blocks, chain->nblocks * sizeof(BlockNumber));
	e->chain.maxblocks = chain->nblocks;
	memo_held += bytes;
	return e;
}

static DirMemo *
memo_find(Relation index, BlockNumber root, uint64 version)
{
	DirMemo    *e = memos == NULL ? NULL : hash_search(memos, &index->rd_locator, HASH_FIND, NULL);

	if (e == NULL || e->root != root || e->version != version)
		return NULL;
	e->used = sift_cache_tick();
	return e;
}

SiftDirEntries
sift_dirchain_entries(Relation index, BlockNumber root, uint64 version)
{
	DirMemo    *e = memo_find(index, root, version);
	SiftSegEntry *segs;
	SiftDirChain chain = {0};
	int			n;

	if (e != NULL)
		return (SiftDirEntries) {.segs = e->segs,.nsegs = e->nsegs,.blocks = e->chain.blocks,.nblocks = e->chain.nblocks,.kept = true};
	n = chain_read(index, root, version, 0, &segs, &chain, NULL);
	if ((e = memo_keep(index, version, segs, n, &chain)) == NULL)
	{
		pfree(chain.leaf);
		pfree(chain.inner);
		return (SiftDirEntries) {.segs = segs,.nsegs = n,.blocks = chain.blocks,.nblocks = chain.nblocks};
	}
	pfree(segs);
	pfree(chain.leaf);
	pfree(chain.inner);
	pfree(chain.blocks);
	return (SiftDirEntries) {.segs = e->segs,.nsegs = n,.blocks = e->chain.blocks,.nblocks = e->chain.nblocks,.kept = true};
}

void
sift_dirchain_entries_release(SiftDirEntries *e)
{
	if (!e->kept)
	{
		pfree((void *) e->segs);
		pfree((void *) e->blocks);
	}
}

int
sift_dirchain_load(Relation index, BlockNumber root, uint64 version, SiftSegEntry **segs, SiftDirChain *chain)
{
	DirMemo    *e = memo_find(index, root, version);
	int			n;

	if (e == NULL)
	{
		n = chain_read(index, root, version, 0, segs, chain, NULL);
		memo_keep(index, version, *segs, n, chain);
		return n;
	}
	*segs = memcpy(palloc(Max(e->nsegs, 1) * sizeof(SiftSegEntry)), e->segs, e->nsegs * sizeof(SiftSegEntry));
	*chain = e->chain;
	chain->leaf = memcpy(palloc(Max(e->chain.nleaves, 1) * sizeof(SiftChainRef)), e->chain.leaf, e->chain.nleaves * sizeof(SiftChainRef));
	chain->inner = memcpy(palloc(Max(e->chain.ninner, 1) * sizeof(SiftDirInner)), e->chain.inner, e->chain.ninner * sizeof(SiftDirInner));
	/* check-memory: allow the pages of one directory chain, 4 bytes each, which the entries it lists bound */
	chain->blocks = memcpy(palloc(Max(e->chain.nblocks, 1) * sizeof(BlockNumber)), e->chain.blocks, e->chain.nblocks * sizeof(BlockNumber));
	chain->maxblocks = Max(e->chain.nblocks, 1);
	return e->nsegs;
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

/* A partition: its entries [from, to) in a leaf of their own */
static void
put_part(ChainOut *o, const SiftSegEntry *segs, const SiftDirPart *part)
{
	Page		page;
	SiftChainLeafData *leaf;
	BlockNumber blk = put_page(o, SIFT_PT_DIRLEAF, &page);

	leaf = (SiftChainLeafData *) SiftPageGetData(page);
	leaf->n = part->n;
	memcpy(leaf->segs, segs + part->first, part->n * sizeof(SiftSegEntry));
	SiftPageSetUsed(page, SIFT_CHAIN_LEAF_USED(leaf->n));
	o->ref[o->nref] = (SiftChainRef) {.blk = blk,.n = part->n,.gen = o->version,.lineage = part->lineage};
	memcpy(o->ref[o->nref++].lo, part->lo, SIFT_BOUND_BYTES);
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
		if (o->ref[i].blk == ref->blk)
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

/*
 * Writes the chain of segs (n entries) that follows old (the chain being replaced, NULL when the directory was block 1 alone), whose entries were
 * oldsegs: interior pages stay as leaves do (put_top). A leaf of old that is at least half full and whose entries are found in segs as they were, together and in order, stays (a smaller one is
 * written again with the entries beside it, so that the leaves of a directory that grows by single entries stay half full and the root does not fill with small ones); the entries between two such leaves go to
 * new leaves; with below, a leaf at or above it is written again, and so is every page the update writes, below it. The metapage is locked exclusively (the blocks come from the free list under it). The blocks of old that the new chain does not
 * keep go to release (room for old->nleaves + 1); returns the new root, which nothing refers to yet.
 */
BlockNumber
sift_dirchain_write(Relation index, Buffer metabuf, uint64 version, const SiftDirChain *old, const SiftSegEntry *oldsegs,
					const SiftSegEntry *segs, int n, const SiftDirPart *parts, int nparts, BlockNumber below, SiftExtent *release, int *nrelease)
{
	ChainOut	o = {.index = index,.metabuf = metabuf,.version = version,.below = below};
	GenPos	   *by_gen = palloc(Max(n, 1) * sizeof(GenPos));
	bool	   *kept = palloc0(((old ? old->nleaves : 0) + 1) * sizeof(bool));
	bool	   *kept_inner = palloc0(((old ? old->ninner : 0) + 1) * sizeof(bool));
	int			pos = 0;
	int			off = 0;
	int			head = nparts > 0 ? (int) parts[0].first : n;
	BlockNumber rootblk;

	o.ref = palloc((n / leaf_cap() + 2 * (old ? old->nleaves : 0) + nparts + 2) * sizeof(SiftChainRef));
	for (int i = 0; i < n; i++)
		by_gen[i] = (GenPos) {segs[i].gen, i};
	qsort(by_gen, n, sizeof(GenPos), cmp_genpos);
	for (uint32 k = 0; old && k < old->nleaves; off += old->leaf[k].n, k++)
	{
		const SiftChainRef *ref = &old->leaf[k];
		GenPos		key = {oldsegs[off].gen, 0};
		const GenPos *hit = bsearch(&key, by_gen, n, sizeof(GenPos), cmp_genpos);

		if (ref->lineage != 0 || ref->n * 2 < leaf_cap() || hit == NULL || hit->pos < pos || hit->pos + ref->n > head || (BlockNumberIsValid(below) && ref->blk >= below) ||
			memcmp(segs + hit->pos, oldsegs + off, ref->n * sizeof(SiftSegEntry)) != 0)
			continue;
		put_leaves(&o, segs, pos, hit->pos);
		o.ref[o.nref++] = *ref;
		kept[k] = true;
		pos = hit->pos + ref->n;
	}
	put_leaves(&o, segs, pos, head);
	for (int p = 0; p < nparts; p++)
	{
		uint32		k = 0;
		int			at = 0;

		for (; old && k < old->nleaves; at += old->leaf[k].n, k++)
			if (old->leaf[k].lineage == parts[p].lineage)
				break;
		if (old && k < old->nleaves && old->leaf[k].n == parts[p].n && memcmp(old->leaf[k].lo, parts[p].lo, SIFT_BOUND_BYTES) == 0 &&
			!(BlockNumberIsValid(below) && old->leaf[k].blk >= below) &&
			memcmp(segs + parts[p].first, oldsegs + at, parts[p].n * sizeof(SiftSegEntry)) == 0)
		{
			o.ref[o.nref++] = old->leaf[k];
			kept[k] = true;
		}
		else
			put_part(&o, segs, &parts[p]);
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
			if (!kept[k])
				release[(*nrelease)++] = (SiftExtent) {old->leaf[k].blk, 1};
	}
	pfree(by_gen);
	pfree(kept);
	pfree(kept_inner);
	pfree(o.ref);
	return rootblk;
}
