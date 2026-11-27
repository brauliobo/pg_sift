/*
 * table/f2/col_delta/dir.h - the directory of a long column delta chain (SIFT_PT_COL_DELTA_DIR = 47).
 *
 * A chain of up to SIFT_CDDIR_FLAT_PAGES pages is the flat chain of f2_col_delta.h. The page that would make it longer makes its directory: pages of SiftColDeltaDirEntry, one for each page
 * of the chain, oldest first, that say what the page holds (the part, the granules, the attributes) without reading it. The anchor then names the newest directory page
 * (SIFT_CDLA_INDEXED_DIR) and the meta page says so for every reader (SIFT_COMPAT_COL_DELTA_DIR). The data pages keep their links and depths: the directory is an index over them, and a
 * reader that wants one part, a few granules or a few attributes reads the entries and only the pages they do not exclude. A chain has no length at which an update is refused;
 * SIFT_CDDIR_SOFT_FOLD_PAGES asks the maintenance worker for a fold.
 *
 * A directory page is written in place only at its newest end, under the partition lock, before the anchor that counts its entries is published: a reader takes the first
 * anchor->npages entries, so an entry added since is not one it sees. An entry is only ever widened, and a page of the chain is widened before the records that need it are written.
 */
#ifndef SIFT_TBL_F2_COL_DELTA_DIR_H
#define SIFT_TBL_F2_COL_DELTA_DIR_H

#include "table/f2/col_delta/col_delta.h"

#define SIFT_CDDIR_MAGIC				0x43444452U		/* "CDDR" */
#define SIFT_CDDIR_VER					1
#define SIFT_CDDIR_MAX_ENTRIES			250
#define SIFT_CDDIR_FLAT_PAGES			4		/* a chain of this many pages or fewer has no directory */
#define SIFT_CDDIR_SOFT_FOLD_PAGES		SIFT_COL_DELTA_FOLD_PAGES	/* the maintenance worker folds the chain, no update waits for it */

#define SIFT_CDD_FLAG_TID_ONLY			(UINT16_C(1) << 0)	/* the page holds address changes only (SIFT_CD_ATT_TID_REMAP) */
#define SIFT_CDD_FLAG_HAS_BATCH			(UINT16_C(1) << 1)	/* the page holds SIFT_CDENT_FLAG_BATCH records */
#define SIFT_CDD_FLAG_HAS_BLOB			(UINT16_C(1) << 2)	/* the page holds SIFT_CDENT_FLAG_BLOB_REF columns */
#define SIFT_CDD_FLAGS_KNOWN			(SIFT_CDD_FLAG_TID_ONLY | SIFT_CDD_FLAG_HAS_BATCH | SIFT_CDD_FLAG_HAS_BLOB)

typedef struct SiftColDeltaDirHdr
{
	uint32		magic;
	uint16		version;
	uint16		flags;			/* 0 */
	uint32		crc32c;			/* at SIFT_PAGE_CRC_OFF */
	uint32		part_id;
	uint32		nentries;		/* 1..SIFT_CDDIR_MAX_ENTRIES */
	uint32		total_delta_pages;	/* the data pages of the chain up to and with this directory page's entries */
	BlockNumber next_dir_blk;	/* the next older directory page, or InvalidBlockNumber */
	BlockNumber prev_dir_blk;	/* reserved: InvalidBlockNumber (a directory page is never changed once a newer one exists) */
	FullTransactionId min_xid;	/* the writers of the entries' pages */
	FullTransactionId max_xid;
} SiftColDeltaDirHdr;

StaticAssertDecl(sizeof(SiftColDeltaDirHdr) == 48, "SiftColDeltaDirHdr must be 48 bytes");
StaticAssertDecl(offsetof(SiftColDeltaDirHdr, crc32c) == SIFT_PAGE_CRC_OFF, "SiftColDeltaDirHdr.crc32c must sit at SIFT_PAGE_CRC_OFF");
StaticAssertDecl(offsetof(SiftColDeltaDirHdr, min_xid) == 32, "SiftColDeltaDirHdr.min_xid must sit at byte 32");

typedef struct SiftColDeltaDirEntry
{
	BlockNumber blkno;			/* the SIFT_PT_COL_DELTA page */
	uint16		nrecs;
	uint16		flags;			/* SIFT_CDD_FLAG_* */
	uint64		part_gen;		/* the part every record of the page is for, 0 for several */
	uint32		granule_lo;		/* the granules the records name */
	uint32		granule_hi;
	uint64		attnum_bloom;	/* sift_cd_att_bit of every attribute a record patches */
} SiftColDeltaDirEntry;

