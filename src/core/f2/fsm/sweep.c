/*
 * core/f2/fsm/sweep.c - the orphan sweep of format major 2 (core/f2/fsm/fsm.h): the extents that a crash, or a failure after the write of a part and before the commit that names it, left allocated
 * and named by nothing go back to the allocator.
 *
 * The sweep names everything the relation holds from the metapage down (the directory of the partitions with its lists, the parts with their extent lists and deletion vectors, the micro-slab and
 * the pending deletes of each partition, the wide values of a sift_table that its granules and its micro-slabs refer to, the column delta chains, the scope directory and its summaries, the projections, the key journal, the lists and the queue of the allocator), takes what is
 * free or queued and the free blocks of the slabs, and what is left below the high-water mark is an orphan. It runs only when no writer has the relation (ExclusiveLock without waiting, the load
 * lock, no VACUUM reading the manifest): no writer is between the allocation of an extent and the commit that names it. Readers allocate nothing and are not excluded, and the lock, unlike
 * AccessExclusiveLock, is not replayed by a standby, so the sweep cancels no query there. It frees nothing, and says why at DEBUG1, when the relation
 * holds something it does not name (a part in an object store, a shard directory), when a page is not what its reference says, when
 * two names or a name and a free list overlap, or when the orphans are over half of the index. A page of the extents a part keeps under its generation when its pieces are moved below a cut (col/f2/tail.c) is an orphan like any other when the crash came before the commit that names it: the generation of a page says nothing of whether the live part names it. While a physical standby streams, the
 * orphans enter the quarantine of the allocator as the extents of a compaction do, and the horizon of the standby holds them back.
 */
#include "core/host.h"

#include "access/xlog.h"
#include "storage/lmgr.h"
#include "utils/timestamp.h"

#include "col/f2/meta.h"
#include "col/f2/f2.h"
#include "col/f2/drop/parts.h"
#include "col/f2/journal.h"
#include "col/f2/l0/shard_dir.h"
#include "col/f2/l0/slab.h"
#include "col/f2/part/format.h"
#include "col/f2/del/pend.h"
#include "core/f2/fsm/fsm.h"
#include "core/f2/fsm/int.h"
#include "core/f2/manifest/manifest.h"
#include "core/f2/proj.h"
#include "core/ring.h"
#include "core/maint/maint.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "core/part_lock.h"
#include "table/f2/blob.h"
#include "table/f2/col_delta/dir.h"
#include "table/f2/tbl.h"

#define SIFT_FSM_SWEEP_MAX_PCT	50		/* the share of the index the orphans may be: more is a reference the sweep missed, not a leak */

enum
{
	NAMED_DIR,
	NAMED_LIST,
	NAMED_PART,
	NAMED_DV,
	NAMED_SLAB,
	NAMED_PEND,
	NAMED_DELTA,
	NAMED_SCOPE,
	NAMED_PROJ,
	NAMED_JRNL,
	NAMED_WIDE,
	NAMED_FREE,
	NNAMED
};

static const char *const NAMED_LABEL[NNAMED] = {"directory", "list", "part", "delvec", "slab", "pend_del", "delta", "scope", "projection", "journal", "wide value", "free slab"};

typedef struct Sweep
{
	Relation	rel;
	SiftColMetaPageData meta;
	PGAlignedBlock root;		/* the allocator root: the slabs */
	ExtVec		held;			/* the extents something names, the free lists and the queue included */
	ExtVec		packed;			/* the packed pages the parts name: one page holds the header of up to 16 of them */
	uint32		named[NNAMED];	/* how many of each kind of block the sweep named, for DEBUG1 */
	const char *why;			/* why the sweep gave up */
	MemoryContext cx;			/* what the sweep keeps */
	MemoryContext open_cx;		/* the part being named: reset when its blocks are held */
} Sweep;

/* How the pages of a chain link: where the next block sits in the payload, and the flags that make a chain one the sweep cannot follow */
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

static bool
give_up(Sweep *s, const char *why)
{
	s->why = why;
	return false;
}

static void
hold(Sweep *s, int kind, BlockNumber start, uint32 nblocks)
{
	sift_fsm_extvec_add(&s->held, start, nblocks, InvalidFullTransactionId);
	s->named[kind] += nblocks;
}

