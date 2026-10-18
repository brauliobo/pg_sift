/*
 * sift_col_enc.h - L1 of the column engine (docs/COL.md sections 1, 2.1-2.4, 2.6, 12): the schema, dictionaries, batches (the decoded rows
 * of one superblock), the unit record and column chunk encodings, chunk filters and zone maps. No pages, no heap, no WAL: values and bytes,
 * so a table access method can reuse it. Portable (sift_host.h only).
 */
#ifndef SIFT_COL_ENC_H
#define SIFT_COL_ENC_H

#include "sift_host.h"

#include "sift_col.h"
#include "sift_col_kern.h"
#include "sift_colcodec.h"

/* ---------- schema ---------- */

typedef struct SiftColSchema
{
	int			ncols;
	int			ngroups;
	SiftColDef *cols;			/* [ncols], paths copied */
	uint32		hash;			/* of ids, kinds, stores, type OIDs, attnos, parents, paths (not flags) */
} SiftColSchema;

/*
 * Copies and validates the leaf list: ids 0..n-1 in order, a child's parent is an earlier GROUP leaf, groups are top level (parent -1),
 * 1 <= n <= SIFT_COL_MAX_COLS. ERRCODE_INVALID_PARAMETER_VALUE otherwise. Allocates in mcxt.
 */
extern SiftColSchema *sift_cschema_create(MemoryContext mcxt, const SiftColDef *defs, int n);

/* The SCHEMA section of a segment (docs/COL.md 2.1): magic 'COLS', ncols, hash, per leaf a fixed part and the path */
extern void sift_cschema_serialize(const SiftColSchema *s, StringInfo out);
/* NULL if the bytes are not a schema (the caller raises ERRCODE_INDEX_CORRUPTED) */
extern SiftColSchema *sift_cschema_parse(MemoryContext mcxt, const void *data, Size len);

/* ---------- dictionaries (code = first-seen order) ---------- */

typedef struct SiftCDict
{
	MemoryContext mcxt;
	uint32		n;
	uint32		cap;
	uint32	   *off;			/* n + 1 */
	char	   *bytes;
	Size		nbytes;
	Size		bytescap;
	uint32	   *slots;			/* open addressing table of code + 1; built lazily by intern / find */
	uint32		nslots;
} SiftCDict;

extern SiftCDict *sift_cdict_create(MemoryContext mcxt);
/* The code of the value, adding it when new; len <= SIFT_COL_MAX_STR (ERRCODE_PROGRAM_LIMIT_EXCEEDED otherwise) */
extern uint32 sift_cdict_intern(SiftCDict *d, const char *s, uint32 len);
/* The code, or -1 */
extern int64 sift_cdict_find(SiftCDict *d, const char *s, uint32 len);
/* The bytes and length of a code < d->n */
extern const char *sift_cdict_str(const SiftCDict *d, uint32 code, uint32 *len);
/* DICTS section entry: u32 n, u32 off[n + 1], bytes */
extern void sift_cdict_serialize(const SiftCDict *d, StringInfo out);
/* Copy of a serialized dictionary; NULL when malformed. *used = bytes consumed */
extern SiftCDict *sift_cdict_parse(MemoryContext mcxt, const void *data, Size len, Size *used);
extern void sift_cdict_view(const SiftCDict *d, uint64 id, SiftColDict *view);

/* ---------- batches: the decoded rows of one SB, keys ascending ---------- */

/* Values of one leaf: key-domain integers (INT as is, F4/F8 order keys, STR codes into the batch's dictionary of the leaf) */
typedef struct SiftCVals
{
	uint32		n;				/* values: rows, or the elements of all rows for a child leaf */
	uint32		cap;
	int64	   *key;
	uint64	   *nulls;			/* NULL until the first NULL; bit set = NULL; capacity cap bits */
	uint64	   *ovf;			/* NULL until the first overflow value; bit set = overflow (also set in nulls) */
	uint32	   *eoff;			/* GROUP leaves: element offset of each row, nrows + 1 entries */
	/* VAR leaves: key[i] is the byte length of value i (0 for NULL), the bytes of all values follow each other in vb, voff[i] the start of value i */
	uint32	   *voff;			/* n + 1 entries once a value was added */
	char	   *vb;
	Size		vcap;
} SiftCVals;

typedef struct SiftColBatch
{
	MemoryContext mcxt;
	const SiftColSchema *schema;
	uint32		sb;
	uint32		nrows;
	uint32		rowcap;
	SiftKey    *keys;			/* ascending, all in superblock sb */
	SiftCVals  *col;			/* [ncols] */
	SiftCDict **dict;			/* [ncols]: batch dictionary of each STR leaf, NULL for the others */
} SiftColBatch;

