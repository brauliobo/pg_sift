/*
 * col/f2/defaults.h - SEC_SCHEMA_DEFAULTS (0x0305): the defaults a sift_table part was written under, so that ALTER TABLE ADD COLUMN ... DEFAULT <const> and DROP COLUMN rewrite no part.
 *
 * A part that has attributes with a fast default (attmissingval) or dropped attributes at write time carries the section and SIFT_SCHEMA_EXT_DEFAULTS in its SEC_SCHEMA, with the stamp of the
 * section. The catalog stays what a reader asks first; the section is the stamped copy that a reader without the relcache, a compaction that checks the catalog before it rewrites old parts under
 * the current schema, and sift.check read. Portable: core/host.h and the C library only.
 *
 * Layout (little endian, every part 8-byte aligned): header (32 B), entries (16 B each, ascending attnum), dropped attnums (int16, ascending, padded), the values (padded).
 */
#ifndef SIFT_F2_DEFAULTS_H
#define SIFT_F2_DEFAULTS_H

#include "core/host.h"

#define SEC_SCHEMA_DEFAULTS_TAG		0x0305
#define SIFT_DEFAULTS_MAGIC			0x4C464453U		/* "SDFL" */
#define SIFT_DEFAULTS_VERSION		1
#define SIFT_DEFAULTS_MAX_ATTNUM	1664
#define SIFT_DEFAULTS_MAX_VALUE		(16U << 20)

typedef enum SiftDefaultKind
{
	SIFT_DEFAULT_INT = 1,
	SIFT_DEFAULT_F8 = 2,
	SIFT_DEFAULT_STR = 3
} SiftDefaultKind;

#define SIFT_DEFAULT_FLAG_NULL		UINT8_C(1)

typedef struct SiftDefaultsHdr
{
	uint32		magic;
	uint16		version;
	uint16		flags;			/* 0 */
	uint32		stamp;			/* CRC32C of everything after the header: what SEC_SCHEMA names with SIFT_SCHEMA_EXT_DEFAULTS */
	uint32		nentries;
	uint32		ndropped;
	uint32		values_len;
	uint64		reserved;		/* 0 */
} SiftDefaultsHdr;

typedef struct SiftDefaultsEnt
{
	int16		attnum;
	uint8		kind;			/* SiftDefaultKind */
	uint8		flags;			/* SIFT_DEFAULT_FLAG_NULL: the column was added with no default (len 0) */
	uint32		off;			/* in the values, ascending and contiguous */
	uint32		len;			/* INT and F8: 8; STR: the image of the datum as a cell holds it */
	uint32		reserved;
} SiftDefaultsEnt;

StaticAssertDecl(sizeof(SiftDefaultsHdr) == 32, "SiftDefaultsHdr must be 32 bytes");
StaticAssertDecl(sizeof(SiftDefaultsEnt) == 16, "SiftDefaultsEnt must be 16 bytes");

typedef enum SiftDefaultsStatus
{
	SIFT_DEFAULTS_OK = 0,
	SIFT_DEFAULTS_SHORT,		/* fewer bytes than the header says */
	SIFT_DEFAULTS_TRAILING,		/* more bytes than the header says */
	SIFT_DEFAULTS_MAGIC_BAD,
	SIFT_DEFAULTS_VERSION_BAD,
	SIFT_DEFAULTS_FLAGS_BAD,	/* a flag or a reserved field that is not zero */
	SIFT_DEFAULTS_COUNT_BAD,
	SIFT_DEFAULTS_CRC_BAD,
	SIFT_DEFAULTS_ATTNUM_BAD,	/* out of range */
	SIFT_DEFAULTS_ORDER_BAD,	/* not strictly ascending, or an offset that is not the next one */
	SIFT_DEFAULTS_KIND_BAD,
	SIFT_DEFAULTS_VALUE_BAD,	/* a length that its kind rules out */
	SIFT_DEFAULTS_OVERLAP,		/* an attribute both with a default and dropped */
	SIFT_DEFAULTS_PADDING_BAD,
	SIFT_DEFAULTS_TOO_BIG,
	SIFT_DEFAULTS_MISMATCH		/* sift_defaults_check: the catalog disagrees with the part */
} SiftDefaultsStatus;

