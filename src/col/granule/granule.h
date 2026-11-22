/*
 * col/granule.h - the one unit of a part of format major 2: a granule of up to 8192 rows, sorted by (key, _tid), with the _tid as an ordinary column chunk (chunk 0).
 *
 * A granule record is a 16-byte header, the chunk table (one SiftChunkRef for _tid and one for each column) and the 8-byte aligned chunks: a scan that needs no TID never
 * reads chunk 0, and a scan of some columns reads the table and those chunks only. A nullable chunk starts with a bit per row (1: not null) unless every row is, or none.
 * The directory of a part (SEC_GRANULE_DIR) holds one SiftGranuleDesc for each granule: where it is, its checksum, the inserter bounds that decide its visibility, and the
 * range of its TIDs.
 */
#ifndef SIFT_GRANULE_H
#define SIFT_GRANULE_H

#include "core/host.h"

#include "agg/vec.h"
#include "col/f2/part_format.h"

#define SIFT_GRANULE_ROWS		8192
#define SIFT_GRANULE_MAGIC		0x47524E32U		/* "GRN2" */
#define SIFT_COL_TID_IDX		0

#define SIFT_GDESC_ALL_FROZEN	(UINT32_C(1) << 0)
#define SIFT_GDESC_HAS_PREAGG	(UINT32_C(1) << 1)
#define SIFT_GDESC_HAS_NULLS	(UINT32_C(1) << 2)
#define SIFT_GDESC_SINGLE_XACT	(UINT32_C(1) << 4)	/* every row was inserted by one transaction (min_xid) at one command (single_cmin): the granule keeps no bytes in SEC_VERS */

typedef struct SiftGranuleDesc
{
	uint64		data_off;
	uint32		data_len;
	uint16		nrows;
	uint16		flags;
	uint32		crc32c;			/* of the granule header and its chunk table */
	uint32		single_cmin;	/* SIFT_GDESC_SINGLE_XACT: the command of every row; else 0 */
	FullTransactionId min_xid;
	FullTransactionId max_xid;
	uint64		tid_min;
	uint64		tid_max;
	uint64		row_base;
} SiftGranuleDesc;

StaticAssertDecl(sizeof(SiftGranuleDesc) == 64, "SiftGranuleDesc must be 64 bytes");

/*
 * How hard the writer works on a granule: a slab batch and a flushed L0 micro-part (level 0) keep to the lightweight codecs (constant, run lengths, delta, frame of reference, a
 * dictionary of at most 256 strings held on the stack, LZ4 only for a large body that a probe shows shrinks), a part of level 1 and up and every bulk build run the whole adaptive selector
 */
typedef enum SiftCompressTier
{
	SIFT_TIER_HOT_SLAB = 0,
	SIFT_TIER_HOT_L0 = 1,
	SIFT_TIER_WARM_L1 = 2,		/* parts of level 1: every codec, the codec of a chunk chosen by what it saves */
	SIFT_TIER_COLD_L2 = 3		/* parts of level 2: as warm, and the chunks that a codec wraps are wrapped in zstd, written once and read rarely */
} SiftCompressTier;

#define SIFT_TIER_IS_COLD(t)	((t) >= SIFT_TIER_WARM_L1)

/*
 * Every integer-class body is a self-describing blob of the column codec (col/codec.h: the vertical 32 x 32 bit-packing, whatever the CPU), except the constant (8 bytes) and the bits of a
 * boolean; the enc names what the cascade chose. The bits above SIFT_ENC_MASK in SiftChunkRef.enc modify the encoding.
 */
typedef enum SiftChunkEnc
{
	SIFT_CENC_TID_DELTA = 1,	/* retired: the _tid of every granule is SIFT_CENC_TID_BLOCK */
	SIFT_CENC_INT_FOR = 2,
	SIFT_CENC_INT_DELTA = 3,
	SIFT_CENC_INT_CONST = 4,
	SIFT_CENC_DICT_CODE = 5,	/* a dictionary (its own, or the part's with SIFT_ENC_PART_DICT) and the codes as a cascade */
	SIFT_CENC_STR_INLINE = 6,	/* the strings as they are, behind their offsets (the lightweight tiers) */
	SIFT_CENC_FLOAT_RAW = 7,
	SIFT_CENC_LIST_LEN = 8,		/* the lengths of arrays: 64-row blocks with the first element of each (SiftArrayLayout) */
	SIFT_CENC_ALL_NULL = 9,
	SIFT_CENC_BOOL_BITS = 10,	/* a bit per value */
	SIFT_CENC_NUM64 = 11,		/* numeric(p <= 18, s): the scale and the cascade of the scaled int64 */
	SIFT_CENC_NUM128 = 12,		/* numeric(p <= 38, s): SiftI128 as it is, or its two halves as cascades, or a dictionary */
	SIFT_CENC_UUID16 = 13,		/* the 16 bytes of a uuid */
	SIFT_CENC_INTERVAL16 = 14,	/* SiftInterval16 as it is, or in halves */
	SIFT_CENC_RLE = 15,			/* runs */
	SIFT_CENC_ALP = 16,			/* doubles as a cascade of the codec: decimals as scaled integers with the others patched in */
	SIFT_CENC_SCALE = 17,		/* retired */
	SIFT_CENC_STR_VIEW = 18,	/* SiftStrView of every value, the long ones in a heap behind them */
	SIFT_CENC_TID_BLOCK = 19,	/* _tid: a header (SiftTidBlockHeader) that names how the heap blocks are held (steps, a dictionary, frames of 128 rows), the blocks, the offsets of the rows as a cascade */
	SIFT_CENC_FLOAT_BSS = 20,	/* doubles with the bytes of each value split into planes, which LZ4 or zstd wraps */
	SIFT_CENC_STR_FSST = 21,	/* strings in a table of symbols of 1 to 8 bytes, each string apart */
	SIFT_CENC_STR_BLOCK = 22,	/* strings as the codec's list: the lengths as a cascade and the bytes wrapped when that pays */
	SIFT_CENC_FIXED16 = 23,		/* 16-byte values (SiftI128, SiftInterval16) as the cascades of their two halves */
	SIFT_CENC_KEY_PRESENT = 24,	/* a leaf that holds no value, only that the key of a jsonb document is there: its validity is the answer (a value reads as 1) */
	SIFT_CENC_ALP_RD = 25,		/* doubles split into a left part of 16 bits or fewer (at most 8 distinct, as codes) and the right bits, packed at their exact width */
	SIFT_CENC_STR_PROMO = 26	/* the text of an astext leaf that is, but for a few rows, one canonical type (SiftColPromo): the values as that type's chunk, the other rows as text */
} SiftChunkEnc;

