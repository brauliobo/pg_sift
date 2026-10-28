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
#include "col/runs.h"
#include "core/store.h"

struct SiftCGOrdBuf;

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
/* The rows of the whole index the segment belongs to (a build: the heap's estimate; a merge or promotion: the directory's): the writer signs the dictionaries when those of the index overflow the cache of parsed dictionaries. 0: unknown */
extern void sift_colw_set_index_rows(SiftColSegWriter *w, uint64 rows);
/* The units added from now on are key-cut (SiftColBatch.keyed); after set_preagg, before the first unit */
extern void sift_colw_set_keyed(SiftColSegWriter *w, uint32 aim_rows);
/* Encodes and writes the batch as the unit of its superblock; superblocks strictly ascending, batch non-empty */
extern void sift_colw_add(SiftColSegWriter *w, const SiftColBatch *b);
extern uint64 sift_colw_rows(const SiftColSegWriter *w);
/* Bytes of units written to the stream so far (the rest of the segment, dictionaries, zones and the like, comes at sift_colw_finish): where a merge cuts its output into partitions */
extern uint64 sift_colw_bytes(const SiftColSegWriter *w);
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

/* The bytes of the unit records at the start of the segment stream, and the range of it sift_cseg_unit_head reads of unit u, for sift_sr_ahead */
extern uint64 sift_cseg_units_len(const SiftColSegment *seg);
extern void sift_cseg_unit_head_span(const SiftColSegment *seg, uint32 u, uint64 *off, uint64 *len);

/* The segment's dictionary of a STR leaf (loaded on first use, NULL for other leaves) */
extern SiftCDict *sift_cseg_dict(SiftColSegment *seg, int col);
/*
 * The dictionary a test of a STR leaf is made over: the parsed one when it is at hand, else a dictionary of n codes that find and match search in the stream, without parsing it; else the parsed one. Only n, find and
 * match may be used, and it is valid for the life of the segment.
 */
extern SiftCDict *sift_cseg_dict_search(SiftColSegment *seg, int col, SiftColOp op);
/* sift.col_verify: the signatures stored for the dictionary d of leaf col are the ones its strings make (a no-op without signatures) */
extern void sift_cseg_dict_tsig_check(SiftColSegment *seg, int col, const SiftCDict *d);
/* For each of the dictionary of leaf col: the blocks the signatures leave for a substring test of needle, and all its blocks (false: no signatures) */
extern bool sift_cseg_dict_tsig(SiftColSegment *seg, int col, const char *needle, uint32 len, uint32 *passed, uint32 *total);
/* sift.col_verify: the counts of the leaf's dictionary against the rows of each code that the units hold */
extern void sift_cseg_dict_count_check(SiftColSegment *seg, int col, const uint32 *rows);
/* Whether the dictionary of the leaf holds the counts */
extern bool sift_cseg_dict_counted(SiftColSegment *seg, int col);
/* Whether the dictionary of leaf col has the hash of its strings (DICT_HASHED); the verify of the hash against the strings d of the leaf (sift.col_verify) */
extern bool sift_cseg_dict_hashed(SiftColSegment *seg, int col);
extern void sift_cseg_dict_verify_hash(SiftColSegment *seg, int col, SiftCDict *d);
/* The rows whose string of the leaf passes op (EQ, LT, LE, GT, GE, PREFIX) against c, from the counts a dictionary of the option count_columns holds; false: none stored */
extern bool sift_cseg_dict_count(SiftColSegment *seg, int col, SiftColOp op, const SiftColValue *c, uint64 *rows);
/*
 * The strings of a few codes without parsing the dictionary of the leaf: whether that is cheaper for want more strings (the dictionary is not at
 * hand, is stored plain or for a lookup, and has more than DICT_LAZY_RATIO times as many codes as the strings read so far from this open segment),
 * and the string of a code, read in place into buf (SIFT_COL_DICT_MAX_ENTRY bytes) or, when the dictionary is at hand, from it. Valid until the next call.
 */
