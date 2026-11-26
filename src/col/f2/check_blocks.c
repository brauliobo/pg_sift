/*
 * col/f2/check_blocks.c - the block ownership pass of sift.check() for an index or a sift_table of format major 2: every block below the high-water mark has one owner, a page of the
 * directory of the partitions, a list, a part (header, extents, deletion vector), a micro-slab or pending-delete chain, a column delta chain, a scope, a projection, the key journal, or the free
 * lists and the free blocks of the metadata slabs of the allocator. A block with two owners (a live part whose extent is also free) is damage that the next allocation turns into two writers of one
 * page; a block with none is a leak, what a failed write left allocated.
 *
 * The owners are one byte for each block of a window that the maintenance budget bounds, and the pass walks the relation once for each window. It needs the relation to itself against writers
 * (ShareLock, taken without waiting: a writer between the allocation of an extent and the commit that names it would be an orphan): busy, or on a standby, it says nothing. The leaks are
 * reported only when the pass named every extent the relation holds: parts in an object store, column delta chains of a part, shard directories and the wide values of a sift_table (named by
 * their granules only) are not named, so the blocks they hold would be taken for leaks.
 */
#include "core/host.h"

#include "access/xlog.h"
#include "storage/lmgr.h"

#include "col/f2/meta.h"
#include "col/f2/f2.h"
#include "col/f2/drop/parts.h"
#include "col/f2/journal.h"
#include "col/f2/l0/shard_dir.h"
#include "col/f2/l0/slab.h"
#include "col/f2/part/format.h"
#include "col/f2/del/pend.h"
#include "core/f2/fsm/fsm.h"
#include "core/f2/manifest/manifest.h"
#include "core/f2/proj.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "core/restrict.h"
#include "table/f2/col_delta/dir.h"
#include "table/f2/tbl.h"

#define OWNER_LIST(X) \
	X(OWN_NONE, "nothing") X(OWN_META, "metapage") X(OWN_DIR, "directory page") X(OWN_LIST, "list page") X(OWN_PART, "part") X(OWN_WIDE, "wide value") X(OWN_PACKED, "packed part page") \
	X(OWN_DV, "deletion vector") X(OWN_SLAB, "micro-slab page") X(OWN_PEND, "pending delete page") X(OWN_DELTA, "column delta page") X(OWN_SCOPE, "scope") \
	X(OWN_PROJ, "projection") X(OWN_JRNL, "key journal page") X(OWN_FREE, "free block") X(OWN_POOL, "metadata page of a lost pool")

#define OWNER_ENUM(id, label) id,
#define OWNER_LABEL(id, label) label,

typedef enum Owner
{
	OWNER_LIST(OWNER_ENUM)
	NOWNERS
} Owner;

static const char *const OWNER_LABELS[NOWNERS] = {OWNER_LIST(OWNER_LABEL)};

typedef struct BlockMap
{
	Relation	rel;
	SiftColMetaPageData meta;
	uint8	   *owner;			/* the owner of each block of the window */
	uint64		from;			/* the window [from, from + span) */
	uint64		span;
	BlockNumber pool_lo;		/* the window of the last batch of metadata pages of a pool (core/f2/fsm/fsm.h) */
	uint32		pool_mask;
	bool		complete;		/* every extent the relation holds has a name */
	MemoryContext open_cx;		/* the part being named: reset when its blocks are claimed */
} BlockMap;

/* How the pages of a chain link: where the next block sits in the payload, and the flags of a chain this pass cannot follow */
typedef struct ChainSpec
{
	SiftPageType type;
	uint32		magic;
	bool		sum;			/* the checksum is kept (a page that is open keeps none) */
	uint32		link_off;
	int			flags_off;		/* the offset of a uint16 of flags in the payload, -1: none */
	uint16		unsupported;
} ChainSpec;

static const ChainSpec SCOPE_DIR = {SIFT_PT_SCOPEDIR, SIFT_SCOPEDIR_MAGIC, true, offsetof(SiftScopeDirPageData, next_blk), -1, 0};
static const ChainSpec L0_SLAB = {SIFT_PT_L0_SLAB, SIFT_L0_SLAB_MAGIC, false, offsetof(SiftL0SlabPageData, next_blk), offsetof(SiftL0SlabPageData, flags), SIFT_L0_SLAB_SHARD_DIR};
static const ChainSpec PEND_DEL = {SIFT_PT_PEND_DEL, SIFT_PEND_DEL_MAGIC, false, offsetof(SiftPendDelPageData, next_blk), offsetof(SiftPendDelPageData, flags), SIFT_PEND_DEL_SHARD_DIR};