/*
 * The type the text of most rows of an astext leaf is in, decided for the part: a boolean, an integer, a decimal of a fixed scale, a date or a timestamp that the text spells in one canonical
 * way, so that writing the value back gives the same bytes. SiftColPromo heads the body of a SIFT_CENC_STR_PROMO chunk, and the part's SEC_SCHEMA lists those of its columns.
 */
typedef enum SiftPromoKind
{
	SIFT_PROMO_NONE = 0,
	SIFT_PROMO_BOOL = 1,		/* true, false */
	SIFT_PROMO_INT64 = 2,		/* an integer of at most 18 digits */
	SIFT_PROMO_NUM64 = 3,		/* a decimal of exactly `param` fractional digits (1..9), at most 18 digits in all */
	SIFT_PROMO_DATE = 5,		/* YYYY-MM-DD, held as the days since 1970-01-01 */
	SIFT_PROMO_TIMESTAMPTZ = 6,	/* YYYY-MM-DD[T ]HH:MM:SS[.fff[fff]](Z|+00:00), held as microseconds since 1970-01-01; `param` fractional digits (0, 3 or 6) */
	SIFT_PROMO_FLOAT64 = 7,		/* a number of any digits that is the shortest text of the double it is, held as that double (as an integer that orders as it does): the decimals NUM64 cannot hold */
	SIFT_PROMO_PREFIX_INT = 8	/* a short ASCII prefix and a decimal number, the number held as an integer and the prefix said once in the chunk (col/granule/promo_prefix.c) */
} SiftPromoKind;

#define SIFT_PROMO_FLAG_TS_Z_SUFFIX	UINT8_C(1 << 3)	/* the timestamp ends in Z (else +00:00) */
#define SIFT_PROMO_FLAG_TS_T_SEP	UINT8_C(1 << 4)	/* the date and the time are apart by T (else a space) */

typedef struct SiftColPromo
{
	uint16		col;			/* the column of the part (0 in a chunk) */
	uint8		kind;			/* SiftPromoKind */
	uint8		flags;			/* SIFT_PROMO_FLAG_* */
	uint8		param;			/* NUM64: the scale; TIMESTAMPTZ: the fractional digits */
	uint8		orig_tag;		/* SIFT_COLTAG_* the rows that are of the type carry (a number, a boolean, a string) */
	uint16		reserved;
} SiftColPromo;

StaticAssertDecl(sizeof(SiftColPromo) == 8, "SiftColPromo must be 8 bytes");

/* The high byte of the second 16-bit word of SEC_SCHEMA (the low byte is the number of sort columns): what follows the sort flags */
#define SIFT_SCHEMA_EXT_PROMO		UINT16_C(0x0100)	/* uint16 npromo, padded to 8 bytes, then SiftColPromo[npromo] ascending by col */
#define SIFT_SCHEMA_EXT_ATTNUM		UINT16_C(0x0400)	/* padded to 8 bytes, then SiftPartColIdentity[ncols]: the attribute each column of the part holds (a sift_table part) */
#define SIFT_SCHEMA_EXT_DEFAULTS	UINT16_C(0x0800)	/* padded to 8 bytes, then uint32 stamp and 4 zero bytes: the stamp of SEC_SCHEMA_DEFAULTS, which the part carries (with SIFT_SCHEMA_EXT_ATTNUM only) */

/* The attribute a column of the part holds and how it is stored: a part is read through the identities it was written with, whatever the attributes are by now */
typedef struct SiftPartColIdentity
{
	int16		attnum;
	uint8		phys_kind;
	uint8		phys_hint;
	uint8		phys_scale;
	uint8		flags;
	uint16		path_hash16;
} SiftPartColIdentity;

StaticAssertDecl(sizeof(SiftPartColIdentity) == 8, "SiftPartColIdentity must be 8 bytes");

#define SIFT_ENC_MASK			UINT16_C(0x00FF)
#define SIFT_ENC_NULL_SPARSE	UINT16_C(0x0100)	/* the rows without a value are a list of 16-bit indexes, not a bit per row */
#define SIFT_ENC_PART_DICT		UINT16_C(0x0200)	/* DICT_CODE: the strings are the dictionary of this column in SEC_DICTS of the part */
#define SIFT_ENC_NONNULL_SPARSE	UINT16_C(0x0400)	/* the rows with a value are a list of 16-bit indexes, not a bit per row: a column that is null but for a few rows */
/*
 * The leaf of a key of a jsonb document (SiftColIn.key_leaf) says on which rows jsonb_exists(document, key) holds, which a null leaf does not: the key may be there with a JSON null, with a value the
 * leaf could not hold, or the document may be an array or a string that holds it. Which of the four is in the enc, and the bytes that tell the rows follow the exceptions of the chunk.
 */