#define DICT_LAZY_RATIO			16
extern bool sift_cseg_dict_lazy(SiftColSegment *seg, int col, uint32 want);
extern PGDLLIMPORT bool sift_col_table_lookup;	/* sift.table_dict_lookup: the order of a dictionary of a sift_table is stored too */
extern PGDLLIMPORT bool sift_col_table_key_table;	/* sift.table_key_table: the segments of a sift_table keep a key table of each key leaf */
/*
 * Whether the dictionary of a STR leaf is given to a reader of codes as a view that reads the strings of the codes it is asked for (SiftColDict.get): it is stored in blocks, is
 * not at hand, and the blocks decompressed for such views are fewer than 1/DICT_LAZY_LOADS of its blocks (a view then asks for the whole dictionary, which the cache keeps).
 * Fills n, get and gctx of `view`.
 */
#define DICT_LAZY_LOADS			8
extern bool sift_cseg_dict_view_lazy(SiftColSegment *seg, int col, SiftColDict *view);
extern const char *sift_cseg_dict_get(SiftColSegment *seg, int col, uint32 code, uint32 *len, char *buf);

/* The backend's cache (col/dictcache.c) of the element structure of a list leaf of a unit: the lengths of the lists of its rows, 16 bit (a unit with a longer list is not kept), and the elements they add up to; the running ends are summed again from them */
typedef struct SiftCLens
{
	uint32		nelems;
	const uint16 *counts;
} SiftCLens;
/* the entry, pinned until the memory context arena is reset (the next unit), or NULL */
extern const SiftCLens *sift_cseg_lens_find(SiftColSegment *seg, uint32 unit, int col, MemoryContext arena);
/* a copy of the lengths a scan just decoded for n rows (counts[n]), when the cache has room */
extern void sift_cseg_lens_keep(SiftColSegment *seg, uint32 unit, int col, uint32 n, const uint32 *counts, uint32 nelems);
/* The bytes the dictionary of a STR leaf takes in the stream (0 for other leaves), as stored: smaller than its parsed size when it is compressed */
extern uint64 sift_cseg_dict_stored(SiftColSegment *seg, int col);

/* The zone of leaf col in unit u (the zone arrays of the leaf are read on first use) */
extern void sift_cseg_zone(SiftColSegment *seg, int col, uint32 u, SiftCZone *z);

/* Adds what the zones and the dictionary of the segment say about leaf col to acc (sift_col_leaf_stats_init first). Reads the leaf's zone arrays and four bytes of its dictionary, no chunk */
extern void sift_col_leaf_stats_init(SiftColLeafStats *acc);
/* Does the segment keep the kinds of every leaf: a segment of column format SIFT_COL_FORMAT_KINDS, or one of an older format with no NULL slot and no astext value in a leaf filled from a jsonb path. Reads the zone arrays */
extern bool sift_cseg_kinds_exact(SiftColSegment *seg);
extern void sift_cseg_leaf_stats(SiftColSegment *seg, int col, SiftColLeafStats *acc);
/* The leaf statistics block of the segment, when it has one, is what the zones of its units add up to (corruption error otherwise) */
extern void sift_cseg_verify_lst(SiftColSegment *seg);
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
/* the exception cube and element blob of unit u (column format minor 19; NULL: the unit has none): same formats, over the unit's exception rows; the first varint of each is the exception leaf */
extern const uint8 *sift_cseg_xcube(SiftColSegment *seg, uint32 u, uint32 *len);
extern const uint8 *sift_cseg_xecube(SiftColSegment *seg, uint32 u, uint32 *len);
extern const uint8 *sift_cseg_dim_cube(SiftColSegment *seg, uint32 u, int dim, uint32 *len);
/*
 * Superunits (docs/COL-PREAGG.md section 10): the units in groups of fan consecutive ones, group k = units [k * fan, min((k + 1) * fan, nsb)), with the page
 * masks of the units, the merged sums of every INT leaf and the merged cube of the dims all of them hold. fan 0: the segment has none.
 * sift_cseg_super_agg: false when the leaf has no summary in the group (not INT, an overflow value, one unit). sift_cseg_super_cube: NULL when the group has none,
 * else valid until the next call on seg. sift_cseg_super_pagemask fills 4 words for each of the n units from the first of group k.
 */