static void
claim(BlockMap *m, Owner kind, uint64 start, uint64 nblocks)
{
	uint64		lo = Max(start, m->from),
				hi = Min(start + nblocks, m->from + m->span);

	if (start + nblocks > m->meta.high_water)
		SIFT_CORRUPT(m->rel, "A %s at block %llu holds %llu blocks and the index was allocated only up to block %u.", OWNER_LABELS[kind], (unsigned long long) start, (unsigned long long) nblocks,
					 m->meta.high_water);
	for (uint64 b = lo; b < hi; b++)
	{
		uint8	   *o = &m->owner[b - m->from];

		if (*o != OWN_NONE && !(kind == OWN_PACKED && *o == OWN_PACKED))
			SIFT_CORRUPT(m->rel, "Block %llu is named twice: as a %s and as a %s.", (unsigned long long) b, OWNER_LABELS[*o], OWNER_LABELS[kind]);
		*o = kind;
	}
}

static void
claim_extents(BlockMap *m, Owner kind, const SiftExtent *ext, int n)
{
	for (int i = 0; i < n; i++)
		claim(m, kind, ext[i].start, ext[i].nblocks);
}

static void
claim_blocks(BlockMap *m, Owner kind, const BlockNumber *blk, int n)
{
	for (int i = 0; i < n; i++)
		claim(m, kind, blk[i], 1);
}

/* A copy of the page when it is a page of the type with the magic (and, for sum, a good checksum): a reference to any other is damage */
static Page
read_page(const BlockMap *m, BlockNumber blk, SiftPageType type, uint32 magic, bool sum, const char *what)
{
	Page		copy = palloc(BLCKSZ);
	Buffer		buf;

	if (blk >= RelationGetNumberOfBlocks(m->rel))
		SIFT_CORRUPT(m->rel, "The %s at block %u lies past the end of the file.", what, blk);
	buf = sift_blk_read(m->rel, blk, BUFFER_LOCK_SHARE);
	memcpy(copy, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	if (PageIsNew(copy) || !sift_page_layout_ok(copy) || SiftPageGetOpaque(copy)->type != type || *(uint32 *) SiftPageGetData(copy) != magic || (sum && !sift_page_sum_ok(copy)))
		SIFT_CORRUPT(m->rel, "The %s at block %u is not the page its reference says.", what, blk);
	return copy;
}

/* Every page of a chain, from head on; a chain that loops, or one this pass cannot follow, is not named and its blocks are no orphans' */
static void
claim_chain(BlockMap *m, Owner kind, BlockNumber head, const ChainSpec *c)
{
	uint32		limit = RelationGetNumberOfBlocks(m->rel);

	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Page		p = read_page(m, blk, c->type, c->magic, c->sum, OWNER_LABELS[kind]);

		CHECK_FOR_INTERRUPTS();
		if (limit-- == 0)
			SIFT_CORRUPT(m->rel, "The chain of %s pages from block %u loops.", OWNER_LABELS[kind], head);
		if (c->flags_off >= 0 && (*(uint16 *) (SiftPageGetData(p) + c->flags_off) & c->unsupported) != 0)
		{
			m->complete = false;
			pfree(p);
			return;
		}
		claim(m, kind, blk, 1);
		memcpy(&blk, SiftPageGetData(p) + c->link_off, sizeof(blk));
		pfree(p);
	}
}

static void
claim_tree(BlockMap *m, BlockNumber blk, uint32 level)
{
	Page		p;
	const SiftPartDirPageData *d;
	const SiftPartDirKey *k;

	if (level > SIFT_PARTDIR_MAX_LEVEL)
		SIFT_CORRUPT(m->rel, "The directory of the partitions has more levels than the format.");
	p = read_page(m, blk, SIFT_PT_PARTDIR, SIFT_PARTDIR_MAGIC, true, "directory page");
	d = (const SiftPartDirPageData *) SiftPageGetData(p);
	if (d->level != level || d->nentries == 0)
		SIFT_CORRUPT(m->rel, "The directory page at block %u is not the level it was named as.", blk);
	claim(m, OWN_DIR, blk, 1);
	k = (const SiftPartDirKey *) (SiftPageGetData(p) + sizeof(SiftPartDirPageData));
	for (uint32 i = 0; level > 0 && i < d->nentries; i++)
		claim_tree(m, k[i].child_blk, level - 1);
	pfree(p);
}

