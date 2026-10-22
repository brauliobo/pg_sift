/*
 * col/seg_int.h - shared between the files of the column segment writer and reader (col/seg.c, col/seg_write.c, col/seg_maint.c).
 */
#ifndef SIFT_COL_SEG_INT_H
#define SIFT_COL_SEG_INT_H

#include "core/host.h"

#include "col/preagg.h"
#include "col/seg.h"
#include "col/ver.h"
#include "core/live.h"

#define ULL(x)			((unsigned long long) (x))
#define ALIGN8(x)		TYPEALIGN(8, x)
#define DICTS_MAGIC		0x44444F43		/* "COLD" */
#define ZONES_MAGIC		0x5A4C4F43		/* "COLZ" */
#define PAGG_MAGIC		0x504C4F43		/* "COLP": the pre-aggregates, at the end of the ZONES section (docs/COL-PREAGG.md section 2) */
#define PAGG_ECUBE_MINOR	4			/* the column format minor that adds the element cubes: directory version 2 */
#define PAGG_SETS_MINOR		SIFT_COL_FORMAT_SETS	/* the column format minor that adds the small-set summaries: directory version 3 */
#define PAGG_SUPER_MINOR	SIFT_COL_FORMAT_SUPER	/* the column format minor that adds the superunit summaries: directory version 4 */
#define PAGG_RUNS_MINOR		SIFT_COL_FORMAT_RUNS	/* the column format minor that adds the sorted runs: directory version 5 */
#define PAGG_VERSION(minor)	((minor) >= PAGG_RUNS_MINOR ? 5 : (minor) >= PAGG_SUPER_MINOR ? 4 : (minor) >= PAGG_SETS_MINOR ? 3 : (minor) >= PAGG_ECUBE_MINOR ? 2 : 1)
#define SETS_MAGIC		0x544C4F43		/* "COLT": the small-set summaries, at the end of the pre-aggregates (docs/COL.md section 2.13) */
#define RUNS_MAGIC		0x524C4F43		/* "COLR": the sorted runs, after the small-set summaries (docs/COL.md section 2.14) */

#define VERS_MAGIC		0x564C4F43		/* "COLV": the version section, at the end of the DOCMAP section (docs/COL-TAM.md section 7) */

/* The version section: a header, a SiftColVerUnit per unit, then the vectors of the units that have any (xmin, then cmin) */
typedef struct VersHeader
{
	uint32		magic;
	uint32		version;
	uint32		nsb;
	uint32		flags;
	uint32		min_xid;		/* of the unfrozen rows, in the order of the server's xids */
	uint32		max_xid;
	uint64		blob_bytes;
} VersHeader;

#define VU_VEC		0x1			/* the unit has vectors (some row is not frozen) */
#define VU_FROZEN	0x2			/* some rows of such a unit are frozen */

typedef struct VersUnit
{
	uint32		xmin_lo;
	uint32		xmin_hi;
	uint32		cmin_hi;
	uint32		flags;
	uint64		off;			/* of the vectors, from the end of the unit directory */
	uint32		xlen;			/* bytes of the xmin vector; the cmin vector follows */
	uint32		clen;
} VersUnit;

StaticAssertDecl(sizeof(VersHeader) == 32 && sizeof(VersUnit) == 32, "the version section is made of 32 byte records");

#define corrupt(seg, ...) SIFT_SEG_CORRUPT((seg)->index, (seg)->entry.gen, __VA_ARGS__)

/* One leaf's zone arrays: min, max, sum (u64 each), nnull, nvals, flags (u32 each, padded to 8), set (STR) */
static inline Size
zone_block_size(const SiftColDef *def, uint32 nsb)
{
	return ALIGN8(nsb * (3 * sizeof(uint64) + 3 * sizeof(uint32))) + (def->store == SIFT_COLS_STR ? nsb * 4 * sizeof(uint64) : 0);
}

/*
 * The pre-aggregates, at the end of the ZONES section (docs/COL-PREAGG.md section 2): a header (magic, version, ncols, nsb, the offset of the
 * cube directory and its blob bytes, the offset of each INT leaf's sums), then per INT leaf the sums of all units as arrays, then the cube
 * directory (u64 offset of each unit's blob, nsb + 1 entries) and the blobs. Offsets are from the start of the header.
 * Version 2 (column format minor 4) adds two entries after the offsets of the sums, the offset of the element directory and its blob bytes,
 * and after the cubes that directory (8-aligned) and the element blobs, laid out like the cubes.
 * Version 3 (column format minor 10) adds two more, the offset of the small-set summaries and their bytes (0: none): a SetsHeader, a SetsLeaf per
 * leaf that has them, then per leaf a directory of nsb + 1 offsets and the blobs, laid out like the cubes (offsets from the start of the header).
 */