extern SiftColBatch *sift_cbatch_create(MemoryContext mcxt, const SiftColSchema *schema);
/* Empties the batch (memory kept) for superblock sb; the dictionaries restart */
extern void sift_cbatch_reset(SiftColBatch *b, uint32 sb);
/* Appends a row; its TID's superblock must be b->sb and its key above the last one's. STR values are interned (ERROR over 255 bytes). */
extern void sift_cbatch_add(SiftColBatch *b, const SiftColRow *row);
/* Appends row r of src (same schema) re-interning its strings; same key rules */
extern void sift_cbatch_add_from(SiftColBatch *dst, const SiftColBatch *src, uint32 r);
/* Appends the rows of the sources merged by ascending key (each source ascending, same sb); on a repeated key the earlier source wins */
extern void sift_cbatch_merge(SiftColBatch *dst, SiftColBatch *const *src, int nsrc);
/*
 * Fills row with row r of the batch; strings point into the dictionaries. A zeroed row is set up on the first call (its arrays are
 * palloc'd in the batch's context; elem has ncols + 1 entries, the last is private) and reused by the next ones.
 */
extern void sift_cbatch_row(const SiftColBatch *b, uint32 r, SiftColRow *row);
extern void sift_cbatch_row_free(SiftColRow *row);
/* The value of a leaf's slot as a SiftColValue (strings point into the dictionary) */
extern void sift_cbatch_value(const SiftColBatch *b, int col, uint32 slot, SiftColValue *v);

/* ---------- zones ---------- */

#define SIFT_COLZ_NOSUM		0x1		/* the integer sum overflowed int64 */
#define SIFT_COLZ_NAN		0x2		/* a float chunk holds a NaN */
#define SIFT_COLZ_CONST		0x4		/* every non-NULL value equal */
#define SIFT_COLZ_OVERFLOW	0x8		/* some value overflowed (docs/COL.md 6): the zone does not describe it */

typedef struct SiftCZone
{
	int64		min;
	int64		max;
	int64		sum_i;
	double		sum_f;
	uint32		nnull;
	uint32		nvals;
	uint32		flags;
	uint32		pad;
	uint64		set[4];			/* STR leaves: bit (code & 255) set for every code present */
} SiftCZone;

/* ---------- chunks ---------- */

#define SIFT_COLC_NULLS		0x1
#define SIFT_COLC_OVF		0x2		/* an overflow bitmap follows the null bitmap */
#define SIFT_COLENC_CONST	0
#define SIFT_COLENC_FOR		1
#define SIFT_COLENC_PACK	2
#define SIFT_COLENC_RLE		3		/* reserved */
#define SIFT_COLENC_CC		4		/* the payload is one blob of the chunk codec (sift_colcodec.h, docs/COL-CODEC.md): bits and base are 0 */
#define SIFT_COLENC_STRS	5		/* VAR leaves: the payload is the codec's list of n strings (the NULL slots are empty strings); bits and base are 0 */

typedef struct SiftCChunk
{
	uint8		enc;
	uint8		bits;
	uint8		flags;
	uint32		n;
	int64		base;
	const uint64 *nulls;		/* NULL without SIFT_COLC_NULLS */
	const uint64 *ovf;			/* NULL without SIFT_COLC_OVF */
	const uint8 *payload;
	uint32		paylen;
} SiftCChunk;

/*
 * Views a chunk (docs/COL.md 2.2); false when its bytes are inconsistent. p must be 8-byte aligned and, like every array a kernel reads,
 * have SIFT_COL_PAD readable bytes past len (the unit record's reader provides them).
 */
extern bool sift_cchunk_parse(const uint8 *p, uint32 len, SiftCChunk *c);

/*
 * Decoding to the key domain: dst[0..n) (NULL slots 0). The _sel form decodes only the positions idx[0..m) into dst[0..m). dst needs
 * n + SIFT_COL_PAD/8 slots.
 */
extern void sift_cchunk_decode(const SiftCChunk *c, int64 *dst);
/* VAR chunks (enc STRS): the value offsets (n + 1) and the bytes of all values, palloc'd in mcxt; the values are exact, the bytes at NULL slots are none */
extern void sift_cchunk_strs(const SiftCChunk *c, MemoryContext mcxt, uint32 **off, char **bytes);
extern void sift_cchunk_decode_sel(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst);

/*
 * Tests: a predicate of a leaf made executable against one segment's dictionary (a STR constant becomes a code or a LUT; a constant
 * absent from the dictionary makes EQ / IN / PREFIX match nothing and NE / NOTIN everything non-NULL).
 */
typedef enum SiftCTestKind
{
	SIFT_CT_NONE,				/* no row matches */
	SIFT_CT_NONNULL,			/* every non-NULL row */
	SIFT_CT_RANGE,				/* lo <= key <= hi (signed, key domain) */
	SIFT_CT_NRANGE,				/* non-NULL and not in [lo, hi] */
	SIFT_CT_LUT,				/* STR: lut bit of the code (lutbits >= the dictionary size) */
	SIFT_CT_ISNULL,
	SIFT_CT_NOTNULL,
	SIFT_CT_SET,				/* INT/F4/F8 IN with two or more distinct constants: key is one of set[] (ascending) */
	SIFT_CT_NSET				/* non-NULL and none of set[] */
} SiftCTestKind;