#define SIFT_ENC_KEY_ALL_PRESENT UINT16_C(0x0800)	/* the key is on every row: no byte */
#define SIFT_ENC_KEY_ALL_ABSENT	UINT16_C(0x1000)	/* the key is on no row: no byte */
#define SIFT_ENC_KEY_EQ_VALID	UINT16_C(0x2000)	/* the key is on the rows that have a value, and on no other: no byte */
#define SIFT_ENC_KEY_HAS_MASK	UINT16_C(0x4000)	/* a SiftKeyPresentHdr and the rows of the key follow the exceptions */
#define SIFT_ENC_KEY_ANY		(SIFT_ENC_KEY_ALL_PRESENT | SIFT_ENC_KEY_ALL_ABSENT | SIFT_ENC_KEY_EQ_VALID | SIFT_ENC_KEY_HAS_MASK)

typedef enum SiftKeyPresentMode
{
	SIFT_KP_ARRAY16 = 1,		/* the rows with the key as ascending 16-bit indexes (while at most 1/16 of the rows) */
	SIFT_KP_NEG_ARRAY16 = 2,	/* the rows without it (while at most 1/16 of the rows) */
	SIFT_KP_BITMAP = 3			/* a bit per row */
} SiftKeyPresentMode;

typedef struct SiftKeyPresentHdr
{
	uint16		mode;			/* SiftKeyPresentMode */
	uint16		count;			/* the indexes that follow (ARRAY16: the rows with the key, NEG_ARRAY16: the rows without it); the rows of the chunk for the bitmap */
	uint32		npresent;		/* the rows with the key */
} SiftKeyPresentHdr;

StaticAssertDecl(sizeof(SiftKeyPresentHdr) == 8, "SiftKeyPresentHdr must be 8 bytes");
#define SIFT_NULL_SPARSE_MAX_DIV 16				/* a list of the nulls, or of the values, is written while there are at most 1/16 of the rows */

/* What the owner of a column knows about its values that the rows do not tell: the encoding it wants for them */
typedef enum SiftChunkHint
{
	SIFT_CHINT_AUTO = 0,
	SIFT_CHINT_NUM64,			/* an INT column that is a numeric of scale SiftColIn.scale */
	SIFT_CHINT_NUM128,			/* a STR column of 16-byte SiftI128 */
	SIFT_CHINT_UUID,			/* a STR column of 16-byte uuids */
	SIFT_CHINT_INTERVAL,		/* a STR column of 16-byte SiftInterval16 */
	SIFT_CHINT_BOOL,			/* an INT column of 0 and 1 */
	SIFT_CHINT_VIEW,			/* a STR column read as SiftStrView */
	SIFT_CHINT_LIST,			/* an INT column of array lengths */
	SIFT_CHINT_KEY_PRESENT		/* an INT column that is null where a key of a document is missing and holds no value else (SIFT_CENC_KEY_PRESENT) */
} SiftChunkHint;

/* Set on the hint of a column the rows are written from (SiftPartSpec.hints) for an astext leaf: the column is a SiftColIn.astext, its hint the rest */
#define SIFT_CHINT_ASTEXT		0x80

#define SIFT_CHUNK_ALL_VALID	(UINT16_C(1) << 0)
#define SIFT_CHUNK_ALL_NULL		(UINT16_C(1) << 1)
#define SIFT_CHUNK_HAS_KINDS	(UINT16_C(1) << 2)
#define SIFT_CHUNK_COMPRESSED	(UINT16_C(1) << 3)
#define SIFT_CHUNK_HAS_OVERFLOW	(UINT16_C(1) << 4)
/*
 * The values of the chunk are the elements of the arrays that another column of the granule holds the lengths of (its SIFT_CENC_LIST_LEN chunk): there are as many as that chunk's
 * total, in the order of the rows, not one for each row. That column is named by bits 6..15 of the flags. The elements of a group in one granule are at most SIFT_GRANULE_ROWS.
 */
#define SIFT_CHUNK_ELEMS		(UINT16_C(1) << 5)
#define SIFT_CHUNK_GROUP_SHIFT	6
#define SIFT_CHUNK_GROUP_MAX	1023

typedef struct SiftChunkRef
{
	uint32		off;			/* from the start of the granule record */
	uint32		len;
	uint16		enc;
	uint16		flags;
	uint32		nnonnull;
} SiftChunkRef;

StaticAssertDecl(sizeof(SiftChunkRef) == 16, "SiftChunkRef must be 16 bytes");

typedef struct SiftGranuleHeader
{
	uint32		magic;
	uint16		nrows;
	uint16		nchunks;		/* ncols + 1 */
	uint32		flags;
	uint32		pad;
	SiftChunkRef chunks[FLEXIBLE_ARRAY_MEMBER];
} SiftGranuleHeader;

StaticAssertDecl(offsetof(SiftGranuleHeader, chunks) == 16, "SiftGranuleHeader fixed prefix is 16 bytes");

typedef enum SiftCellKind
{
	SIFT_CELL_INT = 1,
	SIFT_CELL_STR = 2,
	SIFT_CELL_F8 = 3
} SiftCellKind;