typedef struct SiftDefaultVal
{
	int16		attnum;
	uint8		kind;
	bool		isnull;
	int64		i;
	double		f;
	const uint8 *s;				/* STR: a copy the set of defaults owns */
	uint32		slen;
} SiftDefaultVal;

typedef struct SiftSchemaDefaults
{
	int			n;
	int			cap;
	SiftDefaultVal *e;			/* ascending attnum */
	int			ndropped;
	int			dropcap;
	int16	   *dropped;		/* ascending */
	uint32		stamp;			/* set by sift_defaults_encode and sift_defaults_decode */
} SiftSchemaDefaults;

extern uint32 sift_crc32c(const void *data, Size len);

extern SiftSchemaDefaults *sift_defaults_new(void);
extern void sift_defaults_free(SiftSchemaDefaults *d);

/* Attributes are added in ascending order, each once; a string value is copied */
extern SiftDefaultsStatus sift_defaults_add(SiftSchemaDefaults *d, const SiftDefaultVal *v);
extern SiftDefaultsStatus sift_defaults_add_dropped(SiftSchemaDefaults *d, int16 attnum);

/* The section as bytes (palloc'd); sets d->stamp */
extern SiftDefaultsStatus sift_defaults_encode(SiftSchemaDefaults *d, uint8 **out, uint64 *len);

/* Checks every byte of the section and decodes it; *out is NULL unless OK */
extern SiftDefaultsStatus sift_defaults_decode(const uint8 *buf, uint64 len, SiftSchemaDefaults **out);

extern const SiftDefaultVal *sift_defaults_find(const SiftSchemaDefaults *d, int16 attnum);
extern bool sift_defaults_is_dropped(const SiftSchemaDefaults *d, int16 attnum);
extern bool sift_defaults_val_equal(const SiftDefaultVal *a, const SiftDefaultVal *b);

/* The knowledge of the newer part over that of the older: its values win, what either dropped is dropped; MISMATCH when the newer part has a default for what the older dropped */
extern SiftDefaultsStatus sift_defaults_merge(const SiftSchemaDefaults *newer, const SiftSchemaDefaults *older, SiftSchemaDefaults **out);

/* What the part holds against what the catalog says today (both as SiftSchemaDefaults): MISMATCH with the first attribute whose default differs or that the part dropped and the catalog did not. A default the catalog no longer has (a rewrite clears them) or an attribute the catalog dropped since is fine */
extern SiftDefaultsStatus sift_defaults_check(const SiftSchemaDefaults *part, const SiftSchemaDefaults *catalog, int16 *bad_attnum);

extern const char *sift_defaults_status_text(SiftDefaultsStatus st);

/* The chunk of the part that holds each current column: phys[c] is the position in part_attnums of cur_attnums[c], or -1 when the part has none (it was written before the column was added).
 * Both lists ascend, as the columns of a table do; the chunks of the part that no current column names are those of dropped columns and are never read */
extern void sift_defaults_map_columns(const int16 *part_attnums, int npart, const int16 *cur_attnums, int ncur, int16 *phys);

/* The chunk a reader synthesizes for a column an old part does not have: all null, or one constant */
typedef enum SiftSynthEnc
{
	SIFT_SYNTH_ALL_NULL = 0,
	SIFT_SYNTH_CONST = 1
} SiftSynthEnc;

typedef struct SiftSynth
{
	SiftSynthEnc enc;
	uint8		kind;
	int64		i;
	double		f;
	const uint8 *s;
	uint32		slen;
} SiftSynth;

/* v NULL: the attribute has no default */
extern void sift_defaults_synth(const SiftDefaultVal *v, SiftSynth *out);

/* nrows values of the chunk: isnull always, i for INT, f for F8 (NULL arrays are skipped) */
extern void sift_synth_fill(const SiftSynth *sy, uint32 nrows, int64 *i, double *f, bool *isnull);

#endif							/* SIFT_F2_DEFAULTS_H */
