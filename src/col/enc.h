/*
 * col/enc.h - L1 of the column engine: the schema, dictionaries, batches (the decoded rows
 * of one superblock), the unit record and column chunk encodings, chunk filters and zone maps. No pages, no heap, no WAL: values and bytes,
 * so a table access method can reuse it. Portable (core/host.h only).
 */
#ifndef SIFT_COL_ENC_H
#define SIFT_COL_ENC_H

#include "core/host.h"

#include "col/col.h"
#include "col/kern.h"
#include "col/codec.h"

typedef struct SiftColSchema
{
	int			ncols;
	int			ngroups;
	bool		contain;		/* some leaf is the head of a range or network with the lanes of its containment operators: column format minor 26 (SIFT_COL_FORMAT_CONTAIN) */
	bool		wide;			/* some leaf is a lane, money or interval (sift_col_def_needs_wide): a segment of this schema is column format minor 5 */
	bool		kinds;			/* some leaf is filled from a jsonb path (sift_col_def_has_kinds): a segment of this schema whose kinds are complete is column format minor 6 */
	SiftColDef *cols;			/* [ncols], paths copied */
	uint32		hash;			/* of ids, kinds, stores, type OIDs, attnos, parents, paths (not flags) */
} SiftColSchema;

/*
 * Copies and validates the leaf list: ids 0..n-1 in order, a child's parent is an earlier GROUP leaf, groups are top level (parent -1),
 * 1 <= n <= SIFT_COL_MAX_COLS. ERRCODE_INVALID_PARAMETER_VALUE otherwise. Allocates in mcxt.
 */
extern SiftColSchema *sift_cschema_create(MemoryContext mcxt, const SiftColDef *defs, int n);

/* The SCHEMA section of a segment: magic 'COLS', ncols, hash, per leaf a fixed part and the path */
extern void sift_cschema_serialize(const SiftColSchema *s, StringInfo out);
/* NULL if the bytes are not a schema (the caller raises ERRCODE_INDEX_CORRUPTED) */
extern SiftColSchema *sift_cschema_parse(MemoryContext mcxt, const void *data, Size len);

struct SiftCDictLook;

typedef struct SiftCDict
{
	MemoryContext mcxt;
	uint32		n;
	uint32		cap;
	uint32	   *off;			/* n + 1 */
	char	   *bytes;
	Size		nbytes;
	Size		bytescap;
	uint32	   *slots;			/* open addressing table of code + 1; built by intern, and by find once the dictionary has been asked a few times */
	uint32		nslots;
	uint32		finds;			/* linear finds so far, before the table is worth building */
	uint32		tests;			/* linear value tests so far, before the sorted order is worth building */
	uint64		orows;			/* rows of top-N scans compared with the strings so far (cdict_order) */
	uint32	   *sorted;			/* the codes in byte order of their strings; built by match once the dictionary has been tested a few times */
	bool		blocked;		/* parsed from the blocks of a segment of column format minor 14 (sift.col_verify checks the minor) */
	bool		stored_order;	/* sorted came from the stored form that is searched in place (sift.col_verify checks it) */
	const struct SiftCDictLook *look;	/* a dictionary that stays in the stream (n only): find and match read the segment (col/seg.c) */
	uint32	   *rows;			/* writer only: the rows of each code (option count_columns); serialized with the order as DICT_LOOKUP_COUNT */
	bool		hashed;			/* writer only: serialize the hash of the strings after the order (DICT_HASHED) */
} SiftCDict;

/* The two searches a test makes of a dictionary, for one that stays in the stream */
typedef struct SiftCDictLook
{
	int64		(*find) (const struct SiftCDictLook *look, const char *s, uint32 len);
	uint32		(*match) (const struct SiftCDictLook *look, SiftColOp op, const SiftColValue *c, uint64 *lut);
} SiftCDictLook;