extern uint32 sift_cseg_super_fan(SiftColSegment *seg);
extern bool sift_cseg_unit_pagemask(SiftColSegment *seg, uint32 u, uint64 *pm);

/*
 * The superblocks the rows of a segment are in, as ascending disjoint ranges [lo, hi], read once and kept with the segment. A unit cut by superblock gives its own; a unit cut by key or
 * by TID range, the blocks from the first to the last of its summary (the whole heap when the segment has none)
 */
typedef struct SiftCSbSpan
{
	uint32		lo;
	uint32		hi;
} SiftCSbSpan;
extern uint32 sift_cseg_sb_spans(SiftColSegment *seg, const SiftCSbSpan **spans);
/* Is superblock sb inside one of the spans? */
extern bool sift_cseg_sb_in_spans(SiftColSegment *seg, uint32 sb);
extern void sift_cseg_super_pagemask(SiftColSegment *seg, uint32 k, uint32 n, uint64 *pm);
extern bool sift_cseg_super_agg(SiftColSegment *seg, int col, uint32 k, SiftCAgg *a);
extern const uint8 *sift_cseg_super_cube(SiftColSegment *seg, uint32 k, uint32 *len);
/* sift.col_verify: the superunit block says what the units say (page masks, sums, cubes); an error otherwise, nothing when the segment has none */
extern void sift_cseg_verify_super(SiftColSegment *seg);
#ifdef SIFT_TESTING
/* tests: one byte of the stream is read as its value xor the mask (sift_test_seg_flip_at UINT64_MAX: none); the span of the superunit block in the stream */
extern uint64 sift_test_seg_flip_at;
extern uint8 sift_test_seg_flip_mask;
extern bool sift_cseg_test_super_span(SiftColSegment *seg, uint64 *off, uint64 *len);
extern bool sift_cseg_test_run_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used);
extern bool sift_cseg_test_set_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used);
extern bool sift_cseg_test_ktab_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used);
extern bool sift_cseg_test_point_span(SiftColSegment *seg, uint64 *off, uint64 *len, uint8 **used);
#endif
/* the rollup blob of a list in unit u (NULL, len 0: none), for sift_cpagg_rollup_decode */
extern const uint8 *sift_cseg_rollup(SiftColSegment *seg, uint32 u, int list, uint32 *len);
extern uint64 sift_cseg_preagg_bytes(SiftColSegment *seg);
extern uint32 sift_cseg_preagg_units(SiftColSegment *seg);
/* What the sidecar of the segment holds against the spec an index writes now, and whether a rewrite can give it what it lacks (sift_col_refresh); setting: sift.col_format_minor */
/* sample 0: every unit; else that many of them, evenly spread (c->units says how many were counted) */
extern void sift_cseg_pre_census(SiftColSegment *seg, const SiftCPaggSpec *spec, int setting, uint32 sample, SiftCPaggCensus *c);
extern bool sift_cseg_pre_stale(SiftColSegment *seg, const SiftCPaggSpec *spec, int setting, const SiftCPaggCensus *c);

/* Clears the bits (one per unit, nsb bits) of the units whose zone cannot satisfy the test, with the zone kernels */
extern void sift_cseg_prune(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep, uint32 ulo, uint32 uhi, uint64 *setcut, bool sets);
/*
 * The small-set summary of the leaf alone on the units [ulo, uhi): clears the units it shows cannot hold a match, and sets their bits in
 * setcut (nsb bits, NULL: not told), which sift_cseg_prune does for the tests it takes; the units the tests before it kept are those it can count as its own.
 */