/* The part opened after MemoryContextSwitchTo(m->open_cx): its header, extent lists and stream, or the packed page that holds its header */
static void
claim_opened(BlockMap *m, Owner kind, const SiftPart *part)
{
	SiftExtent *ext;
	int			n;

	if (part->is_packed)
		claim(m, OWN_PACKED, part->header_blk, 1);
	else
	{
		ext = sift_part_blocks(part, &n);
		claim_extents(m, kind, ext, n);
	}
}

/* The wide values of a part of a sift_table: the extents its granules refer to */
static void
claim_wide(BlockMap *m, const SiftPartDesc *d, uint32 part_id)
{
	SiftF2Store *store = sift_f2_store_open(m->rel);
	SiftExtVec	wide = {0};
	SiftF2PartPin *pin;

	if (m->rel->rd_rel->relkind == RELKIND_INDEX || !sift_tbl_extra(store)->has_wide)
		return;
	pin = sift_f2_pin_open(m->rel, store, d, part_id);
	sift_part_wide_extents(m->rel, store, pin, &wide);
	sift_f2_pin_release(pin);
	claim_extents(m, OWN_WIDE, wide.ext, wide.n);
}

static void
claim_part(BlockMap *m, const SiftPartDesc *d, uint32 part_id)
{
	MemoryContext old;

	if (d->col_delta_blk != InvalidBlockNumber || (d->ext_flags & SIFT_PART_EXT_REMOTE))
		m->complete = false;
	if (d->ext_flags & SIFT_PART_EXT_REMOTE)
		return;
	if (d->dv_blk != InvalidBlockNumber)
		claim(m, OWN_DV, d->dv_blk, d->dv_nblocks);
	old = MemoryContextSwitchTo(m->open_cx);
	claim_opened(m, OWN_PART, sift_part_open_desc(m->rel, d, part_id));
	claim_wide(m, d, part_id);
	MemoryContextSwitchTo(old);
	MemoryContextReset(m->open_cx);
}

static void
claim_col_delta(BlockMap *m, const SiftPartitionEntry *e)
{
	SiftColDeltaAnchor a = sift_manifest_read_col_delta(m->rel, e);
	SiftColDeltaDiskReader disk = {.rel = m->rel,.part_id = (uint32) e->part_id};
	SiftColDeltaPageReader reader = sift_col_delta_disk_reader(&disk);
	SiftColDeltaChain chain;
	SiftExtent *ext;
	int			n;

	if (a.delta_head_blkno == InvalidBlockNumber)
		return;
	if (sift_col_delta_chain_read(&a, &reader, &chain) != NULL)
		SIFT_CORRUPT(m->rel, "The column delta chain of partition %llu is not what its anchor says.", (unsigned long long) e->part_id);
	ext = sift_col_delta_chain_extents(&chain, &n);
	claim_extents(m, OWN_DELTA, ext, n);
	sift_col_delta_chain_free(&chain);
}

static void
claim_dir_page(void *ctx, BlockNumber blk, const char *pg)
{
	claim(ctx, SiftPageGetOpaque((Page) pg)->type == SIFT_PT_PEND_DEL ? OWN_PEND : OWN_SLAB, blk, 1);
}

/* The micro-slab of a partition: a chain, or a shard directory with the chains of its shards */
static void
claim_slab(BlockMap *m, const SiftPartitionEntry *e)
{
	if (e->l0_slab_blk != InvalidBlockNumber && sift_l0_slab_is_dir(m->rel, e->part_id, e->l0_slab_blk))
		sift_l0_dir_pages_frozen(m->rel, e->part_id, e->l0_slab_blk, claim_dir_page, m);
	else
		claim_chain(m, OWN_SLAB, e->l0_slab_blk, &L0_SLAB);
}

static void
claim_partition(BlockMap *m, const SiftPartitionEntry *e)
{
	SiftPartDesc *parts;
	int			np;
	uint32		listed = 0;

	for (BlockNumber blk = e->manifest_blk; blk != InvalidBlockNumber;)
	{
		Page		p = read_page(m, blk, SIFT_PT_PARTLIST, SIFT_PARTLIST_MAGIC, true, "part list page");
		const SiftPartListPageData *l = (const SiftPartListPageData *) SiftPageGetData(p);

		if (l->part_id != e->part_id || l->nparts == 0 || (listed += l->nparts) > e->nparts)
			SIFT_CORRUPT(m->rel, "The part list page at block %u is not partition %llu's.", blk, (unsigned long long) e->part_id);
		claim(m, OWN_LIST, blk, 1);
		blk = l->next_blk;
		pfree(p);
	}
	if (listed != e->nparts)
		SIFT_CORRUPT(m->rel, "Partition %llu lists %u parts and its entry counts %u.", (unsigned long long) e->part_id, listed, (unsigned) e->nparts);
	parts = sift_manifest_read_parts(m->rel, e, &np);
	for (int i = 0; i < np; i++)
	{
		CHECK_FOR_INTERRUPTS();
		claim_part(m, &parts[i], (uint32) e->part_id);
	}
	pfree(parts);
	claim_col_delta(m, e);
	claim_slab(m, e);
	claim_chain(m, OWN_PEND, e->pend_del_blk, &PEND_DEL);
}