extern SiftCDict *sift_cdict_create(MemoryContext mcxt);
/* The code of the value, adding it when new; len <= SIFT_COL_MAX_VALUE (ERRCODE_PROGRAM_LIMIT_EXCEEDED otherwise) */
extern uint32 sift_cdict_intern(SiftCDict *d, const char *s, uint32 len);
/* The code, or -1 */
extern int64 sift_cdict_find(SiftCDict *d, const char *s, uint32 len);
/* Whether a string passes a LT, LE, GT, GE (byte order) or PREFIX predicate on the string c */
extern bool sift_cstr_matches(SiftColOp op, const char *s, uint32 len, const SiftColValue *c);
/* Sets in lut the bit of every code whose string satisfies op (LT, LE, GT, GE in byte order, PREFIX) against c; returns how many */
extern uint32 sift_cdict_match(SiftCDict *d, SiftColOp op, const SiftColValue *c, uint64 *lut);
/* Bytewise order of strings */
extern int	sift_cstr_cmp(const char *a, uint32 alen, const char *b, uint32 blen);
/* The strings of a dictionary in byte order, by position: a search of them is the same whether they are in memory or in the stream */
typedef struct SiftCSorted
{
	uint32		n;
	const char *(*str) (const void *ctx, uint32 pos, uint32 *len);
	const void *ctx;
} SiftCSorted;
static inline int
sift_csorted_cmp(const SiftCSorted *s, uint32 pos, const SiftColValue *c)
{
	uint32		len;
	const char *p = s->str(s->ctx, pos, &len);

	return sift_cstr_cmp(p, len, c->v.s, c->len);
}

static inline bool
sift_csorted_prefix(const SiftCSorted *s, uint32 pos, const SiftColValue *c)
{
	uint32		len;
	const char *p = s->str(s->ctx, pos, &len);

	return sift_cstr_matches(SIFT_COLOP_PREFIX, p, len, c);
}

extern uint32 sift_csorted_bound(const SiftCSorted *s, const SiftColValue *c, bool after);
extern void sift_csorted_range(const SiftCSorted *s, SiftColOp op, const SiftColValue *c, uint32 *from, uint32 *to);
/* The bytes and length of a code < d->n */
extern const char *sift_cdict_str(const SiftCDict *d, uint32 code, uint32 *len);
#define DICT_PLAIN		0
#define DICT_STRS		1
#define DICT_LOOKUP		2
#define DICT_BLOCKS		3
#define DICT_LOOKUP_COUNT	4			/* form 2, then (4-aligned) the rows of the strings below each place in byte order, a block of DICT_BLOCK places at a time */
#define DICT_HASHED		0x10		/* or'ed into form 2 or 4: after everything of the form, a hash of the strings to their codes */
#define DICT_HEAD		(2 * sizeof(uint32))
#define DICT_HEAD_LOOKUP	(4 * sizeof(uint32))
#define DICT_BLOCK		64			/* strings in a block of the blocks forms: the unit of compression and of a read in place; 32 compress a dictionary of 20,000 titles 13% worse than the compact form, 64 as well */
#define DICT_BLOCK_RAW_MAX	(DICT_BLOCK * (SIFT_COL_DICT_MAX_ENTRY + 1))	/* the most bytes of a block before compression: a length and the string of each */
#define DICT_BLOCK_HEAD		4			/* u8 algorithm (0: raw, else SIFT_CC_ALG_*), u8 0, u16 raw bytes */
/*
 * DICTS section entry: u32 form, u32 n, then the plain dictionary (u32 off[n + 1], bytes) or, when the policy is not "none" and it is
 * smaller, the codec's string list (lengths as an integer cascade, bytes under the cold wrapper rule). pol NULL: plain. form: 0 for these, or the form that
 * is read in place: u32 form 3 (DICT_BLOCKS), u32 n, u32 length of every string (0: they differ), u32 bytes of the strings,
 * u32 boff[blocks + 1], the blocks of DICT_BLOCK strings each in code order (a length byte for each string unless they have one length, then the strings,
 * compressed when that is smaller; DICT_BLOCK_HEAD bytes of header); form 2 (DICT_LOOKUP) adds u32 code[n] in byte order of the strings (4-aligned),
 * which the search of a string reads; form 4 (DICT_LOOKUP_COUNT, d->rows set) adds after code[], for nb = n / DICT_BLOCK rounded up: {u32 base, u32 off}[nb + 1]
 * (the rows of the codes at the places below the block, and where its packed bytes start; the last entry is the total and the bytes in all), u8 cmin[nb] (padded to 4),
 * and the packed bytes: the rows of the codes of block b, less cmin[b] (the smaller of their least count and 255), as DICT_BLOCK values of w bits each, w = (off[b + 1] - off[b]) / 8,
 * lowest first (a block of one count, as a dictionary of unique strings has, takes no bytes)
 */