typedef struct PaggHeader
{
	uint32		magic;
	uint32		version;
	uint32		ncols;
	uint32		nsb;
	uint64		cubedir_off;			/* 0: no cube in any unit */
	uint64		cube_bytes;
	uint64		lagg_off[FLEXIBLE_ARRAY_MEMBER];	/* [ncols], 0: the leaf has none */
} PaggHeader;

#define PAGG_HDR_SIZE(ver, ncols)	ALIGN8(offsetof(PaggHeader, lagg_off) + ((ncols) + ((ver) > 1 ? 2 : 0) + ((ver) > 2 ? 2 : 0) + ((ver) > 3 ? 2 : 0) + ((ver) > 4 ? 2 : 0)) * sizeof(uint64))
#define PAGG_ECUBEDIR(h)		((h)->lagg_off[(h)->ncols])
#define PAGG_ECUBE_BYTES(h)		((h)->lagg_off[(h)->ncols + 1])
#define PAGG_SETS(h)			((h)->lagg_off[(h)->ncols + 2])
#define PAGG_SETS_BYTES(h)		((h)->lagg_off[(h)->ncols + 3])

#define SETS_EXACT		0x1
#define SETS_BLOOM		0x2
#define SETS_PRES		0x4

typedef struct SetsHeader
{
	uint32		magic;
	uint32		version;		/* 1 */
	uint32		nleaves;
	uint32		nsb;
} SetsHeader;

typedef struct SetsLeaf
{
	uint32		leaf;			/* ascending */
	uint32		flags;			/* SETS_EXACT, SETS_BLOOM: the kinds of blob some unit has (a hint: a test no filter can answer reads no blob of a leaf with only filters) */
	uint64		dir_off;		/* the leaf's directory, from the start of the pre-aggregates, 8-aligned */
	uint64		bytes;			/* the blobs after it */
} SetsLeaf;

StaticAssertDecl(sizeof(SetsHeader) == 16 && sizeof(SetsLeaf) == 24, "the small-set section is made of 16 and 24 byte records");

/* Version 4 (column format minor 12) adds two more entries: the offset of the superunit block and its bytes (0: the segment has none) */
#define PAGG_SUPERDIR(h)		((h)->lagg_off[(h)->ncols + 4])
#define PAGG_SUPER_BYTES(h)		((h)->lagg_off[(h)->ncols + 5])
/* Version 5 (column format minor 13) adds two more: the offset of the sorted runs and their bytes (0: none), the section laid out like the small-set summaries */
#define PAGG_RUNS(h)			((h)->lagg_off[(h)->ncols + 6])
#define PAGG_RUNS_BYTES(h)		((h)->lagg_off[(h)->ncols + 7])

/*
 * The superunit block (docs/COL-PREAGG.md section 10): the units of the segment in groups of `fan` consecutive ones (the last group may be shorter,
 * one unit has no summary). Offsets from the start of the block, all 8-aligned:
 *     SuperHeader, the page mask of every unit (4 x u64 each, the pages of its superblock that hold rows), then for each INT leaf in id order the
 *     SiftCSuperLeaf of every group (nsu x 64 bytes), then (cubedir_off, when any group has a merged cube) the directory of nsu + 1 offsets and the blobs.
 */
#define SUPER_MAGIC		0x53505543		/* "CUPS" */
typedef struct SuperHeader
{
	uint32		magic;
	uint32		fan;
	uint32		nsu;					/* groups: ceil(nsb / fan) */
	uint32		nleaf;					/* INT leaves of the schema */
	uint64		cubedir_off;			/* 0: no group has a cube */
	uint64		cube_bytes;
} SuperHeader;

StaticAssertDecl(sizeof(SuperHeader) == 32, "the superunit header is 32 bytes");

/* The leaves a superunit block summarises: the integer leaves (with their exact sums) and the text leaves (counts only). Floats are left out: their sum depends on the order of the additions */
static inline bool
super_leaf_ok(const SiftColDef *def)
{
	return def->store == SIFT_COLS_INT || def->store == SIFT_COLS_STR;
}

/* The section of the sorted runs has the shape of the small-set one: a header, a record per leaf (flags: RUNS_ORD, RUNS_IDENT: the kinds of run some unit has), a directory and the blobs per leaf */
#define RUNS_ORD		0x1
#define RUNS_IDENT		0x2
typedef SetsHeader RunsHeader;
typedef SetsLeaf RunsLeaf;

/* One INT leaf's sums: sum_lo, sum_hi, sumsq_lo, sumsq_hi (u64 each), flags (u32), all over the units */
static inline Size
lagg_block_size(uint32 nsb)
{
	return ALIGN8((Size) nsb * (4 * sizeof(uint64) + sizeof(uint32)));
}

typedef struct ZoneArrays
{
	const int64 *min;
	const int64 *max;
	const int64 *sum;
	const uint32 *nnull;
	const uint32 *nvals;
	const uint32 *flags;
	const uint64 (*set)[4];
	bool		any_ovf;		/* some unit flags SIFT_COLZ_OVERFLOW */
	bool		any_inl;		/* some unit flags SIFT_COLZ_INLINE: its min and max are prefix keys, not codes */
} ZoneArrays;

