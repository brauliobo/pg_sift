/*
 * granule_fixture.h - real encoded granules for the unit tests of the decoders and filters (granule.mk).
 *
 * granule_fixture_build writes, on the setup thread, a part of ClickBench-shaped columns (URL-like strings, narrow and wide integers, doubles, nullable and sparse columns, exceptions, arrays)
 * through the real writer (sift_granule_encode and the encoders under it) in FX_ROWS-row granules, each under one of the FX_CFGS configurations of tier, wrapper and part codecs. What a
 * reader gets is what it gets from a part: the record, its directory entry, the SiftGranuleView with the part's dictionaries (SiftPartDicts: strings, the FSST table, the zstd DDict), all
 * immutable once built and shared by every worker. fx->g[g].ref[c] is the decode of the column by the ordinary (backend path) functions on the setup thread: the answer a worker must match
 * bit for bit.
 *
 * Synthesized, not written by the part writer: the SiftPartDicts (the writer stores them in SEC_DICTS and sift_part_dicts reads them back; here the encoder's own dictionary, table and zstd
 * dictionary are put in the same structs), and the TIDs (a heap of 60 rows per block).
 */
#ifndef SIFT_UNIT_GRANULE_FIXTURE_H
#define SIFT_UNIT_GRANULE_FIXTURE_H

#include "postgres.h"

#include "col/col.h"
#include "col/granule/granule.h"

#define FX_ROWS			2048
#define FX_SEL_MAX		256		/* rows of the ascending selection of a granule: below SIFT_POINT_ROWS_MAX */
#define FX_CFGS			5
#define FX_MAX_COLS		64

/* How the writer is set for a granule: the tier and wrapper of its chunks, and whether the columns that have a part codec use it */
typedef struct FxCfg
{
	const char *name;
	SiftCompressTier tier;
	int			alg;			/* SIFT_CC_ALG_LZ4 or SIFT_CC_ALG_ZSTD */
	bool		part_codecs;
} FxCfg;

typedef enum FxPart
{
	FX_PART_NONE,
	FX_PART_DICT,				/* the strings of the whole column as the dictionary of the part (SIFT_ENC_PART_DICT) */
	FX_PART_FSST,				/* a table of symbols trained on the whole column */
	FX_PART_ZSTD				/* a zstd dictionary trained on the whole column (a block is decoded with its ZSTD_DDict) */
} FxPart;

typedef struct FxColSpec
{
	const char *name;
	SiftCellKind kind;
	int			shape;			/* the generator of the values (granule_fixture.c) */
	uint8		hint;			/* SiftChunkHint */
	bool		key_leaf;
	uint8		scale;
	int			group_of;		/* 1 + the column of the lengths whose elements these are, else 0 */
	FxPart		part;
} FxColSpec;

/* The decode of one column of one granule by the backend-path functions: what a worker's result must equal */
typedef struct FxRef
{
	int			n;				/* values of the chunk: the rows, or the elements of the arrays of the column it is the elements of */
	bool	   *isnull;
	uint8	   *tag;			/* SIFT_COLTAG_* of the rows whose value the column could not hold */
	int64	   *i;				/* by kind */
	double	   *f;
	SiftStrCell *s;				/* the bytes are copies */
	bool		has_codes;		/* sift_granule_str_codes holds for the chunk: a dictionary of the granule or of the part */
	uint32	   *codes;
} FxRef;

typedef struct FxGranule
{
	char	   *rec;
	uint32		len;
	SiftGranuleDesc desc;
	SiftGranuleView view;		/* with the dictionaries of the part */
	const FxCfg *cfg;
	uint16		nsel;			/* ascending rows the point decode reads */
	uint16		sel[FX_SEL_MAX];
	FxRef	   *ref;			/* ncols */
} FxGranule;

typedef struct GranuleFixture
{
	int			ngranules;
	int			ncols;
	const FxColSpec *cols;
	SiftColDef *defs;			/* ncols: the store of each column, for sift_f2_decode_view */
	SiftPartDicts dicts;
	FxGranule  *g;
	uint64		enc_seen;		/* bit SiftChunkEnc of every encoding some chunk has */
	uint32		flags_seen;		/* the SIFT_CHUNK_* flags of some chunk (bits 0..5), and (SIFT_ENC_* modifiers >> 8) << 16 */
	uint64		wrapped_seen;	/* bit SiftChunkEnc of the encodings some chunk has that the wrapper (SIFT_CHUNK_COMPRESSED) holds */
	void	   *cdicts[FX_MAX_COLS];	/* the zstd dictionaries the granules were written with (the DDicts are in dicts) */
} GranuleFixture;

extern const FxCfg fx_cfgs[FX_CFGS];

/* The fixture of ngranules granules of FX_ROWS rows, the same for the same seed; call after the CPU kernels are set (granule_fixture_init) */
extern void granule_fixture_init(void);
extern GranuleFixture *granule_fixture_build(int ngranules, uint32 seed);
/* Frees the zstd handles of the dictionaries (the rest is palloc'd) */
extern void granule_fixture_free(GranuleFixture *fx);
extern int	granule_fixture_col(const GranuleFixture *fx, const char *name);

/* A Relation for sift_granule_open on the setup thread, whose damage error names no real index */
extern Relation granule_fixture_rel(void);

#endif							/* SIFT_UNIT_GRANULE_FIXTURE_H */