StaticAssertDecl(sizeof(SiftColDeltaDirEntry) == 32, "SiftColDeltaDirEntry must be 32 bytes");
StaticAssertDecl(offsetof(SiftColDeltaDirEntry, part_gen) == 8, "SiftColDeltaDirEntry.part_gen must sit at byte 8");
StaticAssertDecl(offsetof(SiftColDeltaDirEntry, attnum_bloom) == 24, "SiftColDeltaDirEntry.attnum_bloom must sit at byte 24");
StaticAssertDecl(sizeof(SiftColDeltaDirHdr) + SIFT_CDDIR_MAX_ENTRIES * sizeof(SiftColDeltaDirEntry) <= SIFT_PAGE_PAYLOAD, "a full directory page must fit a page");

static inline const SiftColDeltaDirEntry *
sift_cddir_page_entries(const char *page)
{
	return (const SiftColDeltaDirEntry *) (SiftPageGetData(page) + sizeof(SiftColDeltaDirHdr));
}

static inline const SiftColDeltaDirHdr *
sift_cddir_page_hdr(const char *page)
{
	return (const SiftColDeltaDirHdr *) SiftPageGetData(page);
}

extern bool sift_cddir_entry_wanted(const SiftColDeltaDirEntry *e, const SiftColDeltaPrune *prune);

/* The entry of a page of the chain the batch is for (blk) or that is on disk (the records read back) */
extern SiftColDeltaDirEntry sift_cddir_entry_of_batch(BlockNumber blk, const SiftColDeltaBatch *b);
extern SiftColDeltaDirEntry sift_cddir_entry_of_page(BlockNumber blk, const char *page);

/* The entry made to hold what the batch adds to its page: never narrower */
extern void sift_cddir_entry_widen(SiftColDeltaDirEntry *e, const SiftColDeltaBatch *b);

extern void sift_cddir_page_init(Page page, uint64 gen, uint32 part_id, BlockNumber next_dir_blk, uint32 pages_before);
extern bool sift_cddir_page_add(Page page, const SiftColDeltaDirEntry *e, FullTransactionId min_xid, FullTransactionId max_xid);
extern void sift_cddir_page_widen_last(Page page, const SiftColDeltaBatch *b);
extern const char *sift_cddir_page_check(const char *page);

/* The chain of an anchor with a directory: the pages its entries name that the prune (NULL: none) does not leave out */
extern const char *sift_cddir_chain_read(const SiftColDeltaAnchor *anchor, const SiftColDeltaPageReader *reader, const SiftColDeltaPrune *prune, SiftColDeltaChain *out);

/* The data pages of the chain the anchor names, oldest first, as the entries of its directory pages say; NULL why on success. n is the count of entries a read takes, blks the block of each */
extern const char *sift_cddir_chain_entries(const SiftColDeltaAnchor *anchor, const SiftColDeltaPageReader *reader, SiftColDeltaDirEntry **entries, BlockNumber **dir_blks, int *ndir);

/* The directory of the flat chain of n pages, the newest at head: its entries, oldest first */
extern const char *sift_cddir_promote_entries(BlockNumber head, int n, const SiftColDeltaPageReader *reader, SiftColDeltaDirEntry *entries, FullTransactionId *min_xid, FullTransactionId *max_xid);

/* The pages of a chain read through the buffers (f2_col_delta_io.c); snap_manifest_ver is the scan's, 0 for a writer or a vacuum */
typedef struct SiftColDeltaDiskReader
{
	Relation	rel;
	uint32		part_id;
	uint64		snap_manifest_ver;
} SiftColDeltaDiskReader;

extern SiftColDeltaPageReader sift_col_delta_disk_reader(SiftColDeltaDiskReader *d);

typedef struct SiftColDeltaDiskChain
{
	Relation	rel;
	const SiftPartitionEntry *entry;
	SiftColDeltaAnchor *capture;	/* the anchor is the caller's to publish */
	uint64		gen_next;		/* generations reserved ahead for the pages of all the batches of a chain */
	uint32		gen_left;
} SiftColDeltaDiskChain;

/* The protocol's host for a chain on disk, under the partition lock; the pages are written with generic WAL and the anchor is the manifest's, or the caller's to publish (capture) */
extern SiftColDeltaWriter sift_col_delta_disk_writer(SiftColDeltaDiskChain *d);

/* All the records of b appended in place to the newest page of the chain of anchor a, which they leave as it is (its flags, its pages); false, and nothing written, when they would change it */
extern bool sift_col_delta_disk_append_in_place(SiftColDeltaDiskChain *d, const SiftColDeltaAnchor *a, SiftColDeltaBatch *b, FullTransactionId next);

/* The directory pages on disk (generic WAL, the partition lock held) */
extern BlockNumber sift_cddir_io_newest_data(Relation rel, uint32 part_id, BlockNumber dir_head);
extern BlockNumber sift_cddir_io_promote(Relation rel, uint32 part_id, BlockNumber data_head, int npages);
extern BlockNumber sift_cddir_io_add(Relation rel, uint32 part_id, BlockNumber dir_head, const BlockNumber *blks, const SiftColDeltaBatch *parts, int n);

#endif							/* SIFT_TBL_F2_COL_DELTA_DIR_H */