static void
hold_extents(Sweep *s, int kind, const SiftExtent *ext, int n)
{
	for (int i = 0; i < n; i++)
		hold(s, kind, ext[i].start, ext[i].nblocks);
}

static void
hold_blocks(Sweep *s, int kind, const BlockNumber *blk, int n)
{
	for (int i = 0; i < n; i++)
		hold(s, kind, blk[i], 1);
}

/* A copy of the page when it is a page of the type with the magic (and, for sum, a good checksum), else NULL: a reference to a page that is not what it says ends the sweep */
static Page
read_page(Relation rel, BlockNumber blk, SiftPageType type, uint32 magic, bool sum)
{
	Page		copy;
	Buffer		buf;

	if (blk >= RelationGetNumberOfBlocks(rel))
		return NULL;
	copy = palloc(BLCKSZ);
	buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
	memcpy(copy, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	if (PageIsNew(copy) || !sift_page_layout_ok(copy) || SiftPageGetOpaque(copy)->type != type || *(uint32 *) SiftPageGetData(copy) != magic || (sum && !sift_page_sum_ok(copy)))
	{
		pfree(copy);
		return NULL;
	}
	return copy;
}

/* Every page of a chain, from head on, held; false when a page is not of the chain, the chain loops or a page says it is one the sweep cannot follow */
static bool
hold_chain(Sweep *s, int kind, BlockNumber head, const ChainSpec *c)
{
	uint32		limit = RelationGetNumberOfBlocks(s->rel);

	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Page		p = read_page(s->rel, blk, c->type, c->magic, c->sum);

		if (p == NULL || limit-- == 0)
			return give_up(s, "a chain page is not what its link says, or the chain loops");
		if (c->flags_off >= 0 && (*(uint16 *) (SiftPageGetData(p) + c->flags_off) & c->unsupported) != 0)
			return give_up(s, "a chain is a shard directory");
		hold(s, kind, blk, 1);
		memcpy(&blk, SiftPageGetData(p) + c->link_off, sizeof(blk));
		CHECK_FOR_INTERRUPTS();
		pfree(p);
	}
	return true;
}

static void
hold_slab_page(void *ctx, BlockNumber blk, const char *pg)
{
	hold(ctx, SiftPageGetOpaque((Page) pg)->type == SIFT_PT_PEND_DEL ? NAMED_PEND : NAMED_SLAB, blk, 1);
}

/* The micro-slab of a partition: a chain, or a shard directory with the chains of its shards */
static bool
hold_slab(Sweep *s, const SiftPartitionEntry *e)
{
	if (e->l0_slab_blk == InvalidBlockNumber || !sift_l0_slab_is_dir(s->rel, e->part_id, e->l0_slab_blk))
		return hold_chain(s, NAMED_SLAB, e->l0_slab_blk, &L0_SLAB);
	sift_l0_dir_pages_frozen(s->rel, e->part_id, e->l0_slab_blk, hold_slab_page, s);
	return true;
}

/* A page of the directory of the partitions with its level, held */
static bool
hold_dir_page(Sweep *s, BlockNumber blk, uint32 level, Page *out)
{
	Page		p = read_page(s->rel, blk, SIFT_PT_PARTDIR, SIFT_PARTDIR_MAGIC, true);
	const SiftPartDirPageData *d = p != NULL ? (const SiftPartDirPageData *) SiftPageGetData(p) : NULL;

	if (d == NULL || d->level != level || d->nentries == 0)
		return give_up(s, "a page of the directory of the partitions is not the level it was named as");
	hold(s, NAMED_DIR, blk, 1);
	*out = p;
	return true;
}

/* The pages of the directory of the partitions: the root, the interior pages under it and the leaves */
static bool
hold_tree(Sweep *s, BlockNumber blk, uint32 level)
{
	Page		p;
	const SiftPartDirKey *k;
	int			n;

	if (level > SIFT_PARTDIR_MAX_LEVEL)
		return give_up(s, "the directory of the partitions has more levels than the format");
	if (!hold_dir_page(s, blk, level, &p))
		return false;
	if (level == 0)
		return true;
	k = (const SiftPartDirKey *) (SiftPageGetData(p) + sizeof(SiftPartDirPageData));
	n = ((const SiftPartDirPageData *) SiftPageGetData(p))->nentries;
	for (int i = 0; i < n; i++)
		if (!hold_tree(s, k[i].child_blk, level - 1))
			return false;
	return true;
}