/* The bytes of the row counts of a form 4 dictionary of n strings before the packed bytes (the records and cmin), and the number of blocks */
#define DICT_COUNT_NB(n)		(((n) + DICT_BLOCK - 1) / DICT_BLOCK)
#define DICT_COUNT_HEAD(n)		((uint64) 8 * (DICT_COUNT_NB(n) + 1) + ((uint64) DICT_COUNT_NB(n) + 3) / 4 * 4)
/*
 * The hash of a dictionary of DICT_HASHED: SIFT_CDICT_HASH_SLOTS(n) u32 slots, linear probing from the position of the 64-bit hash of the string (its high half scaled to the slots), a slot
 * empty (0) or (fingerprint << cb) | (code + 1), cb the bits of n and the fingerprint the low bits of the hash that are left: an absent string meets an empty slot or a
 * fingerprint that differs, so the search of a segment that lacks it reads one slot and no block; a string that matches is checked against its code's string (a block).
 */
#define SIFT_CDICT_HASH_SLOTS(n)	((n) + (n) / 4 + 8)
extern uint64 sift_cdict_hash(const char *s, uint32 len);
static inline uint32
sift_cdict_hash_cb(uint32 n)
{
	uint32		b = 1;

	while (((uint64) 1 << b) <= n)
		b++;
	return b;
}
static inline uint32
sift_cdict_hash_pos(uint64 h, uint32 nslots)
{
	return (uint32) (((h >> 32) * nslots) >> 32);
}
static inline uint32
sift_cdict_hash_slot(uint64 h, uint32 cb, uint32 code)
{
	return ((uint32) h & (UINT32_MAX >> cb)) << cb | (code + 1);
}
extern void sift_cdict_serialize(SiftCDict *d, const SiftCcPolicy *pol, uint32 form, StringInfo out);
/* The bytes the blocks form (DICT_BLOCKS) adds to the compact one, and the bytes of the compact form, estimated from a sample of the blocks and of the start of the dictionary: short strings that repeat their words compress across the dictionary and not within a block, ids, hashes and long strings alike in both */
extern Size sift_cdict_blocks_extra(SiftCDict *d, const SiftCcPolicy *pol, Size *compact_bytes);
/* Copy of a serialized dictionary; NULL when malformed. *used = bytes consumed */
extern SiftCDict *sift_cdict_plain_begin(MemoryContext mcxt, uint32 n);
extern bool sift_cdict_plain_bytes(SiftCDict *d, MemoryContext mcxt, uint64 room);
extern bool sift_cdict_plain_valid(const SiftCDict *d);
extern SiftCDict *sift_cdict_parse(MemoryContext mcxt, const void *data, Size len, Size *used);
extern void sift_cdict_view(const SiftCDict *d, uint64 id, SiftColDict *view);

/* Values of one leaf: key-domain integers (INT as is, F4/F8 order keys, STR codes into the batch's dictionary of the leaf) */
typedef struct SiftCVals
{
	uint32		n;				/* values: rows, or the elements of all rows for a child leaf */
	uint32		cap;
	int64	   *key;
	uint64	   *nulls;			/* NULL until the first NULL; bit set = NULL; capacity cap bits */
	uint64	   *ovf;			/* NULL until the first overflow value; bit set = overflow (also set in nulls) */
	uint8	   *otag;			/* NULL until the first slot with a tag: its SIFT_COLTAG_* (an overflow value's type, a kind, 0 elsewhere), cap bytes */
	uint32	   *eoff;			/* GROUP leaves: element offset of each row, nrows + 1 entries */
	/* VAR leaves: key[i] is the byte length of value i (0 for NULL), the bytes of all values follow each other in vb, voff[i] the start of value i */
	uint32	   *voff;			/* n + 1 entries once a value was added */
	char	   *vb;
	Size		vcap;
	bool		noted;			/* the leaf went past the unit's byte budget and the NOTICE was raised */
} SiftCVals;

