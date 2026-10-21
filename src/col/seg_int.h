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
#define PAGG_VERSION(minor)	((minor) >= PAGG_ECUBE_MINOR ? 2 : 1)

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

#define PAGG_HDR_SIZE(ver, ncols)	ALIGN8(offsetof(PaggHeader, lagg_off) + ((ncols) + ((ver) > 1 ? 2 : 0)) * sizeof(uint64))
#define PAGG_ECUBEDIR(h)		((h)->lagg_off[(h)->ncols])
#define PAGG_ECUBE_BYTES(h)		((h)->lagg_off[(h)->ncols + 1])

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

typedef struct SegPriv
{
	SiftLive	live;
	SiftCDict **dict;
	ZoneArrays **zones;
	uint8	   *unitbuf;
	Size		unitcap;
	PaggHeader *pagg;			/* the sidecar's header, read on first use */
	Size		pagg_end;		/* offset of the sidecar in the ZONES section */
	uint64	   *cubedir;		/* nsb + 1 */
	uint8	   *cubebuf;
	Size		cubecap;
	uint64	   *ecubedir;		/* the same for the element blobs */
	uint8	   *ecubebuf;
	Size		ecubecap;
	const uint64 **lagg;		/* [ncols] sums of the INT leaves, read on first use: slo, shi, qlo, qhi, then flags */
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

#endif