static void
claim_scopes(BlockMap *m)
{
	int			n;
	SiftManifestScopeRef *refs = sift_manifest_read_scopes(m->rel, &m->meta, &n);

	if (m->meta.scope_dir_blk != InvalidBlockNumber)
		claim_chain(m, OWN_SCOPE, m->meta.scope_dir_blk, &SCOPE_DIR);
	for (int i = 0; i < n; i++)
		if ((refs[i].flags & SIFT_SCOPEREF_ACTIVE) && refs[i].header_blk != InvalidBlockNumber)
		{
			MemoryContext old = MemoryContextSwitchTo(m->open_cx);

			claim_opened(m, OWN_SCOPE, sift_part_open(m->rel, refs[i].header_blk, refs[i].meta_gen, 0));
			MemoryContextSwitchTo(old);
			MemoryContextReset(m->open_cx);
		}
}

static void
claim_projections(BlockMap *m)
{
	int			nrefs,
				nb;
	SiftManifestProjRef *refs;
	BlockNumber *blk;

	if (m->meta.nprojs == 0)
		return;
	refs = sift_manifest_read_projs(m->rel, &m->meta, &nrefs);
	blk = sift_proj_ref_dir_blocks(m->rel, &m->meta, &nb);
	claim_blocks(m, OWN_PROJ, blk, nb);
	for (int i = 0; i < nrefs; i++)
	{
		SiftProjParts parts = sift_manifest_read_proj_parts(m->rel, &refs[i]);

		blk = sift_proj_one_dir_blocks(m->rel, &refs[i], &nb);
		claim_blocks(m, OWN_PROJ, blk, nb);
		for (int k = 0; k < parts.nparts; k++)
		{
			MemoryContext old = MemoryContextSwitchTo(m->open_cx);

			claim_opened(m, OWN_PROJ, sift_part_open(m->rel, parts.parts[k].header_blk, parts.parts[k].part_gen, SIFT_PROJ_PART_ID));
			MemoryContextSwitchTo(old);
			MemoryContextReset(m->open_cx);
		}
	}
}

static void
claim_journal(BlockMap *m)
{
	Page		p;
	const SiftKeyJrnlHeader *h;

	if (m->meta.jrnl_blk == InvalidBlockNumber)
		return;
	p = read_page(m, m->meta.jrnl_blk, SIFT_PT_KEYJRNL, SIFT_KJ_MAGIC, true, "key journal header");
	h = (const SiftKeyJrnlHeader *) SiftPageGetData(p);
	if (h->nslots != SIFT_KJ_SLOTS)
		SIFT_CORRUPT(m->rel, "The key journal header is not what the metapage says.");
	claim(m, OWN_JRNL, m->meta.jrnl_blk, 1);
	claim_blocks(m, OWN_JRNL, h->slot_blk, h->nslots);
	pfree(p);
}

static void
claim_partitions(BlockMap *m)
{
	SiftManifestRead mr = sift_manifest_read_range(m->rel, NULL, NULL);

	if (m->meta.tier_dir_blk != InvalidBlockNumber)
		m->complete = false;
	claim_tree(m, m->meta.partdir_root_blk, m->meta.partdir_level);
	for (int i = 0; i < mr.n; i++)
		claim_partition(m, &mr.entries[i]);
}

/* The extents of one list of the allocator with the pages of the list */
static void
claim_list(BlockMap *m, BlockNumber head)
{
	uint32		limit = RelationGetNumberOfBlocks(m->rel);

	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Page		p = read_page(m, blk, SIFT_PT_FSM_PAGE, SIFT_FSM_MAGIC, false, "free list page");
		const SiftFsmPageData *l = (const SiftFsmPageData *) SiftPageGetData(p);

		if (limit-- == 0 || l->nentries > SIFT_FSM_PAGE_CAP)
			SIFT_CORRUPT(m->rel, "The free list page at block %u loops or holds %u extents.", blk, (unsigned) l->nentries);
		claim(m, OWN_LIST, blk, 1);
		for (uint32 i = 0; i < l->nentries; i++)
			claim(m, OWN_FREE, l->ext[i].start, l->ext[i].nblocks);
		blk = l->next_blk;
		pfree(p);
	}
}

