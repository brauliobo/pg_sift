/*
 * col/col_meta.c - open, read and build the metapage of a sift_col index or sift_table of format major 2 (col/col_meta.h).
 */
#include "core/host.h"

#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bulk_write.h"

#include "col/f2/col_meta.h"
#include "core/extlock.h"
#include "core/f2/fsm.h"
#include "core/f2/manifest.h"
#include "core/page_io.h"

#define INLINE_HDR(p)	((const SiftMetaInlineHeader *) (SiftPageGetData(p) + SIFT_COL_META(p)->inline_off))

static void
check_inline(Relation rel, Page page, const SiftColMetaPageData *m)
{
	const SiftMetaInlineHeader *h = INLINE_HDR(page);

	if (!(m->common.ro_compat_flags & SIFT_ROCOMPAT_META_INLINE) || m->inline_off != sizeof(SiftColMetaPageData) || m->common.struct_size != (uint32) m->inline_off + m->inline_len ||
		m->inline_len < sizeof(SiftMetaInlineHeader) || h->magic != SIFT_META_INLINE_MAGIC || h->version != SIFT_META_INLINE_VERSION || h->parts_off != sizeof(SiftMetaInlineHeader) ||
		h->n_inline_parts > SIFT_META_INLINE_MAX_PARTS || m->inline_len != sizeof(SiftMetaInlineHeader) + h->n_inline_parts * sizeof(SiftPartitionEntry) || h->n_inline_parts != m->npartitions)
		SIFT_CORRUPT(rel, "The inline summary of the metapage is damaged (%u bytes at %u, %u partitions of %u).", m->inline_len, m->inline_off, h->n_inline_parts, m->npartitions);
}

SiftShmIndexMeta
sift_col_meta_shm(const Relation rel, const SiftColMetaPageData *m)
{
	return (SiftShmIndexMeta) {
		.locator = rel->rd_locator, .manifest_version = m->manifest_version, .scope_dir_gen = m->scope_dir_gen, .partdir_root_blk = m->partdir_root_blk,
		.scope_dir_blk = m->scope_dir_blk, .partdir_level = m->partdir_level, .nscopes = m->nscopes, .npartitions = m->npartitions, .total_rows = m->total_rows,
		.total_dead = m->total_dead, .total_bytes = m->total_bytes, .oldest_active_xid = XidFromFullTransactionId(m->oldest_active_xid), .meta_flags = m->state_flags,
		.compat_flags = m->common.compat_flags, .ro_compat_flags = m->common.ro_compat_flags, .inline_off = m->inline_off, .inline_len = m->inline_len,
		.vm_all_visible_vblocks = m->vm_all_visible_vblocks, .vm_all_frozen_vblocks = m->vm_all_frozen_vblocks, .vm_total_vblocks = m->vm_total_vblocks,
		.state_flags = rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP ? SIFT_SHM_SLOT_PRIVATE : 0
	};
}

const SiftColMetaPageData *
sift_col_meta_validate(Relation rel, Page page, bool for_write)
{
	const SiftColMetaPageData *m;

	(void) sift_store_open_meta(rel, page, rel->rd_rel->relkind == RELKIND_INDEX ? SIFT_AM_COL : SIFT_AM_TABLE, sizeof(SiftColMetaPageData), for_write);
	m = SIFT_COL_META(page);
	if (m->inline_len != 0)
		check_inline(rel, page, m);
	if (m->partdir_level > SIFT_PARTDIR_MAX_LEVEL || m->fsm_meta_blk != SIFT_FSM_BLKNO || m->high_water < SIFT_FIRST_SLAB_BLKNO ||
		m->meta_slab_end > m->high_water || m->partdir_root_blk == InvalidBlockNumber || m->partdir_root_blk >= m->high_water)
		SIFT_CORRUPT(rel, "The metapage anchors are out of range (partition directory %u at level %u, allocator %u, high water %u).",
					 m->partdir_root_blk, m->partdir_level, m->fsm_meta_blk, m->high_water);
	if ((m->proj_dir_blk == InvalidBlockNumber) != (m->nprojs == 0) || (m->proj_dir_blk != InvalidBlockNumber && m->proj_dir_blk >= m->high_water) ||
		((m->common.ro_compat_flags & SIFT_ROCOMPAT_PROJECTIONS) != 0) != (m->nprojs != 0))
		SIFT_CORRUPT(rel, "The projection directory anchor is out of range (%u projections, directory %u, high water %u).", m->nprojs, m->proj_dir_blk, m->high_water);
	if (m->jrnl_blk != InvalidBlockNumber && m->jrnl_blk >= m->high_water)
		SIFT_CORRUPT(rel, "The key journal anchor is out of range (block %u, high water %u).", m->jrnl_blk, m->high_water);
	return m;
}

