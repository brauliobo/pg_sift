/*
 * col/seg.h - L2 of the column engine: immutable column segments over the page store.
 * Writer (build, promotion, merge), reader (units, dictionaries, zones, liveness), VACUUM, verification.
 *
 * A segment is the SEGHEADER page, the stream (UNITS, DICTS, ZONES, SBDIR,
 * SCHEMA) and the LIVE pages. The stream is written through sift_sw_*; sift_store_immutable_extents / sift_store_referenced_blocks / the
 * orphan sweep see it as a BM25 segment.
 */
#ifndef SIFT_COL_SEG_H
#define SIFT_COL_SEG_H

#include "core/host.h"

#include "col/enc.h"
#include "col/preagg.h"
#include "core/store.h"

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

typedef struct SiftColSegWriter SiftColSegWriter;

/*
 * The column format minor a segment of schema is written as under the setting sift.col_format_minor (or the minor a sift_table needs: its
 * groups with versions are at least SIFT_COL_FORMAT_VERS): a wide schema is at least SIFT_COL_FORMAT_WIDE; the setting SIFT_COL_FORMAT_KINDS
 * gives SIFT_COL_FORMAT_KINDS to a schema with shredded leaves when kinds_exact (every segment the rows come from keeps its kinds, sift_cseg_kinds_exact;
 * rows of a heap or a write buffer do), SIFT_COL_FORMAT_TAGS to the others. A merge that cannot say so passes false.
 */
extern int sift_col_write_minor(const SiftColSchema *schema, int setting, bool kinds_exact);
/* Allocates in mcxt. schema must outlive the writer. */
/* pol[ncols]: the compression policy of each leaf (NULL: plain FOR/PACK chunks), valid until sift_colw_finish; format: the column format minor written (sift_col_write_minor of the setting) */
extern SiftColSegWriter *sift_colw_begin(SiftWriteTarget *wt, uint64 gen, const SiftColSchema *schema, const SiftCcPolicy *pol,
										 int format, MemoryContext mcxt);
/*
 * Rows that are not final (a sift_table group written while its transactions run, docs/COL-TAM.md section 7): a batch whose rows carry a
 * version (sift_cbatch_add_ver) is written with a version section and the entry flag SIFT_SEG_VERS; entry->reserved is the
 * smallest xmin (the largest is in the version section: sum_dl of a directory entry is the owner's, as the metapage totals it). The owner
 * clears the flag in the directory when the rows are final.
 */
/*
 * The pre-aggregates the writer computes for every unit (docs/COL-PREAGG.md): the default spec unless the owner sets one (the reloption
 * `preagg`). Set before the first sift_colw_add.
 */
extern void sift_colw_set_preagg(SiftColSegWriter *w, const SiftCPaggSpec *spec);
/* Encodes and writes the batch as the unit of its superblock; superblocks strictly ascending, batch non-empty */
extern void sift_colw_add(SiftColSegWriter *w, const SiftColBatch *b);
extern uint64 sift_colw_rows(const SiftColSegWriter *w);
/* Memory the writer holds until the segment ends (zones, unit directory, dictionaries): the caller cuts a segment when it passes its budget */
extern Size sift_colw_memory(const SiftColSegWriter *w);
/* Writes DICTS, ZONES, SBDIR, SCHEMA, the all-live LIVE pages and the header; fills the directory entry (kind IMMUTABLE, format, header_blk, sb_lo/hi, ndocs = rows, bytes) */
extern void sift_colw_finish(SiftColSegWriter *w, SiftSegEntry *entry);

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

/*
 * Versions of the rows (the rows of a sift_table group that are not final; frozen = xmin 0). A segment has them while its directory entry
 * says SIFT_SEG_VERS (format 1.4), or, for a group of 3.0.1 (format 1.3), while entry.reserved holds the xid of the one command that wrote all
 * of it (cmin in the header): the reader returns that as constant vectors, so the owner has one case.
 */
extern bool sift_cseg_vers(const SiftColSegment *seg);
/* The range of the xmins (in the order of xids) of a segment that has versions */
extern void sift_cseg_ver_range(SiftColSegment *seg, uint32 *xmin_lo, uint32 *xmin_hi);

typedef struct SiftColVerSum
{
	bool		any;			/* some row of the unit is not frozen */
	bool		frozen_rows;	/* some row is frozen */
	uint32		xmin_lo;		/* over the unfrozen rows (the order of xids) */
	uint32		xmin_hi;
	uint32		cmin_hi;
} SiftColVerSum;

