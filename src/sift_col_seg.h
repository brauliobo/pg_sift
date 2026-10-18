/*
 * sift_col_seg.h - L2 of the column engine: immutable column segments over the page store (docs/COL.md sections 2.1, 2.3, 2.4, 2.7).
 * Writer (build, promotion, merge), reader (units, dictionaries, zones, liveness), VACUUM, verification.
 *
 * A segment is the SEGHEADER page (a SiftSegHeaderData: the field meanings of docs/COL.md 2.1), the stream (UNITS, DICTS, ZONES, SBDIR,
 * SCHEMA) and the LIVE pages. The stream is written through sift_sw_*; sift_store_immutable_extents / sift_store_referenced_blocks / the
 * orphan sweep see it as a BM25 segment.
 */
#ifndef SIFT_COL_SEG_H
#define SIFT_COL_SEG_H

#include "sift_host.h"

#include "sift_col_enc.h"
#include "sift_store.h"

#define SIFT_COL_SEG_MAGIC_UNIT		0x554C4F43	/* "COLU" */

/* SBDIR entry */
typedef struct SiftColSBRef
{
	uint32		sb;
	uint32		nrows;
	uint64		off;			/* of the unit record in the UNITS section, 8-byte aligned */
	uint64		rank_base;		/* liveness rank of the unit's first row */
	uint32		len;
	uint32		pad;
} SiftColSBRef;

/* ---------- writer ---------- */

typedef struct SiftColSegWriter SiftColSegWriter;

/* Allocates in mcxt. schema must outlive the writer. */
/* pol[ncols]: the compression policy of each leaf (NULL: plain FOR/PACK chunks), valid until sift_colw_finish */
extern SiftColSegWriter *sift_colw_begin(SiftWriteTarget *wt, uint64 gen, const SiftColSchema *schema, const SiftCcPolicy *pol,
										 MemoryContext mcxt);
/* The owner's tag of the segment (SiftSegEntry.reserved: the table AM's xmin, 0 = none); sift_colw_finish stores it in the entry, a rewrite sets it again */
extern void sift_colw_set_tag(SiftColSegWriter *w, uint32 tag);
/* Encodes and writes the batch as the unit of its superblock; superblocks strictly ascending, batch non-empty */
extern void sift_colw_add(SiftColSegWriter *w, const SiftColBatch *b);
extern uint64 sift_colw_rows(const SiftColSegWriter *w);
extern uint64 sift_colw_bytes(const SiftColSegWriter *w);		/* stream bytes so far */
/* Memory the writer holds until the segment ends (zones, unit directory, dictionaries): the caller cuts a segment when it passes its budget */
extern Size sift_colw_memory(const SiftColSegWriter *w);
/* Writes DICTS, ZONES, SBDIR, SCHEMA, the all-live LIVE pages and the header; fills the directory entry (kind IMMUTABLE, format, header_blk, sb_lo/hi, ndocs = rows, bytes) */
extern void sift_colw_finish(SiftColSegWriter *w, SiftSegEntry *entry);

/* ---------- reader ---------- */

typedef struct SiftColSegment
{
	Relation	index;
	SiftSegEntry entry;
	SiftSegHeaderData *hdr;		/* copy of the header page */
	SiftStreamReader sr;
	MemoryContext mcxt;
	const SiftColSchema *schema;
	uint32		nsb;			/* units */
	const SiftColSBRef *sbdir;
	void	   *priv;			/* zone arrays, dictionaries, liveness: loaded on demand */
} SiftColSegment;

/* Reads the header, SCHEMA and SBDIR; ERRCODE_INDEX_CORRUPTED on inconsistencies (type/gen checks included). Everything lives in mcxt. */
extern SiftColSegment *sift_cseg_open(Relation index, const SiftSegEntry *entry, MemoryContext mcxt);
extern void sift_cseg_close(SiftColSegment *seg);		/* releases buffers only */