Buffer
sift_col_meta_lock(Relation rel, int lockmode)
{
	Buffer		buf = sift_blk_read(rel, SIFT_METAPAGE_BLKNO, lockmode);

	sift_col_meta_validate(rel, BufferGetPage(buf), lockmode == BUFFER_LOCK_EXCLUSIVE);
	return buf;
}

SiftColMetaPageData
sift_col_meta_read(Relation rel)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_SHARE);
	SiftColMetaPageData copy = *SIFT_COL_META(BufferGetPage(buf));

	UnlockReleaseBuffer(buf);
	return copy;
}

SiftColMetaPageData
sift_col_meta_read_inline(Relation rel, SiftPartitionEntry **inline_entries, int *n)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_SHARE);
	Page		page = BufferGetPage(buf);
	SiftColMetaPageData copy = *SIFT_COL_META(page);

	*inline_entries = NULL;
	*n = 0;
	if (copy.inline_len != 0)
	{
		*n = INLINE_HDR(page)->n_inline_parts;
		*inline_entries = palloc(Max(*n, 1) * sizeof(SiftPartitionEntry));
		memcpy(*inline_entries, (const char *) INLINE_HDR(page) + INLINE_HDR(page)->parts_off, *n * sizeof(SiftPartitionEntry));
	}
	UnlockReleaseBuffer(buf);
	return copy;
}

void
sift_col_meta_set_inline(Page p0, const SiftPartitionEntry *ents, int n)
{
	SiftColMetaPageData *m = SIFT_COL_META(p0);
	SiftMetaInlineHeader *h = (SiftMetaInlineHeader *) ((char *) m + sizeof(SiftColMetaPageData));

	Assert(n <= SIFT_META_INLINE_MAX_PARTS);
	if (n < 0)
	{
		m->inline_off = m->inline_len = 0;
		memset(h, 0, sizeof(SiftMetaInlineHeader) + SIFT_META_INLINE_MAX_PARTS * sizeof(SiftPartitionEntry));
		sift_meta_seal(p0, sizeof(SiftColMetaPageData));
		return;
	}
	*h = (SiftMetaInlineHeader) {.magic = SIFT_META_INLINE_MAGIC,.version = SIFT_META_INLINE_VERSION,.flags = SIFT_META_INLINE_PARTDIR,.time_leaf = {-1, -1},.n_inline_parts = n,
	.parts_off = sizeof(SiftMetaInlineHeader)};
	memcpy((char *) h + sizeof(SiftMetaInlineHeader), ents, n * sizeof(SiftPartitionEntry));
	memset((char *) h + sizeof(SiftMetaInlineHeader) + n * sizeof(SiftPartitionEntry), 0, (SIFT_META_INLINE_MAX_PARTS - n) * sizeof(SiftPartitionEntry));
	m->inline_off = sizeof(SiftColMetaPageData);
	m->inline_len = sizeof(SiftMetaInlineHeader) + n * sizeof(SiftPartitionEntry);
	m->common.ro_compat_flags |= SIFT_ROCOMPAT_META_INLINE;
	sift_meta_seal(p0, (Size) m->inline_off + m->inline_len);
}