/* What the unit's versions are without decoding them */
extern void sift_cseg_ver_sum(SiftColSegment *seg, uint32 u, SiftColVerSum *sum);
/* The xmin and cmin of every row of unit u in key order (nrows entries each); frozen rows are 0 */
extern void sift_cseg_ver_unit(SiftColSegment *seg, uint32 u, uint32 *xmin, uint32 *cmin);
/* The rows of the page blk inside unit u: the ordinal of the first one and the bit mask (SIFT_OFFMASK_WORDS words) of their offsets; false when the unit has no row on the page */
extern bool sift_cseg_page_rows(SiftColSegment *seg, uint32 u, BlockNumber blk, uint32 *first, uint64 *offmask);
/* The stored segment and unit a unit of a source is read from (a table source: a column group); false for an in-memory unit */
extern bool sift_col_unit_ref(const SiftColUnit *unit, SiftColSegment **seg, uint32 *u);
/* The keys (blk << 16 | off) of the rows of unit u in order, nrows entries */
extern void sift_cseg_unit_keys(SiftColSegment *seg, uint32 u, SiftKey *keys);
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
/* Does the segment keep the kinds of every leaf: a segment of column format SIFT_COL_FORMAT_KINDS, or one of an older format with no NULL slot and no astext value in a leaf filled from a jsonb path. Reads the zone arrays */
extern bool sift_cseg_kinds_exact(SiftColSegment *seg);
extern void sift_cseg_leaf_stats(SiftColSegment *seg, int col, SiftColLeafStats *acc);
/* The zones of the segment's units (INT scalar leaves) into the sample, thinned to keep SIFT_COL_ZONE_SAMPLE units over the segments added; the caller allocates the arrays of the leaves that are sampled (lo[col] not NULL) */
extern void sift_cseg_zone_sample(SiftColSegment *seg, SiftColZones *zs);

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
/* What the sidecar of the segment holds against the spec an index writes now, and whether a rewrite can give it what it lacks (sift_col_refresh); setting: sift.col_format_minor */
extern void sift_cseg_pre_census(SiftColSegment *seg, const SiftCPaggSpec *spec, int setting, SiftCPaggCensus *c);
extern bool sift_cseg_pre_stale(SiftColSegment *seg, const SiftCPaggSpec *spec, int setting, const SiftCPaggCensus *c);

/* Clears the bits (one per unit, nsb bits) of the units whose zone cannot satisfy the test, with the zone kernels */
extern void sift_cseg_prune(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep);

/* Liveness: is any row of the SB's unit dead; the live bits of unit u's rows (nrows bits, padded words) */
extern bool sift_cseg_dirty(SiftColSegment *seg, uint32 u);
extern void sift_cseg_live(SiftColSegment *seg, uint32 u, uint64 *bits);
extern uint64 sift_cseg_live_rows(SiftColSegment *seg);

/* Replaces the contents of dst with the live rows of unit u (dst: a batch of the segment's schema); the rows keep their versions */
extern void sift_cseg_decode_unit(SiftColSegment *seg, uint32 u, SiftColBatch *dst);
/*
 * The same, and a row that is not frozen becomes frozen when freeze(arg, xmin, cmin) says so: the owner knows it committed before every snapshot.
 * Rows are never dropped by it: a row of an aborted writer stays until the owner's VACUUM cleared its liveness (the indexes forgot it first).
 */
typedef bool (*SiftColFreezeFn) (void *arg, uint32 xmin, uint32 cmin);
extern void sift_cseg_decode_unit_freeze(SiftColSegment *seg, uint32 u, SiftColBatch *dst, SiftColFreezeFn freeze, void *arg);

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
 * The source over the column segments of a sift_table (docs/COL-TAM.md section 6; col/scan.c): dir is the caller's directory copy (no snapshot is
 * registered, the caller keeps its own for as long as the source lives), only segments written for schema are read, the visibility is the
 * table's (vis), heap_sbs the superblocks of the table now. No write buffers: the table feeds its delta rows with sift_col_add_batches.
 */
extern SiftColSource *sift_col_open_table(Relation rel, const SiftColSchema *schema, const SiftDirSnapshot *dir, const SiftColVisibility *vis,
										  uint32 heap_sbs, MemoryContext mcxt);

#endif							/* SIFT_COL_SEG_H */