/* A part is opened in a context of its own, which hold_opened resets: the sweep keeps the blocks of every part, not the header page and the lists of each */
static void
open_part_begin(Sweep *s)
{
	MemoryContextSwitchTo(s->open_cx);
}

/* The blocks of the part opened after open_part_begin: its header, extent lists and stream, or the packed page that holds its header */
static void
hold_opened(Sweep *s, int kind, const SiftPart *part)
{
	SiftExtent *ext;
	int			n;

	MemoryContextSwitchTo(s->cx);
	if (part->is_packed)
		sift_fsm_extvec_add(&s->packed, part->header_blk, 1, InvalidFullTransactionId);
	else
	{
		ext = sift_part_blocks(part, &n);
		hold_extents(s, kind, ext, n);
		pfree(ext);
	}
	MemoryContextReset(s->open_cx);
}

/* The extents of the wide values a sift_table keeps beside its parts (a text, jsonb or array value over the wide threshold): named by the granules that refer to them */
static SiftF2Store *
wide_store(const Sweep *s)
{
	SiftF2Store *store = sift_f2_store_open(s->rel);

	return store->host->reclaim_dead_blobs != NULL ? store : NULL;
}

/* The extents collected in the part's context, held in the sweep's */
static void
hold_wide(Sweep *s, const SiftExtVec *wide)
{
	MemoryContextSwitchTo(s->cx);
	hold_extents(s, NAMED_WIDE, wide->ext, wide->n);
	MemoryContextReset(s->open_cx);
}

static void
hold_part_wide(Sweep *s, SiftF2Store *store, const SiftPartDesc *d, uint32 part_id)
{
	SiftExtVec	wide = {0};
	SiftF2PartPin *pin;

	open_part_begin(s);
	pin = sift_f2_pin_open(s->rel, store, d, part_id);
	sift_part_wide_extents(s->rel, store, pin, &wide);
	sift_f2_pin_release(pin);
	hold_wide(s, &wide);
}

static bool
hold_part(Sweep *s, const SiftPartDesc *d, uint32 part_id)
{
	SiftF2Store *wide = wide_store(s);

	if (d->col_delta_blk != InvalidBlockNumber || (d->ext_flags & SIFT_PART_EXT_REMOTE))
		return give_up(s, "a part names a column delta chain of its own or lives in an object store");
	if (d->dv_blk != InvalidBlockNumber)
	{
		hold(s, NAMED_DV, d->dv_blk, d->dv_nblocks);
	}
	if (wide != NULL)
		hold_part_wide(s, wide, d, part_id);
	open_part_begin(s);
	hold_opened(s, NAMED_PART, sift_part_open_desc(s->rel, d, part_id));
	return true;
}

/* The column delta chain of the partition: its data pages and the pages of its directory */
static bool
hold_col_delta(Sweep *s, const SiftPartitionEntry *e)
{
	SiftColDeltaAnchor a = sift_manifest_read_col_delta(s->rel, e);
	SiftColDeltaDiskReader disk = {.rel = s->rel,.part_id = (uint32) e->part_id};
	SiftColDeltaPageReader reader = sift_col_delta_disk_reader(&disk);
	SiftColDeltaChain chain;
	SiftExtent *ext;
	int			n;

	if (a.delta_head_blkno == InvalidBlockNumber)
		return true;
	if (sift_col_delta_chain_read(&a, &reader, &chain) != NULL)
		return give_up(s, "a column delta chain is not what its anchor says");
	ext = sift_col_delta_chain_extents(&chain, &n);
	hold_extents(s, NAMED_DELTA, ext, n);
	sift_col_delta_chain_free(&chain);
	return true;
}