/*
 * The rows of the segment, ascending, that the point directory of leaf col lists under a fingerprint of one of the nkeys keys (the keys of the stored
 * values: the code of a dictionary leaf), palloc'd. The rows may hold other values; the caller checks them. False: the leaf has no directory in the
 * segment, or the keys find more than SIFT_CPOINT_MAX_ROWS rows.
 */
extern bool sift_cseg_point_rows(SiftColSegment *seg, int col, const int64 *keys, int nkeys, uint32 **rows, uint32 *nrows);
/* Does the directory list the row (in the segment) under the key */
/* Does the directory list the row under the key? *dense: the key has no list (any row may hold it) */
extern bool sift_cseg_point_has(SiftColSegment *seg, int col, int64 key, uint32 row, bool *dense);
extern uint32 sift_cseg_point_ndense(SiftColSegment *seg, int col);
/*
 * A point directory of heap addresses (column format minor 16; sift_cseg_point_rows and _has refuse its leaves): whether leaf col has one, the addresses it lists,
 * ascending, under a fingerprint of one of the keys (false when there are too many), and whether it lists each non-NULL row (key[i], tid[i]) of a unit, with the
 * rows counted into listed. Every address is checked to lie in a unit of the segment.
 */
extern bool sift_cseg_point_is_tid(SiftColSegment *seg, int col);
extern bool sift_cseg_point_tids(SiftColSegment *seg, int col, const int64 *keys, int nkeys, const uint64 *keep, SiftKey **tids, uint32 *ntids);
/*
 * The addresses of the same lookup read as far as the caller goes, for a scan that may stop early: a cursor over each list the keys name (at most 8), merged, in
 * pieces. NULL when the keys name more lists, more rows than a point lookup takes, or the leaf has no addresses (sift_cseg_point_tids answers or refuses then).
 * Every address ascends, once; the caller checks that it lies in a unit.
 */
typedef struct SiftPointStream SiftPointStream;
/* The places [from, to) of the byte order of leaf col's dictionary that pass op against c (false: the dictionary is not read in place), and a stream, or the codes, of those places */
extern bool sift_cseg_dict_places(SiftColSegment *seg, int col, SiftColOp op, const SiftColValue *c, uint32 *from, uint32 *to);
extern SiftPointStream *sift_cseg_point_stream_open_places(SiftColSegment *seg, int col, uint32 from, uint32 to);
extern void sift_cseg_dict_codes(SiftColSegment *seg, int col, uint32 from, uint32 to, int64 *keys);
extern SiftPointStream *sift_cseg_point_stream_open(SiftColSegment *seg, int col, const int64 *keys, int nkeys);
extern uint32 sift_cseg_point_stream_next(SiftPointStream *s, SiftKey *out, uint32 max);
extern bool sift_cseg_point_check_tids(SiftColSegment *seg, int col, const int64 *key, const uint64 *nulls, const SiftKey *tid, uint32 n, uint64 *listed);
/* The rows the point directory of leaf col lists (0: it has none), and the check of the blocks of every leaf: slots and lists in range, ascending, as many keys, rows and words as the records say */
extern uint32 sift_cseg_point_count(SiftColSegment *seg, int col);
extern uint32 sift_cseg_point_nkeys(SiftColSegment *seg, int col);
extern void sift_cseg_verify_points(SiftColSegment *seg);
/*
 * The global order of leaf col (col/gorder.h, column format minor 21): false when the segment has none. entries: its rows, values: those with a value (the first ones in
 * the order of (key, heap address), then the rows without a value by address). _chunk: the entries [c * SIFT_CGORD_ROWS, + the count it returns) as keys and heap
 * addresses (SIFT_CGORD_CAP slots each; the keys of a NULL row are 0). _find: the place of the first value at or after key (past it when after), the number of values when none.
 */