typedef struct SiftColBatch
{
	MemoryContext mcxt;
	const SiftColSchema *schema;
	uint32		sb;
	uint32		nrows;
	uint32		rowcap;
	SiftKey    *keys;			/* ascending, all in superblock sb (a keyed batch: any blocks, sb is the sequence number of the unit) */
	bool		keyed;			/* a key-cut unit: the rows are those of a range of the declared order key, whatever their pages */
	bool		unsorted;		/* the rows are in any order (the window of the cut of a key-cut merge, col/order.c): not a unit to encode */
	bool		hasver;			/* xmin and cmin are set for every row (0 = frozen); false: every row is frozen and the arrays mean nothing */
	uint32	   *xmin;			/* [rowcap]: the transaction that inserted the row of a sift_table group (docs/COL-TAM.md section 7) */
	uint32	   *cmin;
	SiftCVals  *col;			/* [ncols] */
	SiftCDict **dict;			/* [ncols]: batch dictionary of each STR leaf, NULL for the others */
} SiftColBatch;

/* Legacy write-buffer values carry UNSURE in memory; a writer must not certify them as complete kinds. */
extern bool sift_cbatch_kinds_exact(const SiftColBatch *b);
extern SiftColBatch *sift_cbatch_create(MemoryContext mcxt, const SiftColSchema *schema);
/* Empties the batch (memory kept) for superblock sb; the dictionaries restart */
extern void sift_cbatch_reset(SiftColBatch *b, uint32 sb);
/* Sets the version of row r (< nrows or the row being added), makes the batch versioned on the first nonzero xmin */
extern void sift_cbatch_set_ver(SiftColBatch *b, uint32 r, uint32 xmin, uint32 cmin);
/* Appends a row; its TID's superblock must be b->sb and its key above the last one's. STR values are interned. */
extern void sift_cbatch_add(SiftColBatch *b, const SiftColRow *row);
/* The same for a row of a sift_table group that carries its version (xmin 0 = frozen); the copies and merges of batches keep it */
extern void sift_cbatch_add_ver(SiftColBatch *b, const SiftColRow *row, uint32 xmin, uint32 cmin);
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

#define SIFT_COLZ_NOSUM		0x1		/* the integer sum overflowed int64 */
#define SIFT_COLZ_NAN		0x2		/* a float chunk holds a NaN */
#define SIFT_COLZ_CONST		0x4		/* every non-NULL value equal */
#define SIFT_COLZ_OVERFLOW	0x8		/* some value overflowed: the zone does not describe it */
#define SIFT_COLZ_INLINE	0x10	/* STR leaf stored inline (SIFT_COLENC_ISTR): min and max are the prefix keys of the smallest and largest value, set is all ones */
#define SIFT_COLZ_JNULL		0x20	/* some NULL slot is a JSON null: a cast of it raises before PostgreSQL 18. Set only where the chunk has kinds, informational: the zone's other fields do not depend on it */

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

#define SIFT_COLC_NULLS		0x1
#define SIFT_COLC_OVF		0x2		/* an overflow bitmap follows the null bitmap */
#define SIFT_COLC_TAGS		0x4		/* minor 4, only with OVF: after the overflow bitmap one nibble (SIFT_COLTAG_*) per set overflow bit, in slot order, padded to 8 bytes */
#define SIFT_COLC_KINDS		0x8		/* minor 4: after the tags a word whose low byte has bit t set for each tag t that has a bitmap, then those bitmaps (n bits, padded to 8 bytes) in tag order: the slots that do not overflow with a type their leaf does not say (JSON null in a NULL slot, the number or boolean in an astext leaf) */
#define SIFT_COLENC_CONST	0
#define SIFT_COLENC_FOR		1
#define SIFT_COLENC_PACK	2
#define SIFT_COLENC_RLE		3		/* reserved */
#define SIFT_COLENC_CC		4		/* the payload is one blob of the chunk codec: bits and base are 0 */
#define SIFT_COLENC_STRS	5		/* VAR leaves: the payload is the codec's list of n strings (the NULL slots are empty strings); bits and base are 0 */
#define SIFT_COLENC_ISTR	6		/* STR leaves, inline: the same payload as STRS, the unit has no dictionary codes for this leaf */