/* The wide values the rows of the micro-slab refer to: they are written before the rows are, and a part takes the references over when the slab is flushed */
static bool
hold_slab_wide(Sweep *s, const SiftPartitionEntry *e)
{
	SiftF2Store *store = wide_store(s);
	SiftExtVec	wide = {0};
	SiftRowBuf *rb;
	BlockNumber *pages;
	int			npages;

	if (store == NULL || e->l0_slab_blk == InvalidBlockNumber)
		return true;
	open_part_begin(s);
	/* check-memory: allow bounded to one micro-slab: SIFT_L0_SLAB_MAX_PAGES pages */
	rb = sift_rowbuf_new(&store->spec);
	sift_l0_slab_read(s->rel, e->part_id, e->l0_slab_blk, rb, &store->spec, InvalidSnapshot, &pages, &npages);
	for (int c = 0; c < rb->ncols; c++)
		for (int r = 0; rb->tag[c] != NULL && r < rb->n; r++)
			if (rb->tag[c][r] == SIFT_COLTAG_WIDE_BLOB)
				sift_tbl_blob_release_ref(s->rel, rb->xpay[c][r], rb->xplen[c][r], &wide);
	sift_rowbuf_free(rb);
	hold_wide(s, &wide);
	return true;
}

/* The part list pages of the partition, then its parts, micro-slab, pending deletes and column delta chain */
static bool
hold_partition(Sweep *s, const SiftPartitionEntry *e)
{
	SiftPartDesc *parts;
	int			np;
	uint32		listed = 0;

	for (BlockNumber blk = e->manifest_blk; blk != InvalidBlockNumber;)
	{
		Page		p = read_page(s->rel, blk, SIFT_PT_PARTLIST, SIFT_PARTLIST_MAGIC, true);
		const SiftPartListPageData *l = p != NULL ? (const SiftPartListPageData *) SiftPageGetData(p) : NULL;

		if (l == NULL || l->part_id != e->part_id || l->nparts == 0 || (listed += l->nparts) > e->nparts)
			return give_up(s, "a part list page is not the partition's");
		hold(s, NAMED_LIST, blk, 1);
		blk = l->next_blk;
		pfree(p);
	}
	if (listed != e->nparts)
		return give_up(s, "a partition lists other parts than its entry counts");
	parts = sift_manifest_read_parts(s->rel, e, &np);
	for (int i = 0; i < np; i++)
		if (!hold_part(s, &parts[i], (uint32) e->part_id))
			return false;
	pfree(parts);
	return hold_col_delta(s, e) && hold_slab(s, e) && hold_slab_wide(s, e) &&
		hold_chain(s, NAMED_PEND, e->pend_del_blk, &PEND_DEL);
}

static bool
hold_scopes(Sweep *s)
{
	int			n;
	SiftManifestScopeRef *refs = sift_manifest_read_scopes(s->rel, &s->meta, &n);

	if (s->meta.scope_dir_blk != InvalidBlockNumber)
	{
		if (!hold_chain(s, NAMED_SCOPE, s->meta.scope_dir_blk, &SCOPE_DIR))
			return false;
	}
	for (int i = 0; i < n; i++)
		if ((refs[i].flags & SIFT_SCOPEREF_ACTIVE) && refs[i].header_blk != InvalidBlockNumber)
		{
			open_part_begin(s);
			hold_opened(s, NAMED_SCOPE, sift_part_open(s->rel, refs[i].header_blk, refs[i].meta_gen, 0));
		}
	return true;
}

static bool
hold_projections(Sweep *s)
{
	int			nrefs,
				nb;
	SiftManifestProjRef *refs;
	BlockNumber *blk;

	if (s->meta.nprojs == 0)
		return true;
	refs = sift_manifest_read_projs(s->rel, &s->meta, &nrefs);
	blk = sift_proj_ref_dir_blocks(s->rel, &s->meta, &nb);
	hold_blocks(s, NAMED_PROJ, blk, nb);
	for (int i = 0; i < nrefs; i++)
	{
		SiftProjParts parts = sift_manifest_read_proj_parts(s->rel, &refs[i]);

		blk = sift_proj_one_dir_blocks(s->rel, &refs[i], &nb);
		hold_blocks(s, NAMED_PROJ, blk, nb);
		for (int k = 0; k < parts.nparts; k++)
		{
			open_part_begin(s);
			hold_opened(s, NAMED_PROJ, sift_part_open(s->rel, parts.parts[k].header_blk, parts.parts[k].part_gen, SIFT_PROJ_PART_ID));
		}
	}
	return true;
}