#define DICT_PARSE_AFTER	32	/* tests of a dictionary searched in the stream, over the queries of a backend, before it is parsed and cached */

/* A dictionary stored plain or for a lookup (DICT_PLAIN, DICT_LOOKUP): where its parts lie in the stream */
typedef struct DictLook
{
	SiftCDictLook look;
	SiftColSegment *seg;
	int			col;
	uint32		n;
	uint32		fixed;			/* the length of every string, 0: off[] says */
	uint32		nbytes;
	uint64		offs_at;		/* stream offsets: plain (form 0): u32 off[n + 1], the strings */
	uint64		bytes_at;
	uint64		boff_at;		/* the searched form (2): u32 boff[blocks + 1], the blocks of DICT_BLOCK strings, u32 code[n] in byte order */
	uint64		blk_at;
	uint64		blk_len;
	uint64		perm_at;
	uint32		blkno;			/* the block in blkraw (UINT32_MAX: none) */
	uint16		blkoff[DICT_BLOCK + 1];	/* where its strings start in blkraw */
	uint8		blkraw[DICT_BLOCK_RAW_MAX];
	uint8		blkz[DICT_BLOCK_RAW_MAX + DICT_BLOCK_HEAD];
	bool		searchable;		/* form 2: find and match read the stream */
	bool		blocked;		/* form 2: the strings are in blocks */
	uint32		reads;			/* strings read in place for sift_cseg_dict_lazy */
	SiftCDict	stub;			/* n and look only: what a test is given instead of the parsed dictionary */
	char		buf[SIFT_COL_DICT_MAX_ENTRY];	/* the string a bisection looks at */
} DictLook;

typedef struct SegPriv
{
	SiftLive	live;
	SiftCDict **dict;
	DictLook  **look;			/* [ncols] read on first use; a NULL entry with look_checked: the dictionary is not of that form */
	bool	   *look_checked;
	ZoneArrays **zones;
	uint8	   *unitbuf;
	Size		unitcap;
	PaggHeader *pagg;			/* the sidecar's header, read on first use */
	Size		pagg_end;		/* offset of the sidecar in the ZONES section */
	uint64	   *cubedir;		/* nsb + 1 */
	uint8	   *cubebuf;
	Size		cubecap;
	const SuperHeader *super;	/* the superunit block's header, read on first use; NULL with super_checked: there is none */
	bool		super_checked;
	uint64	   *supdir;			/* nsu + 1 */
	uint8	   *supbuf;
	Size		supcap;
	SiftCSuperLeaf **sleaf;		/* [ncols] the records of the INT leaf over the groups, read on first use */
	uint64	   *ecubedir;		/* the same for the element blobs */
	uint8	   *ecubebuf;
	Size		ecubecap;
	const uint64 **lagg;		/* [ncols] sums of the INT leaves, read on first use: slo, shi, qlo, qhi, then flags */
	SetsLeaf   *sets;			/* [ncols] the small-set record of each leaf (dir_off 0: none), read on first use */
	uint64    **setdir;			/* [ncols] its directory, once read */
	uint8	   *setbuf;
	Size		setcap;
	RunsLeaf   *runs;			/* [ncols] the sorted-run record of each leaf (dir_off 0: none), read on first use */
	uint64    **rundir;			/* [ncols] its directory, once read */
	Size		vers_at;		/* offset of the version section in the DOCMAP section; vers_len 0: there is none */
	Size		vers_len;
	VersUnit   *vu;				/* its directory of units, read on first use */
	uint64		vblob_at;		/* stream offset of the vectors */
	uint64		vblob_len;
	uint32		vmin;
	uint32		vmax;
} SegPriv;

#define PRIV(seg)	((SegPriv *) (seg)->priv)

/* Read lazily by the reader, used by the VACUUM and verification passes too */
extern bool unit_dirty(SiftColSegment *seg, uint32 u);
extern const ZoneArrays *zone_arrays(SiftColSegment *seg, int col);
extern const VersUnit *vers_units(SiftColSegment *seg);

/* The backend's cache of parsed dictionaries (col/dictcache.c): the dictionary of leaf col of seg, from the cache, else made by load into the memory context it is given */
typedef SiftCDict *(*SiftCDictLoad) (SiftColSegment *seg, int col, MemoryContext mcxt);
extern SiftCDict *sift_cseg_dict_cached(SiftColSegment *seg, int col, SiftCDictLoad load);
/* The same without loading: NULL when the cache does not hold it */
extern SiftCDict *sift_cseg_dict_peek(SiftColSegment *seg, int col);
/* Counts one more test of the dictionary of leaf col that was searched in the stream, and says how many the backend made */
extern uint32 sift_cseg_dict_asked(SiftColSegment *seg, int col);

#endif