typedef struct SiftCChunk
{
	uint8		enc;
	uint8		bits;
	uint8		flags;
	uint32		n;
	int64		base;
	const uint64 *nulls;		/* NULL without SIFT_COLC_NULLS */
	const uint64 *ovf;			/* NULL without SIFT_COLC_OVF */
	const uint8 *tags;			/* NULL without SIFT_COLC_TAGS: the nibbles, low nibble first */
	uint8		kmask;			/* SIFT_COLC_KINDS: bit t set when kind[t] is there */
	const uint64 *kind[8];		/* [SIFT_COLTAG_*] the slots of that kind, NULL when none */
	const uint8 *payload;
	uint32		paylen;
} SiftCChunk;

/* The JSON type tag of the overflow value at slot (SIFT_COLTAG_*); 0 for a chunk without tags. Walks the bitmap: for one slot, not for a loop */
extern uint8 sift_cchunk_ovf_tag(const SiftCChunk *c, uint32 slot);
/* The tag of every slot, a byte each (an overflow value's type, a kind; 0 for the other slots); dst has room for n bytes. False when the chunk has neither tags nor kinds (dst untouched) */
extern bool sift_cchunk_tags(const SiftCChunk *c, uint8 *dst);
/* bits = the slots with tag `tag`: overflow values of that type and the slots of that kind (padded words of c->n); tag 0: overflow values without a known type, also in a chunk without tags */
extern void sift_cchunk_tag_bits(const SiftCChunk *c, uint8 tag, uint64 *bits);

/*
 * Views a chunk; false when its bytes are inconsistent. p must be 8-byte aligned and, like every array a kernel reads,
 * have SIFT_COL_PAD readable bytes past len (the unit record's reader provides them).
 */
extern bool sift_cchunk_parse(const uint8 *p, uint32 len, SiftCChunk *c);

/*
 * Decoding to the key domain: dst[0..n) (NULL slots 0). The _sel form decodes only the positions idx[0..m) into dst[0..m). dst needs
 * n + SIFT_COL_PAD/8 slots.
 */
extern void sift_cchunk_decode(const SiftCChunk *c, int64 *dst);
/*
 * The same into the consumer's lane, without the int64 pass: _u32 the low 32 bits of the keys (an int4 or date, a dictionary code; dst needs
 * SIFT_CC_CAP32(n) slots), _f8 the doubles of a float chunk (dst needs n + SIFT_COL_PAD/8 slots). NULL slots 0.
 */
extern void sift_cchunk_decode_u32(const SiftCChunk *c, uint32 *dst);
extern void sift_cchunk_decode_f8(const SiftCChunk *c, double *dst);
/*
 * The code lane of a dictionary of ndict values (the NULL code is ndict, the tables built over a dictionary have ndict + 1 entries): the
 * same as _u32, and a code past the dictionary raises ERRCODE_DATA_CORRUPTED instead of indexing outside those tables.
 */
extern void sift_cchunk_decode_codes(const SiftCChunk *c, uint32 ndict, uint32 *dst);
/* Raises ERRCODE_DATA_CORRUPTED unless codes[0..n) are all below limit (a kernel that indexes a table by them reads limit entries) */
extern void sift_ccodes_below(const uint32 *codes, uint32 n, uint32 limit);
/* VAR and inline STR chunks (enc STRS, ISTR): the value offsets (n + 1) and the bytes of all values, palloc'd in mcxt; the values are exact, the bytes at NULL slots are none */
extern void sift_cchunk_strs(const SiftCChunk *c, MemoryContext mcxt, uint32 **off, char **bytes);
extern void sift_cchunk_decode_sel(const SiftCChunk *c, const uint32 *idx, uint32 m, int64 *dst);

