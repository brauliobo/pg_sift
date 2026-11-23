/*
 * core/meta.c - the checksum, the page layout of format 2 and the open of its metapage (core/meta.h).
 */
#include "core/host.h"

#include "port/pg_crc32c.h"

#include "access/xlog.h"

#include "col/f2/col_meta.h"
#include "core/extlock.h"
#include "core/maint_pacer.h"
#include "core/f2/meta.h"
#include "core/page_io.h"

uint32
sift_crc32c(const void *data, Size len)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, data, len);
	FIN_CRC32C(crc);
	return crc;
}

void
sift_page_init_part(Page page, SiftPageType type, uint64 gen, uint32 part_id)
{
	sift_page_init(page, type, gen);
	SiftPageGetOpaque(page)->part_id = part_id;
}

void
sift_page_seal(Page page)
{
	uint32	   *crc = (uint32 *) (SiftPageGetData(page) + SIFT_PAGE_CRC_OFF);

	*crc = sift_crc32c(SiftPageGetData(page) + SIFT_PAGE_CRC_FROM, SiftPageGetUsed(page) - SIFT_PAGE_CRC_FROM);
}

bool
sift_page_sum_ok(const Page page)
{
	const uint32 *crc = (const uint32 *) (SiftPageGetData(page) + SIFT_PAGE_CRC_OFF);

	return SiftPageGetUsed(page) >= SIFT_PAGE_CRC_FROM &&
		*crc == sift_crc32c(SiftPageGetData(page) + SIFT_PAGE_CRC_FROM, SiftPageGetUsed(page) - SIFT_PAGE_CRC_FROM);
}

Buffer
sift_blk_read(Relation rel, BlockNumber blk, int lockmode)
{
	Buffer		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_NORMAL, sift_maint_ring);

	LockBuffer(buf, lockmode);
	return buf;
}

Buffer
sift_blk_new(Relation rel, BlockNumber blk)
{
	Buffer		buf;

	if (blk < sift_main_nblocks(rel))
	{
		buf = ReadBufferExtended(rel, MAIN_FORKNUM, blk, RBM_ZERO_AND_LOCK, sift_maint_ring);
		return buf;
	}
	return sift_extend_zeroed_to(rel, blk + 1);
}

/*
 * A standby replays the primary's records while a scan reads: a block the scan's manifest names may be recycled (or cut off) by a record replayed since, and then it holds another
 * page, or none. That is a retry, not damage: with the standby's manifest ahead of the scan's (or no manifest version given), the statement fails with a serialization failure and is
 * read again from the new manifest, where the index is whole. Damage that stays at the same manifest version is still damage.
 */
static void
standby_retry(Relation rel, BlockNumber blk, SiftPageType type, uint64 gen, uint64 snap_manifest_ver)
{
	uint64		now;

	if (!RecoveryInProgress())
		return;
	now = sift_col_meta_read(rel).manifest_version;
	if (snap_manifest_ver == 0 || now != snap_manifest_ver)
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("sift index \"%s\": a page was recycled during the scan, retry", RelationGetRelationName(rel)),
				 errdetail("Block %u, expected page type %d generation %llu; manifest %llu, now %llu.", blk, (int) type, (unsigned long long) gen,
						   (unsigned long long) snap_manifest_ver, (unsigned long long) now),
				 errhint("Set hot_standby_feedback = on, or keep sift.standby_conflicts on at the primary.")));
}

/* The pages a reader cannot name the generation of, which every writer stamps with one from the counter of the metapage: a page that holds none was never written as one of these */
static bool
page_is_recycled_by_gen(SiftPageType type)
{
	return type == SIFT_PT_PARTDIR || type == SIFT_PT_L0_SLAB || type == SIFT_PT_PEND_DEL || type == SIFT_PT_COL_DELTA;
}

void
sift_page_expect_snap(Relation rel, Page page, BlockNumber blk, SiftPageType type, uint64 gen, uint32 part_id, uint64 snap_manifest_ver)
{
	const SiftPageOpaqueData *o;

	if (!sift_page_layout_ok(page))
	{
		standby_retry(rel, blk, type, gen, snap_manifest_ver);
		SIFT_CORRUPT(rel, "Block %u has an invalid page layout.", blk);
	}
	o = SiftPageGetOpaque(page);
	if (o->type != type || (part_id != SIFT_PART_ID_ANY && o->part_id != part_id) || (gen != 0 && o->gen != gen) || (gen == 0 && o->gen == 0 && page_is_recycled_by_gen(type)) || o->flags != 0)
	{
		standby_retry(rel, blk, type, gen, snap_manifest_ver);
		SIFT_CORRUPT(rel, "Block %u: expected page type %d of partition %u, found type %d of partition %u generation %llu.",
					 blk, type, part_id, o->type, o->part_id, (unsigned long long) o->gen);
	}
}