static void
build_pages(Page p0, Page p1, Page p2, SiftAmKind am, uint64 compat_flags, uint32 schema_hash, uint32 state_flags)
{
	SiftColMetaPageData *m;
	BlockNumber high_water = (BlockNumber) ((uint64) SIFT_FIRST_SLAB_BLKNO + SIFT_FIRST_SLAB_BLOCKS);

	sift_page_init_part(p0, SIFT_PT_META, 0, 0);
	m = SIFT_COL_META(p0);
	m->common = (SiftMetaCommon) {.magic = SIFT_MAGIC,.format_major = SIFT_FORMAT2_MAJOR,.format_minor = SIFT_FORMAT2_MINOR,.am_kind = am,
	.compat_flags = compat_flags | SIFT_COMPAT_PARTITIONED | SIFT_COMPAT_TIME_BOUNDS};
	m->schema_hash = schema_hash;
	m->state_flags = state_flags;
	m->next_gen = 1;
	m->next_part_id = 2;
	m->manifest_version = 1;
	m->partdir_root_blk = SIFT_FIRST_SLAB_BLKNO;
	m->fsm_meta_blk = SIFT_FSM_BLKNO;
	m->high_water = m->meta_slab_end = high_water;
	m->npartitions = 1;
	m->oldest_active_xid = InvalidFullTransactionId;
	m->scope_dir_blk = InvalidBlockNumber;
	m->proj_dir_blk = InvalidBlockNumber;
	m->tier_dir_blk = InvalidBlockNumber;
	m->jrnl_blk = InvalidBlockNumber;
	sift_partdir_build_root(p1);
	sift_col_meta_set_inline(p0, (const SiftPartitionEntry *) (SiftPageGetData(p1) + sizeof(SiftPartDirPageData)), 1);
	sift_fsm_build_root(p2, SIFT_FIRST_SLAB_BLKNO);
}

/* The last block of the zeroed first slab, logged: its replay extends the file of a standby or of a recovery to the high water mark the metapage names */
static void
log_extension_end(Relation rel, Buffer last)
{
	if (RelationNeedsWAL(rel))
	{
		START_CRIT_SECTION();
		MarkBufferDirty(last);
		log_newpage_buffer(last, false);
		END_CRIT_SECTION();
	}
	UnlockReleaseBuffer(last);
}

void
sift_col_meta_init(Relation rel, SiftAmKind am, uint64 compat_flags, uint32 schema_hash, uint32 state_flags)
{
	SiftXlog	x;
	Page		p0,
				p1,
				p2;
	BlockNumber high_water = (BlockNumber) ((uint64) SIFT_FIRST_SLAB_BLKNO + SIFT_FIRST_SLAB_BLOCKS);

	if (sift_main_nblocks(rel) < high_water)
		log_extension_end(rel, sift_extend_zeroed_to(rel, high_water));
	x = sift_xlog_start(rel);
	p0 = sift_xlog_register(&x, sift_blk_new(rel, SIFT_METAPAGE_BLKNO), GENERIC_XLOG_FULL_IMAGE);
	p1 = sift_xlog_register(&x, sift_blk_new(rel, SIFT_FIRST_SLAB_BLKNO), GENERIC_XLOG_FULL_IMAGE);
	p2 = sift_xlog_register(&x, sift_blk_new(rel, SIFT_FSM_BLKNO), GENERIC_XLOG_FULL_IMAGE);
	build_pages(p0, p1, p2, am, compat_flags, schema_hash, state_flags);
	sift_xlog_done(&x);
}

/* The init fork of an unlogged index: the three pages the index starts with and the zeros of the first slab, written through the bulk writer, which logs each page before it checksums and writes it, and syncs the fork */
void
sift_col_meta_init_fork(Relation rel, SiftAmKind am, uint64 compat_flags, uint32 schema_hash, uint32 state_flags)
{
	BulkWriteState *bulk = smgr_bulk_start_rel(rel, INIT_FORKNUM);
	BulkWriteBuffer buf[SIFT_FIRST_SLAB_BLKNO + SIFT_FIRST_SLAB_BLOCKS];

	for (BlockNumber b = 0; b < lengthof(buf); b++)
	{
		buf[b] = smgr_bulk_get_buf(bulk);
		memset(buf[b]->data, 0, BLCKSZ);
	}
	build_pages(buf[SIFT_METAPAGE_BLKNO]->data, buf[SIFT_FIRST_SLAB_BLKNO]->data, buf[SIFT_FSM_BLKNO]->data, am, compat_flags, schema_hash, state_flags);
	for (BlockNumber b = 0; b < lengthof(buf); b++)
		smgr_bulk_write(bulk, b, buf[b], true);
	smgr_bulk_finish(bulk);
}

void
sift_col_meta_set_schema_hash(Relation rel, uint32 schema_hash)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(rel);
	Page		p0 = sift_xlog_register(&x, buf, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);

	m->schema_hash = schema_hash;
	sift_meta_seal(p0, m->common.struct_size);
	sift_xlog_done(&x);
}

const SiftColMetaPageData *
sift_col_open_meta(Relation rel, SiftColMetaPageData *out)
{
	*out = sift_col_meta_read(rel);
	return out;
}