/*
 * An inline STR chunk (enc ISTR) as the dictionary-coded chunk the readers and tests work on: *dict is a dictionary of the chunk's own
 * strings (palloc'd in mcxt, codes in first-seen order) and *out a FOR chunk of the codes (payload palloc'd in mcxt, with the NULL and
 * overflow bitmaps of c). idx[0..m) names the only positions given a code (the others read 0), NULL: all. False when c is another encoding
 * (*out is then c).
 */
extern bool sift_cchunk_istr_codes(const SiftCChunk *c, MemoryContext mcxt, const uint32 *idx, uint32 m, SiftCDict **dict, SiftCChunk *out);

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
	SIFT_CT_NSET,				/* non-NULL and none of set[] */
	SIFT_CT_STRS				/* STRS and ISTR chunks: pred holds, string by string; an inline chunk first filters on its prefix blob */
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
	const SiftColPred *pred;	/* STR: the predicate, for the zones of inline units (which have no codes); not copied */
	const struct SiftCStrSet *strset;	/* STRS: the constants of an IN or NOT IN list longer than SIFT_CSTRSET_MIN, sorted */
} SiftCTest;

/*
 * The one matcher of an IN list over strings or bytes: the constants sorted once, a binary search for each value (a comparison with each constant up to
 * SIFT_CSTRSET_MIN). The chunks of strings and the records of a write buffer (sift_cwbuf_lookup) use it; for the keys of the integer leaves the same
 * sorted set is the SIFT_CT_SET of sift_ctest_make, probed with sift_ctest_key.
 */
#define SIFT_CSTRSET_MIN	8
typedef struct SiftCStrSet SiftCStrSet;
extern SiftCStrSet *sift_cstrset_make(const SiftColValue *vals, int n);
extern bool sift_cstrset_has(const SiftCStrSet *s, const char *str, uint32 len);
/* Whether key is one of the keys a test of an integer leaf (RANGE, SET, NONE...) holds, as the units' filter decides it */
extern bool sift_ctest_key(const SiftCTest *t, int64 key);

/* dict: the segment's dictionary of the leaf (STR; may be NULL for the other stores). Allocates the LUT in CurrentMemoryContext. */
extern void sift_ctest_make(const SiftColDef *def, SiftCDict *dict, const SiftColPred *p, SiftCTest *t);

/*
 * The test of a predicate on a leaf whose chunks hold the strings themselves (VAR leaves, and the inline chunks of STR leaves, whose unit has
 * no dictionary to translate the constants with): no dictionary needed, pred must outlive t. For a VAR leaf sift_ctest_make does this.
 */
extern void sift_ctest_make_strs(const SiftColPred *p, SiftCTest *t);

/* bits = the non-NULL rows of a STRS or ISTR chunk whose byte length is in [lo, hi] (a column kernel over the decoded lengths), padded words of c->n rows */
extern void sift_cchunk_strs_length(const SiftCChunk *c, uint32 lo, uint32 hi, uint64 *bits);

/* Can a unit with this zone hold a matching row (conservative) */
extern bool sift_ctest_zone(const SiftCTest *t, const SiftCZone *z);

/* bits &= rows of the chunk that satisfy the test (bits has the padded words of c->n rows) */
extern void sift_ctest_run(const SiftCTest *t, const SiftCChunk *c, uint64 *bits);

/* ---------- units ---------- */

/*
 * Encodes the batch as one unit record appended to out (the record starts 8-byte aligned in out: padding is added first, *start gets its offset):
 * the row directory then one chunk per leaf (a leaf all of whose values are NULL still gets a CONST chunk with the NULL bitmap). zones[ncols]
 * receives the zones (may be NULL). dictmap[col] (NULL array or NULL entry: identity) translates the batch's STR codes into the segment's; inl[col]
 * (NULL array: none) stores that STR leaf inline, as strings (enc ISTR), and then dictmap[col] is not used.
 * SIFT_COL_PAD zero bytes stay readable after the record in out's buffer (not counted in out->len), so it can be parsed and filtered in place.
 * pol[ncols] (NULL: none) is the compression policy of each leaf: a chunk that is not constant is then a blob of the chunk codec
 * (SIFT_COLENC_CC); without it the chunk is the plain FOR or PACK form (in-memory units that are only scanned).
 * tags: every chunk with overflow values gets SIFT_COLC_TAGS and every chunk with kinds SIFT_COLC_KINDS (column format minor 4); without it the chunk is what minor 3 writes.
 */