void
sift_page_expect(Relation rel, Page page, BlockNumber blk, SiftPageType type, uint64 gen, uint32 part_id)
{
	sift_page_expect_snap(rel, page, blk, type, gen, part_id, 0);
}

static bool
rel_is_table(Relation rel)
{
	return rel->rd_rel->relkind == RELKIND_RELATION || rel->rd_rel->relkind == RELKIND_MATVIEW;
}

static const char *
rebuild_hint(Relation rel)
{
	return rel_is_table(rel) ? "Rebuild the table with VACUUM FULL or dump and reload it before upgrading."
		: psprintf("Run REINDEX INDEX %s to rebuild it in format major %u.", sift_index_ident(rel), SIFT_FORMAT2_MAJOR);
}

static void
refuse_other_major(Relation rel, const SiftMetaCommon *hdr)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("%s \"%s\" uses on-disk format major %u.%u, but this pg_sift requires format major %u",
					rel_is_table(rel) ? "sift_table" : "sift index", RelationGetRelationName(rel),
					hdr->format_major, hdr->format_minor, SIFT_FORMAT2_MAJOR),
			 errhint("%s", rebuild_hint(rel))));
}

static void
refuse_old_layout(Relation rel, const SiftMetaCommon *hdr, Size min_size)
{
	ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("%s \"%s\" has a metapage of %u bytes, written by a pre-release layout of format major %u; this pg_sift needs %zu bytes",
					rel_is_table(rel) ? "sift_table" : "sift index", RelationGetRelationName(rel), hdr->struct_size, SIFT_FORMAT2_MAJOR, min_size),
			 errhint("%s", rebuild_hint(rel))));
}

const SiftMetaCommon *
sift_store_open_meta(Relation rel, Page page, SiftAmKind am, Size min_size, bool for_write)
{
	const SiftMetaCommon *hdr = (const SiftMetaCommon *) SiftPageGetData(page);

	if (!sift_page_layout_ok(page) || SiftPageGetOpaque(page)->type != SIFT_PT_META || SiftPageGetUsed(page) < offsetof(SiftMetaCommon, struct_size))
		SIFT_CORRUPT(rel, "Block %u is not a metapage.", SIFT_METAPAGE_BLKNO);
	if (hdr->magic != SIFT_MAGIC)
		SIFT_CORRUPT(rel, "The metapage magic 0x%08x does not match 0x%08x.", hdr->magic, SIFT_MAGIC);
	if (hdr->format_major != SIFT_FORMAT2_MAJOR)
		refuse_other_major(rel, hdr);
	if (hdr->am_kind != am)
		SIFT_CORRUPT(rel, "The metapage is of access method %u, not %u.", hdr->am_kind, am);
	if (SiftPageGetUsed(page) != hdr->struct_size || hdr->struct_size < sizeof(SiftMetaCommon))
		SIFT_CORRUPT(rel, "The metapage holds %u bytes in a struct of %u bytes.", SiftPageGetUsed(page), hdr->struct_size);
	if (hdr->crc32c != sift_crc32c((const char *) hdr + SIFT_META_CRC_FROM, hdr->struct_size - SIFT_META_CRC_FROM))
		SIFT_CORRUPT(rel, "The metapage checksum does not match its bytes.");
	if (hdr->struct_size < min_size)
		refuse_old_layout(rel, hdr, min_size);
	if ((hdr->compat_flags & ~SIFT_KNOWN_COMPAT_MASK) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift index \"%s\" needs features this pg_sift does not have", RelationGetRelationName(rel)),
				 errdetail("Required feature bits 0x%llx are unknown.", (unsigned long long) (hdr->compat_flags & ~SIFT_KNOWN_COMPAT_MASK)),
				 errhint("Install the pg_sift release that wrote the index.")));
	if (for_write && (hdr->ro_compat_flags & ~SIFT_KNOWN_ROCOMPAT_MASK) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift index \"%s\" can be read but not written by this pg_sift", RelationGetRelationName(rel)),
				 errdetail("Write-protected feature bits 0x%llx are unknown.", (unsigned long long) (hdr->ro_compat_flags & ~SIFT_KNOWN_ROCOMPAT_MASK)),
				 errhint("Install the pg_sift release that wrote the index.")));
	return hdr;
}

void
sift_meta_seal(Page page, Size struct_size)
{
	SiftMetaCommon *hdr = (SiftMetaCommon *) SiftPageGetData(page);

	hdr->struct_size = struct_size;
	SiftPageSetUsed(page, struct_size);
	hdr->crc32c = sift_crc32c((const char *) hdr + SIFT_META_CRC_FROM, struct_size - SIFT_META_CRC_FROM);
}

void
sift_xlog_done(SiftXlog *x)
{
	sift_xlog_finish(x);
	for (int i = 0; i < x->n; i++)
		UnlockReleaseBuffer(x->buf[i]);
}