/* The header of the key journal and the pages of its records */
static bool
hold_journal(Sweep *s)
{
	Page		p;
	const SiftKeyJrnlHeader *h;

	if (s->meta.jrnl_blk == InvalidBlockNumber)
		return true;
	p = read_page(s->rel, s->meta.jrnl_blk, SIFT_PT_KEYJRNL, SIFT_KJ_MAGIC, true);
	h = p != NULL ? (const SiftKeyJrnlHeader *) SiftPageGetData(p) : NULL;
	if (h == NULL || h->nslots != SIFT_KJ_SLOTS)
		return give_up(s, "the key journal header is not what the metapage says");
	hold(s, NAMED_JRNL, s->meta.jrnl_blk, 1);
	hold_blocks(s, NAMED_JRNL, h->slot_blk, h->nslots);
	return true;
}

/* The partitions: their directory pages and everything each of them holds */
static bool
hold_partitions(Sweep *s)
{
	SiftManifestRead mr;

	if (s->meta.tier_dir_blk != InvalidBlockNumber)
		return give_up(s, "the index has parts in an object store");
	if (!hold_tree(s, s->meta.partdir_root_blk, s->meta.partdir_level))
		return false;
	mr = sift_manifest_read_range(s->rel, NULL, NULL);
	for (int i = 0; i < mr.n; i++)
		if (!hold_partition(s, &mr.entries[i]))
			return false;
	return true;
}