extern void sift_cunit_encode(const SiftColBatch *b, uint32 *const *dictmap, const bool *inl, const SiftCcPolicy *pol, bool tags, StringInfo out,
							  Size *start, SiftCZone *zones);

/* The bytes of the row directory of the sorted keys of a unit: of superblock sb, or of a key-cut unit (keyed); *pages is the number of heap pages the keys fall in */
extern Size sift_cunit_directory_bytes(const SiftKey *keys, uint32 nrows, bool keyed, uint32 sb, uint32 *pages);

/*
 * Must the STR leaf col of the batch be stored inline in a segment whose dictionary of the leaf is seg: some string is over
 * SIFT_COL_DICT_MAX_ENTRY bytes, or the dictionary would exceed maxbytes with the strings the batch adds.
 */
extern bool sift_cbatch_str_inline(const SiftColBatch *b, int col, SiftCDict *seg, Size maxbytes);

typedef struct SiftCUnitView
{
	const uint8 *rec;
	uint32		len;
	const SiftColSchema *schema;
	uint32		nrows;
	uint32		npages;
	uint64		pagemask[SIFT_PAGEMASK_WORDS];
	uint8		page[SIFT_SB_PAGES];	/* page within the superblock of the p-th page of the unit: the set bits of pagemask, in order */
	bool		keyed;			/* explicit TIDs: blocks[p] is the heap block of the p-th page, koff[r] the offset of row r (pagemask, page, maxoff, sparse unused) */
	const BlockNumber *blocks;	/* [npages] ascending; NULL until sift_cunit_keyed_load (a scan that needs no TID never decodes the directory) */
	const uint16 *koff;			/* [nrows] */
	const uint8 *kdir;			/* the packed directory (keyed) */
	uint32		kwidths;		/* bit widths of the block distances, the rows of a page and the offsets */
	uint32		kblk0;			/* the first block */
	uint32		kblk1;			/* the last block */
	const uint32 *rowstart;		/* npages + 1 (unaligned access is avoided by the parser copying when needed) */
	const uint16 *maxoff;
	const uint8 *sparse;
	const uint32 *chunk_off;
	const uint32 *chunk_len;
} SiftCUnitView;

_Static_assert(SIFT_SB_PAGES <= 256, "SiftCUnitView.page holds page numbers in a byte");

/* false when inconsistent (the caller raises ERRCODE_INDEX_CORRUPTED); rec must stay valid and be 8-byte aligned */
extern bool sift_cunit_parse(const uint8 *rec, uint32 len, const SiftColSchema *schema, SiftCUnitView *v);
extern uint32 sift_cunit_head_need(const uint8 *rec, uint32 avail, uint32 len, int ncols);

/* Decodes the TID directory of a key-cut unit parsed by sift_cunit_parse_head (errors on a damaged one); a no-op when it is decoded or the unit is not keyed */
extern void sift_cunit_keyed_load(const SiftCUnitView *v);

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
extern uint32 sift_cunit_find_rows(const SiftCUnitView *v, uint32 sb, const SiftKey *keys, uint32 n, uint32 *rows);
extern void sift_cunit_row_keys(const SiftCUnitView *v, uint32 sb, const uint32 *ords, uint32 n, SiftKey *out);
/* Pages: index p in 0..npages-1 -> heap block sb * 256 + page, its row range and the 512-bit mask of its offsets */
extern BlockNumber sift_cunit_page_block(const SiftCUnitView *v, uint32 sb, uint32 p);
extern void sift_cunit_page_offmask(const SiftCUnitView *v, uint32 p, uint64 *offmask);
/* The same for a caller that visits the pages in order: the bitmaps of the sparse pages follow each other, soff is where page p's starts (the sum of sift_cunit_page_sparse_len of the pages before) */
extern uint32 sift_cunit_page_sparse_len(const SiftCUnitView *v, uint32 p);
extern void sift_cunit_page_offmask_at(const SiftCUnitView *v, uint32 p, uint32 soff, uint64 *offmask);
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
