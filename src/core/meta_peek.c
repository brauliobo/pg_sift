/*
 * core/meta_peek.c - the format of a relation from its block 0 (core/meta_peek.h).
 */
#include "postgres.h"

#include "storage/bufmgr.h"
#include "utils/rel.h"

#include "col/f2/col_meta.h"
#include "core/meta_peek.h"
#include "core/ops.h"
#include "core/page_io.h"
#include "core/ring.h"

/* The format of the relations a CREATE INDEX makes */
#define SIFT_NEW_FORMAT_MAJOR	SIFT_FORMAT2_MAJOR

#ifdef SIFT_TESTING
bool		sift_test_create_format1;
#endif

static bool
is_metapage(Page page)
{
	const SiftMetaPageData *meta = (const SiftMetaPageData *) SiftPageGetData(page);

	return !PageIsNew(page) && PageGetSpecialSize(page) == MAXALIGN(sizeof(SiftPageOpaqueData)) &&
		SiftPageGetOpaque(page)->type == SIFT_PT_META && SiftPageGetUsed(page) >= SIFT_META_BASE_SIZE && meta->magic == SIFT_MAGIC;
}

SiftMetaPeek
sift_meta_peek_format(Relation rel)
{
	SiftMetaPeek peek = {.status = SIFT_FMT_EMPTY};
	Buffer		buf;
	Page		page;

	if (RelationGetNumberOfBlocks(rel) == 0)
		return peek;
	buf = ReadBuffer(rel, SIFT_METAPAGE_BLKNO);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	if (is_metapage(page))
	{
		const SiftMetaPageData *meta = (const SiftMetaPageData *) SiftPageGetData(page);

		peek.format_major = meta->format_major;
		peek.format_minor = meta->format_minor;
		peek.status = meta->format_major == SIFT_FORMAT_MAJOR ? SIFT_FMT_V1 : meta->format_major == SIFT_FORMAT2_MAJOR ? SIFT_FMT_V2 : SIFT_FMT_OTHER;
		if (peek.status == SIFT_FMT_V1)
			peek.flags = meta->flags & SIFT_META_QUARANTINED;
		else if (peek.status == SIFT_FMT_V2 && (SIFT_COL_META(page)->state_flags & SIFT_META_FLAG_QUARANTINED))
			peek.flags = SIFT_META_QUARANTINED;
	}
	else
		peek.status = SIFT_FMT_BAD;
	UnlockReleaseBuffer(buf);
	return peek;
}

bool		sift_new_format_journal;

static uint16 new_format_override[SIFT_NEW_FORMAT_DEPTH];
static int	new_format_depth = 0;

void
sift_tbl_push_new_format(uint16 major)
{
	SIFT_INVARIANT(new_format_depth < SIFT_NEW_FORMAT_DEPTH, "the new relation format is pushed more than SIFT_NEW_FORMAT_DEPTH deep");
	new_format_override[new_format_depth++] = major;
}

void
sift_tbl_pop_new_format(void)
{
	SIFT_INVARIANT(new_format_depth > 0, "the new relation format is popped without a push");
	new_format_depth--;
}

uint16
sift_new_relation_format_major(void)
{
	if (sift_new_format_journal)
		return SIFT_FORMAT_MAJOR;
	if (new_format_depth > 0)
		return new_format_override[new_format_depth - 1];
#ifdef SIFT_TESTING
	if (sift_test_create_format1)
		return SIFT_FORMAT_MAJOR;
#endif
	return SIFT_NEW_FORMAT_MAJOR;
}

/* The format major of a file never changes once its block 0 is a metapage, so it is kept by file, and a plan that asks for the format of each index of a table reads no block 0 */
#define MAJOR_MEMO_N 16
static struct MajorMemo
{
	Oid			relid;
	RelFileNumber file;
	uint16		major;
} major_memo[MAJOR_MEMO_N];
static SiftRing major_memo_ring;

uint16
sift_meta_resolve_major(Relation rel)
{
	SiftMetaPeek peek;

	for (int i = 0; i < MAJOR_MEMO_N; i++)
		if (major_memo[i].relid == RelationGetRelid(rel) && major_memo[i].file == rel->rd_locator.relNumber)
			return major_memo[i].major;
	peek = sift_meta_peek_format(rel);
	switch (peek.status)
	{
		case SIFT_FMT_EMPTY:
			return sift_new_relation_format_major();
		case SIFT_FMT_V1:
		case SIFT_FMT_V2:
			major_memo[SIFT_RING_SLOT(major_memo_ring, MAJOR_MEMO_N)] = (struct MajorMemo) {RelationGetRelid(rel), rel->rd_locator.relNumber, peek.format_major};
			return peek.format_major;
		case SIFT_FMT_OTHER:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("sift index \"%s\" has format major %u, which this build (pg_sift %s) does not read",
								   RelationGetRelationName(rel), peek.format_major, SIFT_MODULE_VERSION),
							errhint("Install the pg_sift release that wrote the index, or REINDEX it.")));
		case SIFT_FMT_BAD:
			break;
	}
	SIFT_CORRUPT(rel, "Block 0 is not a metapage.");
	return 0;
}