/* One column of rows in memory; isnull is NULL when no value is */
typedef struct SiftColIn
{
	SiftCellKind kind;
	const int64 *i;
	const double *f;
	const char *const *s;
	const uint32 *slen;
	const bool *isnull;
	const uint8 *tag;			/* NULL, or per row the SIFT_COLTAG_* of a value the column cannot hold (the row is null: an overflow), of a JSON null, or the kind of a value it holds (a number or boolean in an astext leaf), kept in the exceptions of the chunk */
	uint8		hint;			/* SiftChunkHint */
	bool		key_leaf;		/* the leaf of a top-level key of a jsonb document: the chunk keeps on which rows the key exists (SIFT_ENC_KEY_*): a row has it when it holds a value or its tag says the key is there */
	uint8		scale;			/* of SIFT_CHINT_NUM64 */
	/*
	 * Arrays of objects: a column of lengths (hint SIFT_CHINT_LIST) has eoff, the n + 1 offsets of the elements of its rows; the columns of the elements (group1 = 1 + the column of the
	 * lengths) hold one value for each element, in the order of the rows, so their arrays are as long as the elements of the rows they stand for.
	 */
	uint16		group1;
	const uint32 *eoff;
	/* A cell that holds a value the column cannot (tag != 0) keeps the value itself as the payload of its exception: NULL, or per row a pointer and a length (0: none) */
	const char *const *xpay;
	const uint32 *xplen;
	/* A string column whose values are already codes of the dictionary of its part (a compaction that remapped the codes of its inputs): the code of each value, in pdict, and no strings (s and slen are NULL) */
	const uint32 *dcode;
	struct SiftDictEnc *pdict;
	bool		astext;			/* a STR column that is an astext leaf of a jsonb path: its text may be held as a type (promo) */
	const struct SiftColPromo *promo;	/* the type the part holds this column in, or NULL */
} SiftColIn;

typedef struct SiftGranuleIn
{
	int			nrows;
	int			ncols;
	const uint64 *tid;
	const SiftColIn *cols;
	FullTransactionId min_xid;
	FullTransactionId max_xid;
	uint64		row_base;
	bool		all_frozen;
	int			compress_alg;	/* SIFT_CC_ALG_LZ4 or _ZSTD: a chunk that shrinks by an eighth is wrapped; 0: none */
	SiftCompressTier tier;
	const struct SiftDictEnc *const *dicts;	/* per column, the dictionary of the part it is coded by, or NULL */
	const struct SiftFsst *const *part_fsst;	/* per column, the table of symbols the whole part is coded by, or NULL (the array too: none) */
	void *const *part_cdict;	/* per column, the zstd dictionary (sift_colz_zstd_create_cdict) the string bytes of the part are compressed with, or NULL (the array too: none) */
} SiftGranuleIn;

/* The transaction id as a full one: the epoch is that of the next transaction when it is normal; the frozen id is epoch 0 */
extern FullTransactionId sift_xid_to_fxid(TransactionId xid);

/* Rows [from, ...) of a column */
extern SiftColIn sift_colin_slice(const SiftColIn *c, int from);

/* Appends the record of a granule to out (which holds a multiple of 8 bytes) and returns its directory entry */
extern SiftGranuleDesc sift_granule_encode(StringInfo out, const SiftGranuleIn *in);

/* A granule record read back: the pieces a decode needs */
typedef struct SiftGranuleView
{
	const char *rec;
	uint32		len;
	const SiftGranuleHeader *hdr;
	const struct SiftPartDicts *dicts;	/* the dictionaries of the part the granule is in, when a chunk is coded by them */
} SiftGranuleView;

/*
 * Checks the magic, the table and the bounds of the chunks of the record of len bytes, and, when the directory entry is given, its row count and the checksum of the header and
 * table (a granule kept in a page of the slab has no entry: the checksum of its page covers it)
 */
extern SiftGranuleView sift_granule_open(Relation rel, const char *rec, uint32 len, const SiftGranuleDesc *desc);

/* The same for a granule read from an opened part: the dictionaries of the part are attached */
extern SiftGranuleView sift_granule_open_part(Relation rel, const SiftPart *part, const char *rec, uint32 len, const SiftGranuleDesc *desc);

extern void sift_granule_tids(const SiftGranuleView *v, uint64 *out);
extern void sift_granule_ints(const SiftGranuleView *v, int col, int64 *out, bool *isnull);
/* The values of the ascending rows only, out[j] and isnull[j] for rows[j]: the chunk is decoded at those values and not through the rows between them */
extern void sift_granule_point_ints(const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, int64 *out, bool *isnull);
extern void sift_granule_point_f8(const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, double *out, bool *isnull);
extern void sift_granule_f8(const SiftGranuleView *v, int col, double *out, bool *isnull);

/* The most rows a scan decodes at their own values (a directory or a filter left that few): more are decoded as the whole column */
#define SIFT_POINT_ROWS_MAX		512
/* One row of a column, decoded without the others (point_decode.c): false for a NULL. The string points into the record or into a scratch of the current context */
extern bool sift_granule_point_str(const SiftGranuleView *v, int col, uint16 row, const char **p, uint32 *len);
/* The strings of the column are one compressed block: a point read inflates all of them */
extern bool sift_granule_str_is_block(const SiftGranuleView *v, int col);
extern uint8 sift_granule_point_tag(const SiftGranuleView *v, int col, uint16 row);

/* The SIFT_COLTAG_* of the rows whose value the column could not hold (0 for the others) */
extern void sift_granule_tags(const SiftGranuleView *v, int col, uint8 *tag);

/* A row of the leaf of a key of a jsonb document has the key when it holds a value or its tag says the key is there: a JSON null, a value of another type, a document that is an array or a string that holds it */
extern bool sift_key_row_present(bool isnull, uint8 tag);

/*
 * The rows of the granule whose column col holds a value, ascending, without decoding any value: none of an all-null chunk, the list as it is stored for SIFT_ENC_NONNULL_SPARSE, else the
 * bits of the validity. The count when it is at most cap, else cap + 1 (out is not complete then).
 */
extern uint16 sift_granule_nonnull_rows(const SiftGranuleView *v, int col, uint16 *out, uint16 cap);

/* The same from an opened part, reading the chunk table of granule g and at most the validity of the column (a few bytes for a sparse one); nothing is read when the table says there are more than cap */
extern uint16 sift_part_read_nonnull_rows(Relation rel, const SiftPart *part, const SiftGranuleDesc *gdesc, int col, uint16 *out, uint16 cap);