/* The lists of the allocator with their pages, the queue, and the free blocks of every slab; the root is copied for the slabs */
static void
hold_allocator(Sweep *s)
{
	Buffer		buf = sift_blk_read(s->rel, SIFT_FSM_BLKNO, BUFFER_LOCK_SHARE);
	const SiftFsmMetaPageData *m;
	const SiftFsmSlabTable *t;
	ExtVec		pages = {0};

	sift_fsm_check_root(s->rel, buf);
	memcpy(s->root.data, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	m = FSMM(s->root.data);
	t = SLABS(s->root.data);
	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		sift_fsm_walk_list(s->rel, m->class_head[k], k, &s->held, &pages);
	sift_fsm_walk_list(s->rel, m->quarantine_head, SIFT_FSM_CLASS_QUEUE, &s->held, &pages);
	for (int i = 0; i < pages.n; i++)
		hold(s, NAMED_LIST, pages.v[i].start, 1);
	for (uint32 i = 0; i < t->nslabs; i++)
		for (uint32 b = 0, run = 0; b <= t->slab[i].nblocks; b++)
		{
			bool		free = b < t->slab[i].nblocks && (t->slab[i].free[b / 64] & (UINT64CONST(1) << (b % 64))) != 0;

			if (free)
				run++;
			else if (run > 0)
			{
				hold(s, NAMED_FREE, t->slab[i].start + b - run, run);
				run = 0;
			}
		}
}

static void
sort_unique_packed(Sweep *s)
{
	int			n = 0;

	if (s->packed.n > 1)
		qsort(s->packed.v, s->packed.n, sizeof(SiftFsmExtent), sift_fsm_ext_cmp);
	for (int i = 0; i < s->packed.n; i++)
		if (n == 0 || s->packed.v[n - 1].start != s->packed.v[i].start)
			s->packed.v[n++] = s->packed.v[i];
	for (int i = 0; i < n; i++)
		hold(s, NAMED_PART, s->packed.v[i].start, 1);
}

/* Cuts [start, end) at the edges of the slabs: a metadata page goes back to its bitmap, a block of the data zone to a list, and one extent is never both */
static void
add_orphans(Sweep *s, ExtVec *out, uint64 start, uint64 end)
{
	const SiftFsmSlabTable *t = SLABS(s->root.data);

	while (start < end)
	{
		uint64		stop = end;

		for (uint32 i = 0; i < t->nslabs; i++)
		{
			uint64		lo = t->slab[i].start,
						hi = lo + t->slab[i].nblocks;

			if (start >= lo && start < hi)
				stop = Min(stop, hi);
			else if (lo > start)
				stop = Min(stop, lo);
		}
		sift_fsm_extvec_add(out, (BlockNumber) start, (uint32) (stop - start), InvalidFullTransactionId);
		start = stop;
	}
}

/* The runs of blocks below the high-water mark that nothing names, in order; false when two names overlap or one lies past the mark */
static bool
orphan_runs(Sweep *s, ExtVec *out, uint64 *blocks)
{
	uint64		cur = 0;

	sort_unique_packed(s);
	if (s->held.n > 1)
		qsort(s->held.v, s->held.n, sizeof(SiftFsmExtent), sift_fsm_ext_cmp);
	for (int i = 0; i < s->held.n; i++)
	{
		const SiftFsmExtent *e = &s->held.v[i];

		if (e->nblocks == 0 || e->start < cur)
			return give_up(s, "two names, or a name and a free list, overlap");
		if (e->start > cur)
			add_orphans(s, out, cur, e->start);
		cur = (uint64) e->start + e->nblocks;
	}
	if (cur > s->meta.high_water)
		return give_up(s, "a name lies past the high-water mark");
	if (cur < s->meta.high_water)
		add_orphans(s, out, cur, s->meta.high_water);
	*blocks = 0;
	for (int i = 0; i < out->n; i++)
		*blocks += out->v[i].nblocks;
	return true;
}

/* The start of this server in milliseconds, kept to 32 bits and never 0 (the root of a relation never swept) */
static uint32
server_start(void)
{
	return (uint32) (PgStartTime / 1000) | 1;
}

/*
 * The relations this backend found nothing to sweep in: a sweep that frees nothing from a root whose pool window is clear changes nothing but the stamp of the server, which is not worth a
 * record. A backend never outlives the server, so its memory of a clean sweep is of this server; another backend sweeps once more and finds the same.
 */
#define SWEPT_CLEAN_SLOTS	64

static RelFileLocator swept_clean[SWEPT_CLEAN_SLOTS];
static SiftRing swept_clean_ring;
static uint32 swept_clean_used;

static bool
swept_clean_here(Relation rel)
{
	for (uint32 i = 0; i < swept_clean_used; i++)
		if (RelFileLocatorEquals(swept_clean[i], rel->rd_locator))
			return true;
	return false;
}

static void
remember_swept_clean(Relation rel)
{
	swept_clean[SIFT_RING_SLOT(swept_clean_ring, SWEPT_CLEAN_SLOTS)] = rel->rd_locator;
	if (swept_clean_used < SWEPT_CLEAN_SLOTS)
		swept_clean_used++;
}

bool
sift_fsm_sweep_due(Relation rel)
{
	Buffer		buf;
	bool		due;

	if (swept_clean_here(rel))
		return false;
	buf = sift_blk_read(rel, SIFT_FSM_BLKNO, BUFFER_LOCK_SHARE);
	due = FSMM(BufferGetPage(buf))->swept_start != server_start();
	UnlockReleaseBuffer(buf);
	return due;
}

/* Records that this server has swept the relation; when the sweep named every page and freed what nothing names, the window of the last batch of a pool has none that it leaves unnamed any more */
static void
mark_swept(Relation rel, bool complete)
{
	Buffer		buf0 = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(rel);
	Page		root = sift_xlog_register(&x, sift_fsm_lock_root(rel), 0);

	FSMM(root)->swept_start = server_start();
	if (complete)
		FSMM(root)->pool_lo = FSMM(root)->pool_mask = 0;
	sift_fsm_root_seal(root);
	sift_xlog_done(&x);
	UnlockReleaseBuffer(buf0);
}

/* Names everything the relation holds; false (and why) when it holds something the sweep cannot name */
static bool
collect(Sweep *s)
{
	hold(s, NAMED_DIR, 0, SIFT_FSM_BLKNO + 1);
	hold_allocator(s);
	return hold_partitions(s) && hold_scopes(s) && hold_projections(s) && hold_journal(s);
}

static bool
orphans_safe(Sweep *s, uint64 blocks)
{
	if (blocks * 100 > (uint64) s->meta.high_water * SIFT_FSM_SWEEP_MAX_PCT)
		return give_up(s, "the orphans are over half of the index");
	return true;
}

static uint64
release_orphans(Sweep *s, const ExtVec *orphans)
{
	SiftExtent *ext = palloc(Max(orphans->n, 1) * sizeof(SiftExtent));
	uint64		blocks = 0;

	for (int i = 0; i < orphans->n; i++)
	{
		ext[i] = (SiftExtent) {.start = orphans->v[i].start,.nblocks = orphans->v[i].nblocks};
		blocks += ext[i].nblocks;
	}
	sift_fsm_release(s->rel, ext, orphans->n, sift_maint_standby_streams() ? ReadNextFullTransactionId() : InvalidFullTransactionId);
	(void) sift_fsm_reclaim(s->rel);
	pfree(ext);
	return blocks;
}

static void
say_named(const Sweep *s, uint64 orphans)
{
	StringInfoData buf;

	initStringInfo(&buf);
	for (int k = 0; k < NNAMED; k++)
		appendStringInfo(&buf, "%s%s %u", k > 0 ? ", " : "", NAMED_LABEL[k], s->named[k]);
	ereport(DEBUG1, (errmsg("sift index \"%s\": the orphan sweep named %s; free and queued extents %d; orphans %llu", RelationGetRelationName(s->rel), buf.data, s->held.n, (unsigned long long) orphans)));
	pfree(buf.data);
}

static uint64
sweep_locked(Relation rel)
{
	Sweep		s = {.rel = rel,.meta = sift_col_meta_read(rel),.cx = CurrentMemoryContext,
		.open_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift orphan sweep part")};
	ExtVec		orphans = {0};
	uint64		blocks = 0;
	bool		complete = false;

	if (!collect(&s) || !orphan_runs(&s, &orphans, &blocks) || !orphans_safe(&s, blocks))
	{
		ereport(DEBUG1, (errmsg("sift index \"%s\": the orphan sweep gave up: %s", RelationGetRelationName(rel), s.why)));
		blocks = 0;
	}
	else
	{
		say_named(&s, blocks);
		if (blocks > 0)
			blocks = release_orphans(&s, &orphans);
		complete = true;
	}
	if (blocks > 0)
		ereport(LOG, (errmsg("sift index \"%s\": the orphan sweep freed %llu blocks", RelationGetRelationName(rel), (unsigned long long) blocks)));
	if (complete && blocks == 0 && FSMM(s.root.data)->pool_mask == 0)
		remember_swept_clean(rel);
	else
		mark_swept(rel, complete);
	return blocks;
}

/* The relation is its own: no other backend or maintenance worker can write it, so no writer is between an allocation and its commit */
static bool
sweep_allowed(Relation rel)
{
	if (RecoveryInProgress() || rel->rd_createSubid != InvalidSubTransactionId || rel->rd_firstRelfilelocatorSubid != InvalidSubTransactionId)
		return false;
	return rel->rd_rel->relkind != RELKIND_INDEX || rel->rd_index->indisready;
}

static uint64
sweep_in_own_context(Relation rel)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift orphan sweep");
	MemoryContext old = MemoryContextSwitchTo(cx);
	uint64		freed = sweep_locked(rel);

	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
	return freed;
}

bool
sift_fsm_names_all_extents(Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);

	if (rel->rd_rel->relkind == RELKIND_INDEX)
		return true;
	for (int i = 0; i < desc->natts; i++)
		if (TupleDescAttr(desc, i)->attlen < 0)
			return false;
	return true;
}

