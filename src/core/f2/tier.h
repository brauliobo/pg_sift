/*
 * core/f2/tier.h - format major 2: the directory of the parts that live in object storage (SIFT_PT_TIERDIR = 45), the page of it and the checks of that page.
 *
 * A part of level 2 that was moved to an object keeps its descriptor in the part list (SIFT_PART_EXT_REMOTE, header_blk invalid); the directory says which object holds it, how long it is and what its
 * bytes hash to. The pages are immutable once published and copy on write, like the scope directory: a commit of the manifest writes a new chain and the metapage names it (tier_dir_blk, ntier_parts,
 * tier_dir_gen). The refs are ordered by part_gen, which is unique in the relation. No secret and no URI is stored: the object key is derived from the relation and the part (f2_tier.h) and the
 * location from the policy (sift.tier_policies).
 */
#ifndef SIFT_TIER_H
#define SIFT_TIER_H

#include "core/host.h"

#include "core/f2/meta.h"
#include "core/page.h"

#define SIFT_TIERDIR_MAGIC		0x54524452U		/* "TRDR" */
#define SIFT_TIERDIR_VERSION	1

/* SiftTierPartRef.flags */
#define SIFT_TIER_REMOTE_ONLY		(UINT32_C(1) << 0)	/* the local blocks of the part are released: the object is the only copy */
#define SIFT_TIER_LOCAL_CACHE		(UINT32_C(1) << 1)	/* the object is a verified copy of a part that still has its local blocks */
#define SIFT_TIER_DELETE_PENDING	(UINT32_C(1) << 2)	/* the part left the part list (TTL): expires_at is when the object may be deleted, after every standby has replayed the drop */

typedef struct SiftTierPartRef
{
	uint64		part_gen;
	uint32		part_id;		/* the partition the part is listed in */
	uint32		flags;			/* SIFT_TIER_* */
	uint64		byte_len;		/* bytes of the whole object */
	uint32		header_len;		/* the image of the part header page that follows the object header */
	uint32		stream_crc32c;	/* CRC32C of the whole object */
	uint64		obj_hash[2];	/* the first 128 bits of the SHA-256 of the whole object */
	uint32		policy_id;		/* sift.tier_policies */
	uint32		reserved;
	TimestampTz tiered_at;
	TimestampTz expires_at;		/* 0: the part has no TTL deadline; with SIFT_TIER_DELETE_PENDING: when the object may be deleted */
	uint8		etag[24];		/* the ETag the store returned, cut to 24 bytes, NUL padded */
} SiftTierPartRef;

StaticAssertDecl(sizeof(SiftTierPartRef) == 96, "SiftTierPartRef must be 96 bytes");

/* One page of the directory: refs in ascending part_gen; the next page of the chain follows them (same header layout as SiftScopeDirPageData) */
typedef struct SiftTierDirPageData
{
	uint32		magic;
	uint16		nentries;
	uint16		version;		/* SIFT_TIERDIR_VERSION */
	uint32		crc32c;
	BlockNumber next_blk;		/* the next SIFT_PT_TIERDIR page, or InvalidBlockNumber */
	uint64		manifest_version;
	uint64		tier_dir_gen;
	SiftTierPartRef refs[FLEXIBLE_ARRAY_MEMBER];
} SiftTierDirPageData;

StaticAssertDecl(offsetof(SiftTierDirPageData, refs) == 32, "SiftTierDirPageData header is 32 bytes");
StaticAssertDecl(offsetof(SiftTierDirPageData, crc32c) == SIFT_PAGE_CRC_OFF, "the checksum of the page sits where the metadata pages have it");

#define SIFT_TIERDIR_CAP		((SIFT_PAGE_PAYLOAD - offsetof(SiftTierDirPageData, refs)) / sizeof(SiftTierPartRef))

StaticAssertDecl(SIFT_TIERDIR_CAP == 84, "a directory page holds 84 refs");

/*
 * A part that leaves the part list while its object is live (a TTL drop, a compaction that rewrites it) keeps its ref in the same commit, flagged SIFT_TIER_DELETE_PENDING with expires_at set to
 * when the object may be deleted: grace_us after the commit, so that every standby has replayed the drop (a standby that still reads the part before that finds the object). The ref leaves the
 * directory in the commit that follows the deletion.
 */
static inline SiftTierPartRef
sift_tier_ref_expire(const SiftTierPartRef *ref, TimestampTz now, int64 grace_us)
{
	SiftTierPartRef out = *ref;

	out.flags |= SIFT_TIER_DELETE_PENDING;
	out.expires_at = now + grace_us;
	return out;
}

#define SIFT_TIER_DELETE_GRACE_US	(INT64CONST(3600) * 1000000)

/* How long the object of a part that left the relation stays in the store, and with it every object a rewrite of the relation left: a standby that replays the change, or a recovery, may read it */
extern int64 sift_tier_grace_us(void);

/* The most refs a chain holds: the metapage counts them in a uint32 and the manifest bounds the parts of a relation far below it */
#define SIFT_TIER_MAX_REFS		UINT32_C(16777216)

/* Lays the refs (ascending part_gen, at most SIFT_TIERDIR_CAP) out as one sealed page of type SIFT_PT_TIERDIR */
extern void sift_tier_page_fill(Page page, uint64 gen, uint64 manifest_version, BlockNumber next_blk, const SiftTierPartRef *refs, int n);

/* NULL when the page is a valid page of the chain of generation gen, else what is wrong with it */
extern const char *sift_tier_page_check(const Page page, uint64 gen);

/* The ref of the part of generation part_gen in refs (ascending part_gen), or NULL */
extern const SiftTierPartRef *sift_tier_refs_find(const SiftTierPartRef *refs, int n, uint64 part_gen);

/*
 * The refs of a directory with the edit applied: the refs of set replace the ref of their part_gen or are added, the refs of drop_gens are removed. dst has room for n + nset refs; returns the count.
 * A ref with the same part_gen twice in set, or a part_gen in drop_gens that the directory does not hold, is -1 (the caller raises).
 */
extern int	sift_tier_refs_apply(SiftTierPartRef *dst, const SiftTierPartRef *refs, int n, const SiftTierPartRef *set, int nset, const uint64 *drop_gens, int ndrop);

/* The refs in ascending part_gen without duplicates, every flag known and every byte_len and header_len set: NULL, else the first fault */
extern const char *sift_tier_refs_check(const SiftTierPartRef *refs, int n);

#endif							/* SIFT_TIER_H */