typedef struct SiftCTest
{
	SiftCTestKind kind;
	int64		lo;				/* RANGE / NRANGE bounds; for LUT and SET the smallest and largest key that can match */
	int64		hi;
	const int64 *set;			/* palloc'd by sift_ctest_make */
	uint32		nset;
	const uint64 *lut;			/* palloc'd by sift_ctest_make */
	uint32		lutbits;
	uint64		fold[4];		/* LUT / STR EQ: the bits (code & 255) the test can match, to compare with a zone's set */
	bool		fold_valid;
} SiftCTest;

/* dict: the segment's dictionary of the leaf (STR; may be NULL for the other stores). Allocates the LUT in CurrentMemoryContext. */
extern void sift_ctest_make(const SiftColDef *def, SiftCDict *dict, const SiftColPred *p, SiftCTest *t);

/* Can a unit with this zone hold a matching row (conservative) */
extern bool sift_ctest_zone(const SiftCTest *t, const SiftCZone *z);

/* bits &= rows of the chunk that satisfy the test (bits has the padded words of c->n rows) */
extern void sift_ctest_run(const SiftCTest *t, const SiftCChunk *c, uint64 *bits);

/* ---------- units (docs/COL.md 2.1) ---------- */

/*
 * Encodes the batch as one unit record appended to out (the record starts 8-byte aligned in out: padding is added first, *start gets its offset):
 * the row directory then one chunk per leaf (a leaf all of whose values are NULL still gets a CONST chunk with the NULL bitmap). zones[ncols]
 * receives the zones (may be NULL). dictmap[col] (NULL array or NULL entry: identity) translates the batch's STR codes into the segment's.
 * SIFT_COL_PAD zero bytes stay readable after the record in out's buffer (not counted in out->len), so it can be parsed and filtered in place.
 * pol[ncols] (NULL: none) is the compression policy of each leaf: a chunk that is not constant is then a blob of the chunk codec
 * (SIFT_COLENC_CC); without it the chunk is the plain FOR or PACK form (in-memory units that are only scanned).
 */
extern void sift_cunit_encode(const SiftColBatch *b, uint32 *const *dictmap, const SiftCcPolicy *pol, StringInfo out, Size *start,
							  SiftCZone *zones);

typedef struct SiftCUnitView
{
	const uint8 *rec;
	uint32		len;
	const SiftColSchema *schema;
	uint32		nrows;
	uint32		npages;
	uint64		pagemask[SIFT_PAGEMASK_WORDS];
	const uint32 *rowstart;		/* npages + 1 (unaligned access is avoided by the parser copying when needed) */
	const uint16 *maxoff;
	const uint8 *sparse;
	const uint32 *chunk_off;
	const uint32 *chunk_len;
} SiftCUnitView;

/* false when inconsistent (the caller raises ERRCODE_INDEX_CORRUPTED); rec must stay valid and be 8-byte aligned */
extern bool sift_cunit_parse(const uint8 *rec, uint32 len, const SiftColSchema *schema, SiftCUnitView *v);

/*
 * The same for a unit whose record is read lazily: only its first avail bytes (the row directory and the chunk table, which a bound of
 * SIFT_COL_UNIT_HEAD covers) are present, len is the full length; the chunks are not looked at. The caller makes the bytes of a chunk
 * present (v->chunk_off[col], v->chunk_len[col]) before sift_cunit_chunk(col).
 */
#define SIFT_COL_UNIT_HEAD		(24 * 1024)
extern bool sift_cunit_parse_head(const uint8 *rec, uint32 avail, uint32 len, const SiftColSchema *schema, SiftCUnitView *v);

/* The chunk of leaf col; false when the unit has none (all values NULL) */
extern bool sift_cunit_chunk(const SiftCUnitView *v, int col, SiftCChunk *c);

/* Keys (blk << 16 | off) of rows [0, nrows) of the unit for superblock sb */
extern void sift_cunit_keys(const SiftCUnitView *v, uint32 sb, SiftKey *keys);
/* Pages: index p in 0..npages-1 -> heap block sb * 256 + page, its row range and the 512-bit mask of its offsets */
extern BlockNumber sift_cunit_page_block(const SiftCUnitView *v, uint32 sb, uint32 p);
extern void sift_cunit_page_offmask(const SiftCUnitView *v, uint32 p, uint64 *offmask);
/* Page index of a row (binary search of rowstart) */
extern uint32 sift_cunit_row_page(const SiftCUnitView *v, uint32 row);

/*
 * Appends the live rows of the unit (live: bit vector over its rows, NULL = all) to dst, whose schema is the unit's and whose sb is sb.
 * dicts[col]: the segment's dictionary of each STR leaf (read), the rows' strings are re-interned in dst's own dictionaries.
 */
extern void sift_cunit_decode(const SiftCUnitView *v, uint32 sb, SiftCDict *const *dicts, const uint64 *live, SiftColBatch *dst);

/* The zone recomputed from a chunk (for verification: C4). set is meaningful for STR leaves only. */
extern void sift_cchunk_zone(const SiftCChunk *c, SiftColStore store, SiftCZone *z);

#endif							/* SIFT_COL_ENC_H */