/* The values a chunk holds: the rows of the granule, or the elements of its arrays for the column of an element */
extern int	sift_granule_chunk_len(const SiftGranuleView *v, int col);

/* The payload of the exception of value i of a column (a pointer into the record, *len bytes); NULL when the value is no exception or kept none */
extern const char *sift_granule_exc_payload(const SiftGranuleView *v, int col, int i, uint32 *len);

/*
 * Rows [row_off, row_off + nrows) (nrows <= SIFT_VEC_LEN) of a column decoded straight into dst->data (SIFT_VEC_ALIGN aligned, SIFT_VEC_PAD bytes past the last element, room for the type
 * dst->type asks for) and dst->valid (SIFT_VEC_WORDS words; unused when the rows are all valid: dst->all_valid). Sets *exc_out to the exceptions of the chunk (SIFT_CHUNK_HAS_OVERFLOW) or NULL.
 * dst->dict is set for SVEC_DICT32. The string views point into the granule record, which must live as long as the vector is used.
 */
extern void sift_granule_decode_vec(const SiftGranuleView *v, int col, uint16 row_off, uint16 nrows, SiftVec *dst, const SiftVariantExcStream **exc_out);

/* The dictionary of a DICT_CODE chunk (SVEC_DICT32): its strings, which code i of dst->data names */
typedef struct SiftGranuleDict
{
	uint32		n;
	const uint32 *off;			/* n + 1 byte offsets */
	const char *bytes;
	bool		part;			/* the dictionary of the whole part, not of the granule */
} SiftGranuleDict;

typedef enum SiftMaskState
{
	SIFT_MASK_ALL_ZERO,
	SIFT_MASK_ALL_ONE,
	SIFT_MASK_BITS				/* out_mask holds the rows */
} SiftMaskState;

/*
 * The exceptions of a chunk: a header, the mask of the rows (only when there are more entries than SIFT_EXC_SPARSE_MAX: fewer are found by a binary search of their row indexes),
 * one entry per row and the end of the payload, then the payload (all of it a multiple of 8 bytes)
 */
typedef struct SiftExcHeader
{
	uint16		n_entries;
	uint16		flags;			/* SIFT_EXC_MASKED, SIFT_EXC_KINDS_PACKED */
	uint32		payload_bytes;
} SiftExcHeader;

#define SIFT_EXC_MASK_WORDS		(SIFT_GRANULE_ROWS / 64)
#define SIFT_EXC_SPARSE_MAX		511
#define SIFT_EXC_MASK_MIN_ROWS	2048
#define SIFT_EXC_MASKED			UINT16_C(1)
/* Every entry is a bare kind (no payload): the block is the header and the kind of each row of the chunk as a cascade of the codec (payload_bytes: its bytes), the entries and the mask are made when the chunk is opened */
#define SIFT_EXC_KINDS_PACKED	UINT16_C(1 << 1)
#define SIFT_EXC_MASK_BYTES(h)	(((h)->flags & SIFT_EXC_MASKED) ? SIFT_EXC_MASK_WORDS * 8 : 0)
#define SIFT_EXC_PAYLOAD_PADDED(h)	(((uint64) (h)->payload_bytes + 7) & ~(uint64) 7)
#define SIFT_EXC_BLOCK_LEN(h)	(((h)->flags & SIFT_EXC_KINDS_PACKED) ? sizeof(SiftExcHeader) + SIFT_EXC_PAYLOAD_PADDED(h) : \
								 sizeof(SiftExcHeader) + SIFT_EXC_MASK_BYTES(h) + ((uint64) (h)->n_entries + 1) * 8 + SIFT_EXC_PAYLOAD_PADDED(h))

/* A chunk of a granule as stored: the bytes (decompressed when the chunk is wrapped), where its validity bits and its body start; owned: the buffers sift_chunk_close frees */
typedef struct SiftChunk
{
	const SiftChunkRef *ref;
	const SiftGranuleView *view;
	int			idx;			/* the chunk of the granule: 0 is the _tid, column c is chunk c + 1 */
	uint16		enc;			/* ref->enc without the modifiers */
	uint8		owned;			/* SIFT_CHUNK_OWN_* */
	const char *raw;
	const uint8 *validity;		/* NULL: every row is valid (or none, with ALL_NULL) */
	const SiftExcHeader *exc;	/* NULL: no exceptions; the block as stored, or made of the packed kinds (exc_len is what the chunk holds of it) */
	uint32		exc_len;
	const SiftKeyPresentHdr *keyp;	/* NULL: no rows of a key are listed (SIFT_ENC_KEY_HAS_MASK); the list or the bitmap follows the header */
	const char *body;
	uint32		body_len;
	int			nrows;
	int			nnonnull;
} SiftChunk;

#define SIFT_CHUNK_OWN_RAW		UINT8_C(1)		/* raw is the decompressed chunk */
#define SIFT_CHUNK_OWN_VALID	UINT8_C(2)		/* validity is the bitmap made of a list of rows */
#define SIFT_CHUNK_OWN_EXC		UINT8_C(4)		/* exc is the block made of packed kinds */

/*
 * The exceptions of the rows of one column of a granule, read in ascending row order (a row before the last one asked is found by a bisection): the chunk is opened once, and a chunk that
 * has none, or holds them as the bytes of the record, allocates nothing. The payloads point into the reader and live until sift_exc_reader_close.
 */
typedef struct SiftExcReader
{
	SiftChunk	chunk;
	const SiftVariantExcEntry *entries;	/* NULL: the chunk has no exceptions */
	const char *payloads;
	const uint64 *mask;			/* SIFT_EXC_MASKED: the rows that are exceptions */
	uint32		n_entries;
	uint32		cursor;			/* the entry after the last one found */
	bool		open;
} SiftExcReader;

