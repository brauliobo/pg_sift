/*
 * sift_col_seg.h - L2 of the column engine: immutable column segments over the page store.
 * Writer (build, promotion, merge), reader (units, dictionaries, zones, liveness), VACUUM, verification.
 *
 * A segment is the SEGHEADER page, the stream (UNITS, DICTS, ZONES, SBDIR,
 * SCHEMA) and the LIVE pages. The stream is written through sift_sw_*; sift_store_immutable_extents / sift_store_referenced_blocks / the
 * orphan sweep see it as a BM25 segment.
 */
#ifndef SIFT_COL_SEG_H
#define SIFT_COL_SEG_H

#include "sift_host.h"

#include "sift_col_enc.h"
#include "sift_col_preagg.h"
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
/* pol[ncols]: the compression policy of each leaf (NULL: plain FOR/PACK chunks), valid until sift_colw_finish; format: the column format minor written (sift.col_format_minor) */
extern SiftColSegWriter *sift_colw_begin(SiftWriteTarget *wt, uint64 gen, const SiftColSchema *schema, const SiftCcPolicy *pol,
										 int format, MemoryContext mcxt);
/*
 * The owner's tag of the segment: SiftSegEntry.reserved (the table AM's xmin, 0 = none) and, in the segment header, aux (its cmin). sift_colw_finish
 * stores both; a merge or a rewrite of tagged segments sets them again. The owner clears the tag in the entry when the rows are final.
 */
extern void sift_colw_set_tag(SiftColSegWriter *w, uint32 tag, uint32 aux);
/*
 * The pre-aggregates the writer computes for every unit (docs/COL-PREAGG.md): the default spec unless the owner sets one (the reloption
 * `preagg`). Set before the first sift_colw_add.
 */
extern void sift_colw_set_preagg(SiftColSegWriter *w, const SiftCPaggSpec *spec);
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
/* The aux half of the tag the segment was written with (sift_colw_set_tag) */
#define SIFT_CSEG_TAG_AUX(seg)	((uint32) (seg)->hdr->sum_dl)
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
/* The bytes the dictionary of a STR leaf takes in the stream (0 for other leaves), as stored: smaller than its parsed size when it is compressed */
extern uint64 sift_cseg_dict_stored(SiftColSegment *seg, int col);

/* The zone of leaf col in unit u (the zone arrays of the leaf are read on first use) */
extern void sift_cseg_zone(SiftColSegment *seg, int col, uint32 u, SiftCZone *z);

/* Adds what the zones and the dictionary of the segment say about leaf col to acc (sift_col_leaf_stats_init first). Reads the leaf's zone arrays and four bytes of its dictionary, no chunk */
extern void sift_col_leaf_stats_init(SiftColLeafStats *acc);
extern void sift_cseg_leaf_stats(SiftColSegment *seg, int col, SiftColLeafStats *acc);

/*
 * Pre-aggregates of unit u (a segment of this format always has the sidecar). sift_cseg_agg: the zone and, for an INT leaf, the exact sum and
 * sum of squares. sift_cseg_cube: the unit's cube blob for sift_cpagg_decode (NULL: the unit has none), valid until the next call on seg.
 * sift_cseg_ecube: the unit's element blob (column format minor 4; NULL: none, or a segment of minor 3), valid until the next call of it on seg.
 * sift_cseg_dim_cube: the blob that holds the histogram of dim, the cube or the list's part of the element blob, for sift_cpagg_decode_dim.
 * Sizes of the whole sidecar and the units that have a cube or element cubes, for sift.col_index_info.
 */
extern void sift_cseg_agg(SiftColSegment *seg, int col, uint32 u, SiftCAgg *a);
extern const uint8 *sift_cseg_cube(SiftColSegment *seg, uint32 u, uint32 *len);
extern const uint8 *sift_cseg_ecube(SiftColSegment *seg, uint32 u, uint32 *len);
extern const uint8 *sift_cseg_dim_cube(SiftColSegment *seg, uint32 u, int dim, uint32 *len);
/* the rollup blob of a list in unit u (NULL, len 0: none), for sift_cpagg_rollup_decode */
extern const uint8 *sift_cseg_rollup(SiftColSegment *seg, uint32 u, int list, uint32 *len);
extern uint64 sift_cseg_preagg_bytes(SiftColSegment *seg);
extern uint32 sift_cseg_preagg_units(SiftColSegment *seg);

/* Clears the bits (one per unit, nsb bits) of the units whose zone cannot satisfy the test, with the zone kernels */
extern void sift_cseg_prune(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep);

/* Liveness: is any row of the SB's unit dead; the live bits of unit u's rows (nrows bits, padded words) */
extern bool sift_cseg_dirty(SiftColSegment *seg, uint32 u);
extern void sift_cseg_live(SiftColSegment *seg, uint32 u, uint64 *bits);
extern uint64 sift_cseg_live_rows(SiftColSegment *seg);

/* Replaces the contents of dst with the live rows of unit u (dst: a batch of the segment's schema) */
extern void sift_cseg_decode_unit(SiftColSegment *seg, uint32 u, SiftColBatch *dst);

/*
 * Does a unit flag the overflow of a leaf while no live row holds an overflow value in it (the rows that did are dead)? The flag stays with the
 * dead rows until the segment is rewritten, and a query that reads the leaf keeps the normal plan (docs/COL-PLAN.md): the rewrite is worth it
 */
extern bool sift_cseg_stale_ovf(SiftColSegment *seg);

/*
 * The rows of the units that flag an overflow value that hold one and that dead reports gone from the heap (dead to every snapshot) lose
 * their liveness bit, as in a bulk delete: a VACUUM that skips the vacuuming of the indexes never tells the index which rows died, and the flag
 * would stay with them. Caller holds the maintenance lock. Returns the rows cleared.
 */
typedef bool (*SiftCsegDead) (const ItemPointerData *tid, void *arg);
extern int64 sift_cseg_reap_overflow(SiftColSegment *seg, SiftCsegDead dead, void *arg);

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
 * table's (vis), heap_sbs the superblocks of the table now. No write buffers: the table feeds its delta rows with sift_col_add_batches.
 */
extern SiftColSource *sift_col_open_table(Relation rel, const SiftColSchema *schema, const SiftDirSnapshot *dir, const SiftColVisibility *vis,
										  uint32 heap_sbs, MemoryContext mcxt);

#endif							/* SIFT_COL_SEG_H */