/* The superblocks the immutable segments of the directory cover (an upper bound of their units) */
extern uint64 sift_cseg_dir_units(const SiftDirSnapshot *dir);

/* Index of the first unit whose sb >= sb, nsb if none */
extern uint32 sift_cseg_find_unit(const SiftColSegment *seg, uint32 sb);

/* The record of unit u, 8-byte aligned, valid until the next call on seg */
extern const uint8 *sift_cseg_unit(SiftColSegment *seg, uint32 u, uint32 *len);

/*
 * Lazy form of sift_cseg_unit for scans that need some columns: the buffer has the unit's full length but only its first *avail
 * (SIFT_COL_UNIT_HEAD at most) bytes are read; sift_cseg_unit_fill reads the bytes [off, off + len) of the record (a chunk) into it. Valid
 * until the next call of any of the three on seg.
 */
extern const uint8 *sift_cseg_unit_head(SiftColSegment *seg, uint32 u, uint32 *len, uint32 *avail);
extern void sift_cseg_unit_fill(SiftColSegment *seg, uint32 u, uint32 off, uint32 len);

/* The segment's dictionary of a STR leaf (loaded on first use, NULL for other leaves) */
extern SiftCDict *sift_cseg_dict(SiftColSegment *seg, int col);

/* The zone of leaf col in unit u (the zone arrays of the leaf are read on first use) */
extern void sift_cseg_zone(SiftColSegment *seg, int col, uint32 u, SiftCZone *z);

/* Clears the bits (one per unit, nsb bits) of the units whose zone cannot satisfy the test (docs/COL.md 2.4), with the zone kernels */
extern void sift_cseg_prune(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep);

/* Liveness (docs/COL.md 2.7): is any row of the SB's unit dead; the live bits of unit u's rows (nrows bits, padded words) */
extern bool sift_cseg_dirty(SiftColSegment *seg, uint32 u);
extern void sift_cseg_live(SiftColSegment *seg, uint32 u, uint64 *bits);
extern uint64 sift_cseg_live_rows(SiftColSegment *seg);

/* Replaces the contents of dst with the live rows of unit u (dst: a batch of the segment's schema) */
extern void sift_cseg_decode_unit(SiftColSegment *seg, uint32 u, SiftColBatch *dst);

/*
 * VACUUM: calls cb for every live row (its TID from the row directory) and clears the liveness bits of the ones it reports dead
 * (dirty flag first, then the bits, generic WAL, at most 4 pages per record). Caller holds the maintenance lock. Returns rows removed,
 * adds to stats like sift_seg_bulkdelete.
 */
extern int64 sift_cseg_bulkdelete(SiftColSegment *seg, IndexBulkDeleteCallback cb, void *cbstate, IndexBulkDeleteResult *stats);

/*
 * Verification (sift.col_verify): header, schema, SBDIR order and ranks, every unit parses, chunks decode with n = nrows (children: the sum
 * of the counts), zones equal the recomputed ones (C4), row keys ascending and inside the SB, dictionary codes in range, liveness
 * ndead. ERRCODE_INDEX_CORRUPTED. Returns the number of live rows.
 */
extern uint64 sift_cseg_verify(SiftColSegment *seg);

/*
 * The source over the column segments of a sift_table (docs/COL-TAM.md section 6; sift_col_scan.c): dir is the caller's directory copy (no snapshot is
 * registered, the caller keeps its own for as long as the source lives), only segments written for schema are read, the visibility is the
 * table's (vis), heap_sbs the superblocks of the table now. No mutable segments: the table feeds its delta rows with sift_col_add_batches.
 */
extern SiftColSource *sift_col_open_table(Relation rel, const SiftColSchema *schema, const SiftDirSnapshot *dir, const SiftColVisibility *vis,
										  uint32 heap_sbs, MemoryContext mcxt);

#endif							/* SIFT_COL_SEG_H */