extern void sift_exc_reader_open(const SiftGranuleView *v, int col, SiftExcReader *er);
extern const char *sift_exc_reader_at(SiftExcReader *er, uint32 row, uint32 *len);
/* The SIFT_COLTAG_* of the row: 0 for a row that is no exception */
extern uint8 sift_exc_reader_tag(SiftExcReader *er, uint32 row);
extern void sift_exc_reader_close(SiftExcReader *er);

/* What the strings of a chunk were decoded into, to free with the cursor that holds them */
typedef struct SiftStrOwn
{
	void	   *a[3];
} SiftStrOwn;

/*
 * The rows of a column of one granule decoded batch after batch (sift_granule_decode_vec for each batch opens the chunk again, and decodes what it holds before the batch again): the chunk is
 * opened once and the state of the decode (the rank of the validity, the codec, a dictionary, the strings of a block) is kept. What a batch points to (string views, dst->dict, *exc_out) lives until
 * sift_vec_cursor_close. A cursor of the rows of a key (sift_vec_cursor_open_key_present) walks the list of rows once.
 */
typedef struct SiftGranVecCursor
{
	SiftChunk	chunk;
	SiftVecType vtype;
	bool		open;
	bool		prepared;
	bool		windowed;		/* one batch is asked of the cursor (sift_granule_decode_vec): the strings of the range are decoded, not those of the granule */
	uint8		keyp_const;		/* 0: the chunk says; 1: no row has the key; 2: every row has it */
	int			rank_word;
	int			rank_sum;
	uint32		keyp_idx;
	struct SiftCc *cc;
	SiftGranuleDict dict;
	bool		dict_ready;
	double	   *alp_all;
	const char **str_p;			/* all the strings of a block, a symbol-table or a dictionary chunk */
	uint32	   *str_len;
	SiftStrOwn	str_own;
	SiftVariantExcStream *exc;
	uint64	   *exc_mask_own;
	void	   *num;			/* the values of one batch */
	const char **batch_p;
	uint32	   *batch_len;
} SiftGranVecCursor;

extern void sift_vec_cursor_open(const SiftGranuleView *v, int col, SiftVecType vtype, SiftGranVecCursor *cur);
extern void sift_vec_cursor_decode(SiftGranVecCursor *cur, uint16 row_off, uint16 nrows, SiftVec *dst, const SiftVariantExcStream **exc_out);
extern void sift_vec_cursor_open_key_present(const SiftGranuleView *v, int col, SiftGranVecCursor *cur);
extern SiftMaskState sift_vec_cursor_key_present(SiftGranVecCursor *cur, uint16 row_off, uint16 nrows, uint64 *out_mask);
extern void sift_vec_cursor_close(SiftGranVecCursor *cur);

/* The lengths of an array column as the layout the array kernels read (one block of 64 parent rows after the other); false when the chunk is not a LIST_LEN one */
extern bool sift_granule_array_layout(const SiftGranuleView *v, int col, SiftArrayLayout *out);

typedef struct SiftStrCell
{
	const char *p;
	uint32		len;
	bool		isnull;
} SiftStrCell;

extern void sift_granule_strs(const SiftGranuleView *v, int col, SiftStrCell *out);
/* The strings of the ascending rows; out[j] is of rows[j] (isnull set), the strings point into the record or into a scratch of the current context */
extern void sift_granule_point_strs(const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, SiftStrCell *out);

/* The code of each row of a column held as a dictionary of strings, with isnull per row and the dictionary the codes name (valid with the granule); false, with nothing written, when the chunk is not coded */
extern bool sift_granule_str_codes(const SiftGranuleView *v, int col, uint32 *codes, bool *isnull, SiftGranuleDict *dict);

/*
 * The codes of a string column of a part that has a dictionary for it, each sent through map (nmap entries: the code of the dictionary of the part to a code of another one) into out
 * with isnull per row; no string is read. A chunk that the dictionary of the part does not code is damage.
 */
extern void sift_granule_dict_codes(const SiftGranuleView *v, int col, const uint32 *map, uint32 nmap, int64 *out, bool *isnull);

struct SiftF2KllBuild;
struct SiftF2KllCols;

/* The rows of a part in memory, columnar and in sort order, with the key each routes by (40 bytes, the order of core/range.h) */
typedef struct SiftRows
{
	int			nrows;
	int			ncols;
	const uint64 *tid;
	const uint32 *xmin;
	const uint32 *cmin;
	const SiftColIn *cols;
	const uint8 (*keys)[SIFT_BOUND_BYTES];
	const uint16 *sort_cols;
	const uint8 *sort_flags;	/* of each: SIFT_KEY_DESC, SIFT_KEY_NULLS_FIRST (nsort entries); NULL: all ascending, NULLs last */
	int			nsort;
	const uint8 *key_lo;		/* the key range of the part when keys is NULL */
	const uint8 *key_hi;
	bool		preagg;			/* a part of level 1 and up gets SEC_PREAGG by the integer column preagg_dim, of the integer column preagg_measure */
	int			preagg_dim;
	int			preagg_measure;
	int			time_col;		/* 1 + the integer column the part keeps a time stride index of (SEC_GRANULE_TSTRIDE), 0: none */
	int			time_cols[SIFT_MANIFEST_TIME_COLS];	/* 1 + the integer column the part's time bounds are of (SiftPartDesc.time_min), 0: none */
	uint32		part_flags;		/* SIFT_PART_FLAG_TIME_HOT or _TIME_COLD: the tier of the part, set by the compaction that splits rows by time; SIFT_PART_FLAG_BUILT, set by a build */
	int			compress_alg;	/* of SiftGranuleIn */
	const struct SiftPartDicts *adopt_dicts;	/* the dictionaries of a part that holds most of these rows, whose tables the part may keep (NULL: train them) */
	const struct SiftF2KllCols *kll;	/* the columns a part of level 1 and up keeps a quantile sketch of (SEC_KLL), NULL: none */
	const SiftPartColIdentity *col_ids;	/* ncol_ids == ncols of them: written in SEC_SCHEMA with SIFT_SCHEMA_EXT_ATTNUM (NULL: none) */
	int			ncol_ids;
	bool		bulk_tail;		/* SiftPartIn.bulk_tail of the part written (a direct write of the cold tier) */
	const void *defaults;		/* SEC_SCHEMA_DEFAULTS of the part, defaults_len bytes (NULL: the table has no fast default and no dropped column) */
	uint64		defaults_len;
	uint32		defaults_stamp;	/* its stamp, which SEC_SCHEMA names */
} SiftRows;