/* Another VACUUM reads the manifest (this backend's own run, which is the one that sweeps, has finished with what it read) */
static bool
vacuum_read_by_another(Relation rel)
{
	if (!ConditionalLockPage(rel, SIFT_F2_VACUUM_READ_LOCK_BLKNO, ExclusiveLock))
		return true;
	UnlockPage(rel, SIFT_F2_VACUUM_READ_LOCK_BLKNO, ExclusiveLock);
	return false;
}

uint64
sift_fsm_sweep_orphans(Relation rel)
{
	bool		had_lock = CheckRelationLockedByMe(rel, ExclusiveLock, true);
	uint64		freed = 0;

	if (!sweep_allowed(rel) || (!had_lock && !ConditionalLockRelation(rel, ExclusiveLock)))
		return 0;
	if (ConditionalLockPage(rel, SIFT_F2_LOAD_LOCK_BLKNO, ExclusiveLock))
	{
		if (!vacuum_read_by_another(rel))
		{
			sift_fsm_meta_pool_drain(rel);
			freed = sweep_in_own_context(rel);
		}
		UnlockPage(rel, SIFT_F2_LOAD_LOCK_BLKNO, ExclusiveLock);
	}
	if (!had_lock)
		UnlockRelation(rel, ExclusiveLock);
	return freed;
}
