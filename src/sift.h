/*
 * sift.h - shared constants, keys, options, GUCs and catalog lookups.
 *
 * Every sift .c file includes "sift_host.h" (PostgreSQL files: "postgres.h" first) and then this header.
 * See DESIGN.md section 0 for the data model and invariants.
 */
#ifndef SIFT_H
#define SIFT_H

#include "sift_host.h"

#define SIFT_MAGIC				0x53494654	/* "SIFT" */
#define SIFT_FORMAT_MAJOR		1
/* 1.1: term blobs may end with a position stream; indexes of 1.0 are read as before */
#define SIFT_FORMAT_MINOR		1

/* Heap geometry: superblock (SB) = 256 pages, scoring block = 16 pages */
#define SIFT_SB_SHIFT			8
#define SIFT_SB_PAGES			(1 << SIFT_SB_SHIFT)
#define SIFT_BLK_SHIFT			4
#define SIFT_BLK_PAGES			(1 << SIFT_BLK_SHIFT)
#define SIFT_BLKS_PER_SB		(SIFT_SB_PAGES / SIFT_BLK_PAGES)
#define SIFT_MAX_OFFSET			MaxHeapTuplesPerPage
#define SIFT_PAGEMASK_WORDS		(SIFT_SB_PAGES / 64)
#define SIFT_OFFMASK_WORDS		8
#define SIFT_SIMD_ALIGN			64

/* Limits (DESIGN.md (k)) */
#define SIFT_MAX_TERM_BYTES		255
#define SIFT_MAX_TF				255
#define SIFT_MAX_DL				PG_UINT16_MAX
#define SIFT_MAX_SEGMENTS		128
#define SIFT_MAX_LEAVES			256
#define SIFT_MAX_DEPTH			32
#define SIFT_MAX_PHRASE			128
#define SIFT_LANES				8

StaticAssertDecl(BLCKSZ == 8192, "sift requires BLCKSZ 8192");
StaticAssertDecl(MaxHeapTuplesPerPage <= SIFT_OFFMASK_WORDS * 64,
				 "heap offsets must fit the 512-bit offset mask");

/* Document key: heap block << 16 | offset (48 bits used); sorts in ctid order */
typedef uint64 SiftKey;

static inline SiftKey
sift_key_from_tid(ItemPointer tid)
{
	return ((uint64) ItemPointerGetBlockNumberNoCheck(tid) << 16) |
		ItemPointerGetOffsetNumberNoCheck(tid);
}

static inline void
sift_key_to_tid(SiftKey key, ItemPointer tid)
{
	ItemPointerSet(tid, (BlockNumber) (key >> 16), (OffsetNumber) (key & 0xFFFF));
}

static inline BlockNumber
sift_key_block(SiftKey key)
{
	return (BlockNumber) (key >> 16);
}

static inline uint32
sift_block_sb(BlockNumber blk)
{
	return blk >> SIFT_SB_SHIFT;
}

/* Index reloptions (amoptions); strings are offsets from the struct start */
typedef struct SiftOptions
{
	int32		vl_len_;
	double		k1;
	double		b;
	int			stemmer;		/* text search dictionary name, "" = none */
	bool		case_folding;
	bool		accent_folding;
	int			mutable_limit;	/* kB; seal the active mutable segment above it */
	int			vacuum_segments; /* directory entries VACUUM merges towards (maintenance only, not part of the format) */
	int			merge_fanin;	/* segments of a tier that trigger a merge (read at merge time, not part of the format) */
	int			build_segments; /* 0 = one per build participant */
	int			prefix_index;	/* longest word prefix (characters) indexed as a term, 0 = none */
	int			fields;			/* comma separated field names, "" = none */
	int			analyzer;		/* SIFT_ANALYZER_* */
	bool		positions;		/* CREATE INDEX stores word positions, so phrases are verified from the index (DESIGN.md (g)) */
} SiftOptions;

/* analyzer: keyword indexes the whole value as one term (DESIGN.md (l)) */
#define SIFT_ANALYZER_TEXT		0
#define SIFT_ANALYZER_KEYWORD	1

/*
 * prefix_index = n also indexes the first 1..n characters of every word as the term SEP prefix, so
 * a prefix query of up to n characters is one term lookup instead of an expansion (DESIGN.md (h)).
 * Synthetic terms (prefixes, field words) start with a control byte, which no word does: they sort
 * before every word and no expansion reaches them.
 */
#define SIFT_PREFIX_SEP		'\x1E'
#define SIFT_MAX_PREFIX_INDEX	15
#define SIFT_SYNTHETIC_TERM(t)	((unsigned char) *(t) < ' ')

/*
 * fields = 'a,b' names the first sections of the indexed text, which is split at SIFT_FIELD_SEP
 * (a text without one is a single section).
 * Every section is searched by unqualified queries (they see the plain concatenation); the words of
 * a named section are also terms of their own, the byte SIFT_FIELD_BASE + k - 1 then the word
 * (k = 1.., prefix terms: that byte, SIFT_PREFIX_SEP, the prefix), which a query reaches as a:word.
 */
#define SIFT_FIELD_SEP		'\x1D'
#define SIFT_FIELD_BASE		0x10
#define SIFT_MAX_FIELDS		14
#define SIFT_FIELD_BYTE(k)	((char) (SIFT_FIELD_BASE + (k) - 1))
#define SIFT_FIELDS_BYTES	128
/* is the byte c the first of a term of one of the first n fields */
#define SIFT_FIELD_TERM(c, n)	((unsigned char) (c) >= SIFT_FIELD_BASE && (unsigned char) (c) < SIFT_FIELD_BASE + (n))

#define SIFT_DEFAULT_K1				1.2
#define SIFT_DEFAULT_B				0.75
#define SIFT_DEFAULT_MUTABLE_LIMIT	4096
#define SIFT_DEFAULT_VACUUM_SEGMENTS	2

/* Returns the index options with defaults filled in; never NULL */
extern const SiftOptions *sift_index_options(Relation index);
extern const char *sift_options_stemmer(const SiftOptions *opts);
extern const char *sift_options_fields(const SiftOptions *opts);

/* GUCs (defined in sift_handler.c, except sift.simd in sift_cpu.c) */
extern PGDLLIMPORT int sift_max_expansions;
extern PGDLLIMPORT bool sift_enable_scan;
extern PGDLLIMPORT bool sift_enable_count;
extern PGDLLIMPORT bool sift_parallel_scan;
extern PGDLLIMPORT bool sift_async_maintenance;
extern PGDLLIMPORT int sift_maintenance_naptime;
extern PGDLLIMPORT int sift_maintenance_cost_delay;
extern PGDLLIMPORT bool sift_prefetch;

/*
 * Catalog OIDs of extension objects, cached per backend and reset by syscache
 * invalidation. Fields are InvalidOid when the extension is not installed in
 * the current database.
 */
typedef struct SiftCatalog
{
	Oid			am;
	Oid			match_op;		/* text @@> text */
	Oid			match_func;		/* sift.match(text, text) */
	Oid			match_idx_func; /* sift.match_idx(text, text, oid) */
	Oid			score_func;		/* sift.score(tid) */
	Oid			max_score_func; /* sift.max_score(tid) */
} SiftCatalog;

extern const SiftCatalog *sift_catalog(void);

extern void _PG_init(void);

#ifdef SIFT_TESTING
extern void sift_leak_test_init(void);
#endif

#endif							/* SIFT_H */