/* The dictionary of a string column of a part (SEC_DICTS): its distinct strings in byte order; a granule keeps the codes */
typedef struct SiftDictEnc
{
	uint32		n;
	const char **p;
	uint32	   *len;
	uint32	   *row_codes;		/* NULL, or the code of each of the nrows values in the dictionary (0 for a null): kept by sift_dict_enc_build so that no string is searched for again */
	int			nrows;
} SiftDictEnc;

/* What SEC_DICTS holds for a column of a part: its distinct strings (the codes of its chunks), the table of symbols all its chunks are coded by, or a dictionary of zstd for the bytes of its chunks */
typedef enum SiftDictKind
{
	SIFT_DICT_KIND_STR = 0,
	SIFT_DICT_KIND_FSST = 1,
	SIFT_DICT_KIND_ZSTD = 2
} SiftDictKind;

struct SiftFsst;

typedef struct SiftPartDict
{
	uint16		col;
	uint8		kind;			/* SiftDictKind */
	uint32		n;				/* STR: the strings; FSST: the symbols; ZSTD: the bytes of the dictionary */
	uint32	   *off;			/* STR: n + 1 */
	char	   *bytes;			/* STR: the strings; ZSTD: the dictionary as stored */
	struct SiftFsst *fsst;		/* FSST */
	void	   *zstd_ddict;		/* ZSTD: the handle of sift_colz_zstd_create_ddict, freed with the memory context the part's dictionaries are in */
} SiftPartDict;

typedef struct SiftPartDicts
{
	int			n;
	SiftPartDict *d;
} SiftPartDicts;

/* The dictionary of the given kind that column col of the part has among its dictionaries (ds may be NULL), or NULL */
extern const SiftPartDict *sift_part_dict_find(const SiftPartDicts *ds, int col, SiftDictKind kind);

/* The dictionaries of a part (SEC_DICTS), read and cached with the part; NULL when it has none */
extern const SiftPartDicts *sift_part_dicts(Relation rel, const SiftPart *part);

/* The number of rows from `from` that make one granule: SIFT_GRANULE_ROWS, or fewer when an array column would hold more elements than that */
extern int	sift_rows_granule_len(const SiftRows *rows, int from);

/* Writes a part of format major 2 from the rows: granules, directory, TID index, zones and (unless every row is frozen) the version vectors; the descriptor for the manifest */
extern SiftPartDesc sift_part_write_rows(Relation rel, uint32 part_id, uint64 part_gen, uint16 level, SiftCompressTier tier, const SiftRows *rows);

/* The same part of level 0 and generation 0 made in memory and written nowhere: what a standby reads the rows of a micro-slab from (col/f2/f2_slab_view.c) */
extern SiftPart *sift_part_rows_in_memory(Relation rel, uint32 part_id, SiftCompressTier tier, const SiftRows *rows, SiftPartDesc *d);

/* The directory entry of granule g of an opened part, read without the others */
extern SiftGranuleDesc sift_part_read_gdesc(Relation rel, const SiftPart *part, uint32 g);

/* Granule g of an opened part: the record (palloc'd) and its directory entry */
extern char *sift_part_read_granule(Relation rel, const SiftPart *part, uint32 g, SiftGranuleDesc *desc);

/* The same with the chunks of the columns whose want flag is set and the _tid (chunk 0) read only: the bytes of the other chunks are not defined */
extern char *sift_part_read_granule_cols(Relation rel, const SiftPart *part, uint32 g, const bool *want, SiftGranuleDesc *desc);

/*
 * A granule read as a scan needs it: the header and the table of its chunks first, which is all a scan of a granule that no row passes ever reads, then the chunks it asks for, each once.
 * Chunk 0 is the _tid, chunk c + 1 the column c. rec holds the record: the bytes of a chunk not yet loaded are not defined.
 */
typedef struct SiftLazyGranule
{
	const SiftPart *part;
	SiftGranuleDesc gd;
	char	   *rec;
	bool	   *loaded;
	uint32		nchunks;
} SiftLazyGranule;

extern void sift_lazy_granule_open(Relation rel, const SiftPart *part, uint32 g, SiftLazyGranule *lg);

/* The same for a scan that holds the directory of the part (SiftF2PartPin.gdir): the entry of the granule is not read a second time */
extern void sift_lazy_granule_open_desc(Relation rel, const SiftPart *part, uint32 g, const SiftGranuleDesc *gd, SiftLazyGranule *lg);

/* The chunks listed (n of them), those not yet loaded in one batched read */
extern void sift_lazy_granule_load(Relation rel, SiftLazyGranule *lg, const int *chunks, int n);

/* The _tid column of granule g (dir[g].nrows values), read without the chunks of the other columns */
extern uint64 *sift_part_read_tids(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, uint32 g);

/* The same for granules [g0, g0 + n), n at most SIFT_TID_BATCH_GRANULES: the reads of all of them go together, so a stream of the blocks looks ahead over them instead of one read after another */
#define SIFT_TID_BATCH_GRANULES 64
extern uint64 **sift_part_read_tids_batch(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, uint32 g0, uint32 n, SiftIoMode mode);