extern uint32 sift_cseg_gorder_collation(SiftColSegment *seg, int col);	/* the collation of the strings of a text block, 0 for any other */
extern const char *sift_cseg_gorder_str(SiftColSegment *seg, int col, uint32 rank, uint32 *len);	/* the string of a key of a text block, valid until the next call for the leaf */
extern int	sift_cseg_gorder_slot(SiftColSegment *seg, int col, bool grouped);	/* the slot of the block of a leaf, sorted by group or plain, to pass as col to the others; -1: none */
extern bool sift_cseg_gorder(SiftColSegment *seg, int col, uint32 *entries, uint32 *values);
extern uint32 sift_cseg_gorder_chunk(SiftColSegment *seg, int col, uint32 c, int64 *key, uint64 *addr);
extern void sift_cseg_gorder_fill(SiftColSegment *seg, int col, uint32 p, uint32 w, struct SiftCGOrdBuf *buf);	/* _head for the chunk that holds entry p, into buf */
extern uint32 sift_cseg_gorder_head(SiftColSegment *seg, int col, uint32 c, uint32 w, int64 *key, uint64 *addr);	/* _chunk for the first w entries of the chunk only */
extern int	sift_cseg_gorder_group(SiftColSegment *seg, int col);	/* the leaf the block is grouped by, -1 when it is not */
extern void sift_cseg_gorder_range(SiftColSegment *seg, int col, int64 group, uint32 *vs, uint32 *ve, uint32 *ns, uint32 *ne);	/* the entries of a group (all of a block that is not grouped) */
extern uint32 sift_cseg_gorder_count(SiftColSegment *seg, int col, int64 lo, int64 hi);
extern bool sift_cseg_gorder_tids(SiftColSegment *seg, int col, int64 lo, int64 hi, uint32 limit, SiftKey **tids, uint32 *ntids);
extern uint32 sift_cseg_gorder_seek(SiftColSegment *seg, int col, uint32 vs, uint32 ve, int64 key, bool after, struct SiftCGOrdBuf *keep);	/* the first entry of [vs, ve) with key >= key (after: > key); ve when none; the chunk it ends in is left in keep */
extern void sift_cseg_prune_sets(SiftColSegment *seg, int col, const SiftCTest *t, uint64 *keep, uint32 ulo, uint32 uhi, uint64 *setcut);
/* The bytes the summaries of the segment take */
extern uint64 sift_cseg_set_bytes(SiftColSegment *seg);
/* The bytes the sorted runs of the segment take */
extern uint64 sift_cseg_run_bytes(SiftColSegment *seg);
extern uint64 sift_cseg_hll_bytes(SiftColSegment *seg);
/* The histogram of the key leaf col over the whole segment, from its key table (col/ktab.h), kept with the segment; NULL when the leaf has none */
struct SiftCHistDim;
extern const struct SiftCHistDim *sift_cseg_ktab(SiftColSegment *seg, int col);
/* The stored sketch of leaf col in unit u (len bytes, well formed: sift_hll_check), NULL when the unit has none; valid until the next call */
extern const uint8 *sift_cseg_hll(SiftColSegment *seg, int col, uint32 u, uint32 *len);
/* HLL_LIST | HLL_DENSE: the forms of sketch some unit of the leaf has (0: none) */
extern uint32 sift_cseg_hll_kinds(SiftColSegment *seg, int col);
/* The kinds (RUNS_ORD, RUNS_IDENT) the record of the leaf says some unit has; 0 when the leaf has no run */
extern uint32 sift_cseg_run_kinds(SiftColSegment *seg, int col);
/* the kinds (SETS_EXACT, SETS_BLOOM, SETS_PRES) the record of leaf col in the small-set summaries lists */
extern uint32 sift_cseg_set_kinds(SiftColSegment *seg, int col);
/*
 * The sorted run of leaf col in unit u (col/runs.h): its header in r and where its bytes start in at; false when the unit has none. Then the entries
 * [pos, pos + cnt) of it as the ordinals of rows (sift_cseg_run_read), each checked to be a row of the unit.
 */