/* The free lists with their pages, the queue of released extents, and the free blocks of every slab of metadata pages */
static void
claim_allocator(BlockMap *m)
{
	Buffer		buf = sift_blk_read(m->rel, SIFT_FSM_BLKNO, BUFFER_LOCK_SHARE);
	PGAlignedBlock root;
	const SiftFsmMetaPageData *fm = (const SiftFsmMetaPageData *) SiftPageGetData((Page) root.data);
	const SiftFsmSlabTable *t = (const SiftFsmSlabTable *) (SiftPageGetData((Page) root.data) + SIFT_FSM_SLABS_OFF);

	memcpy(root.data, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	m->pool_lo = fm->pool_lo;
	m->pool_mask = fm->pool_mask;
	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		claim_list(m, fm->class_head[k]);
	claim_list(m, fm->quarantine_head);
	for (uint32 i = 0; i < t->nslabs; i++)
		for (uint32 b = 0; b < t->slab[i].nblocks; b++)
			if (t->slab[i].free[b / 64] & (UINT64CONST(1) << (b % 64)))
				claim(m, OWN_FREE, t->slab[i].start + b, 1);
}

/* The pages of the last window of a pool that nothing else names: what a restart or a crash took from the pool, which the orphan sweep frees */
static void
claim_lost_pool(BlockMap *m)
{
	for (int i = 0; i < SIFT_FSM_POOL_SPAN; i++)
	{
		uint64		b = (uint64) m->pool_lo + i;

		if ((m->pool_mask & (UINT32_C(1) << i)) && b >= m->from && b < m->from + m->span && b < m->meta.high_water && m->owner[b - m->from] == OWN_NONE)
			m->owner[b - m->from] = OWN_POOL;
	}
}

static void
report_leaks(const BlockMap *m)
{
	uint64		end = Min(m->from + m->span, m->meta.high_water),
				first = 0,
				n = 0;

	for (uint64 b = m->from; b < end; b++)
		if (m->owner[b - m->from] == OWN_NONE)
		{
			first = n == 0 ? b : first;
			n++;
		}
	if (n > 0)
		SIFT_CORRUPT(m->rel, "%llu blocks below the high-water mark are named by no part, list, chain or free list, the first is block %llu.", (unsigned long long) n, (unsigned long long) first);
}

static void
walk(BlockMap *m)
{
	memset(m->owner, OWN_NONE, m->span);
	claim(m, OWN_META, 0, SIFT_FSM_BLKNO + 1);
	claim_allocator(m);
	claim_partitions(m);
	claim_scopes(m);
	claim_projections(m);
	claim_journal(m);
	claim_lost_pool(m);
}

static void
check_windows(Relation rel)
{
	BlockMap	m = {.rel = rel,.meta = sift_col_meta_read(rel),.complete = sift_fsm_names_all_extents(rel) && !sift_fsm_sweep_due(rel)};
	Size		budget = Max(sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 2), 1024 * 1024);

	m.span = Min((uint64) m.meta.high_water, budget);
	m.open_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 check blocks part");
	/* check-memory: allow one window of the maintenance budget */
	m.owner = palloc(Max(m.span, 1));
	for (m.from = 0; m.from < m.meta.high_water; m.from += m.span)
	{
		walk(&m);
		if (m.complete)
			report_leaks(&m);
	}
	pfree(m.owner);
	MemoryContextDelete(m.open_cx);
}

void
sift_f2_check_blocks(Relation rel)
{
	bool		held = CheckRelationLockedByMe(rel, ShareLock, true);

	if (RecoveryInProgress() || (!held && !ConditionalLockRelation(rel, ShareLock)))
	{
		ereport(DEBUG1, (errmsg("sift index \"%s\": the block ownership is not checked, the relation is being written or is on a standby", RelationGetRelationName(rel))));
		return;
	}
	sift_fsm_meta_pool_drain(rel);
	if (!sift_xid_unassignable() && sift_fsm_sweep_due(rel))
		(void) sift_fsm_sweep_orphans(rel);
	check_windows(rel);
	if (!held)
		UnlockRelation(rel, ShareLock);
}