/* The directory of the granules of an opened part (ngranules entries) */
extern SiftGranuleDesc *sift_part_granule_dir(Relation rel, const SiftPart *part);

/*
 * Parts that can be joined as they are: the same schema and dictionaries, rows frozen, no pre-aggregate. sift_part_write_joined writes one part whose granules are those of the
 * parts in the order given (their keys ascend from one to the next), copied with their zones, address index, time strides and exceptions; *granules is how many. The granules are read from the parts and written
 * one page at a time, so what the writer holds is the metadata of the sections and one granule, *writer_bytes of them.
 */
extern bool sift_parts_joinable(Relation rel, SiftPart *const *src, int n);
extern SiftPartDesc sift_part_write_joined(Relation rel, uint32 part_id, uint64 part_gen, uint16 level, SiftPart *const *src, const SiftPartDesc *sdesc, int n, struct SiftF2KllBuild *kll, uint64 *granules, uint64 *writer_bytes);

/* The inserter transactions of the rows of granule g (xmin, cmin per row; a granule of one transaction and command keeps none in SEC_VERS and is made from its entry), or NULL when the granule is frozen */
typedef struct SiftRowVer
{
	uint32		xmin;
	uint32		cmin;
} SiftRowVer;

extern SiftRowVer *sift_part_read_vers(Relation rel, const SiftPart *part, const SiftGranuleDesc *dir, uint32 g);

/* Per-granule TID summary of SEC_TID_INDEX */
typedef struct SiftTidIndexEntry
{
	uint64		tid_min;
	uint64		tid_max;
	uint64		blk_bloom;		/* a bit of the 64 for each heap block that has a row in the granule */
} SiftTidIndexEntry;

static inline uint64
sift_tid_block_bit(uint64 tid)
{
	return UINT64CONST(1) << (((tid >> 16) * UINT64CONST(0x9E3779B97F4A7C15)) >> 58);
}

#define SIFT_TSTRIDE_ROWS		256

/* The least and the greatest time of 256 rows of a granule (SEC_GRANULE_TSTRIDE: a header, the first stride of each granule, then the strides) */
typedef struct SiftTimeStride
{
	int64		time_min;
	int64		time_max;
} SiftTimeStride;

StaticAssertDecl(sizeof(SiftTimeStride) == 16, "SiftTimeStride is 16 bytes");

typedef struct SiftTStrideHeader
{
	uint32		ngranules;
	uint32		total_strides;
} SiftTStrideHeader;

/* The strides of granule g of an opened part (palloc'd, *n of them); NULL when the part keeps no time index */
extern SiftTimeStride *sift_part_read_tstride(Relation rel, const SiftPart *part, uint32 g, int *n);

/* The columns of a granule that have exceptions, for SEC_VARIANT_EXC */
typedef struct SiftVariantExcSummary
{
	uint32		granule;
	uint16		col;
	uint16		pad;
	uint32		n_entries;
	uint32		pad2;
} SiftVariantExcSummary;

StaticAssertDecl(sizeof(SiftVariantExcSummary) == 16, "SiftVariantExcSummary is 16 bytes");

/* Per column and granule: the least and the greatest of an integer column and the rows without a value of any column of the rows */
typedef struct SiftZone
{
	int64		min;
	int64		max;
	uint32		nnull;
	uint32		flags;			/* SIFT_ZONE_* */
} SiftZone;

#define SIFT_ZONE_NONE			(UINT32_C(1) << 0)	/* no least and greatest: a column that is no integer or holds only nulls */
#define SIFT_ZONE_KEY_ALL_ABSENT (UINT32_C(1) << 1)	/* the leaf of a key of a document: the key is on no row of the granule (SIFT_ENC_KEY_ALL_ABSENT) */
#define SIFT_ZONE_KEY_ALL_PRESENT (UINT32_C(1) << 2)	/* the key is on every row (SIFT_ENC_KEY_ALL_PRESENT) */
/*
 * A text column held as a type in this granule (SIFT_CENC_STR_PROMO): min and max are the least and the greatest value of the rows of the type, in the units the type is held in (the
 * kind and its parameter are in the bits above 8); SIFT_ZONE_PROMO_EXC: some row is not of the type (its text is held apart, or it is a JSON null or a value no text holds)
 */
#define SIFT_ZONE_PROMO			(UINT32_C(1) << 3)
#define SIFT_ZONE_PROMO_EXC		(UINT32_C(1) << 4)
#define SIFT_ZONE_PROMO_KIND(f)	(((f) >> 8) & 0xF)
#define SIFT_ZONE_PROMO_PARAM(f) (((f) >> 12) & 0xF)
#define SIFT_ZONE_PROMO_FLAGS(f) (((f) >> 16) & 0xFF)

/* The value a text literal has in the type a zone's granule holds a column in (SIFT_ZONE_PROMO); false when no row of the granule can be that text of the type's spelling */
extern bool sift_promo_literal(uint32 zone_flags, const char *s, uint32 len, int64 *v);

/*
 * The rows [row_off, row_off + nrows) (nrows <= SIFT_VEC_LEN) of the leaf of a key on which jsonb_exists(document, key) holds, as the words of a vector (bit i: row row_off + i; the bits past nrows
 * zero), without decoding a value; the leaf is one whose chunk keeps the key (SiftColIn.key_leaf), else ERROR. A chunk that has none of the rows is SIFT_MASK_ALL_ZERO and one that has all
 * of them SIFT_MASK_ALL_ONE, and out_mask is not written then.
 */
extern SiftMaskState sift_granule_decode_key_present(const SiftGranuleView *v, int col, uint16 row_off, uint16 nrows, uint64 *out_mask);

#endif							/* SIFT_GRANULE_H */