extern bool sift_cseg_run(SiftColSegment *seg, int col, uint32 u, SiftCRun *r, uint64 *at);
extern void sift_cseg_run_bytes_read(SiftColSegment *seg, uint64 at, const SiftCRun *r, uint32 pos, uint32 cnt, uint8 *dst);
extern void sift_cseg_run_read(SiftColSegment *seg, uint64 at, const SiftCRun *r, uint32 pos, uint32 cnt, uint32 *out);
/* The head of the run (r->heads > 0): sift_crun_head_counts entries, the low end first, then the high end from the last of the run */
extern void sift_cseg_run_heads(SiftColSegment *seg, uint64 at, const SiftCRun *r, SiftCHeadEnt *out);
/*
 * The page mask and the run head of leaf col in unit u, the two reads of a head bypass (col/scan_unit.c), kept by the backend for the next query (col/dictcache.c:
 * a segment never changes). False when the segment has no page masks; else has: the unit has a head, nlo + nhi entries of ent (sift_cseg_run_heads).
 */
typedef struct SiftCSbHead
{
	bool		has;
	uint32		nlo;
	uint32		nhi;
	uint64		pm[SIFT_PAGEMASK_WORDS];
	SiftCHeadEnt ent[2 * SIFT_CRUN_HEAD_MAX];
} SiftCSbHead;

extern bool sift_cseg_sbhead(SiftColSegment *seg, uint32 u, int col, SiftCSbHead *out);
/* The whole stored run with its head, sift_crun_total(r) bytes (the verifier) */
extern void sift_cseg_run_blob(SiftColSegment *seg, uint64 at, const SiftCRun *r, uint8 *dst);
/* The small-set summary of leaf col in unit u for sift_cset_open (NULL: the unit has none), valid until the next call on seg */
extern const uint8 *sift_cseg_set(SiftColSegment *seg, int col, uint32 u, uint32 *len);
/*
 * The key presence of leaf col in unit u: a bitmap (the key of bit 0 divided by 64, the words and their number, valid until the next call on seg) or a list
 * (nkeys > 0: the ascending keys, in memory of the caller's context). False when the unit has neither.
 */
typedef struct SiftCPresence
{
	int64		wbase;
	uint64		nwords;
	const uint8 *words;
	uint32		nkeys;
	int64	   *keys;
} SiftCPresence;
extern bool sift_cseg_presence(SiftColSegment *seg, int col, uint32 u, SiftCPresence *p);

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

#ifdef SIFT_TESTING
/* The counts of sift_test_cube_damage (col/test.c) over the cube blobs it was given */
typedef struct SiftTestDamage
{
	int64		blobs,
				intact,
				minmax,
				calendar,
				cut,
				cut_accepted,
				damaged,
				accepted,
				unsound,
				census_cut_odd,
				census_odd;
} SiftTestDamage;

extern void sift_test_cube_damage(const SiftColSchema *sc, const SiftCPaggSpec *spec, const uint8 *blob, uint32 len, const uint8 *eblob, uint32 elen,
								  MemoryContext cx, SiftTestDamage *r);
#endif

/* Are the segment's units cut by a declared key? Not told by the minor: a later minor (a segment may have several of the optional records) is no more keyed than an earlier one; the first keyed segments had no flag and are minor 17 */
static inline bool
sift_cseg_keyed(const SiftSegEntry *e)
{
	return (e->flags & SIFT_SEG_KEYED) != 0 || e->format == SIFT_COL_FORMAT_KEYED;
}

#endif							/* SIFT_COL_SEG_H */
