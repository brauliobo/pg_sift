/*
 * sift_col_gpu.h - what the Vulkan path of the aggregate scan (docs/GPU.md) needs of the column engine: the identity of a stored unit, its
 * chunk bytes, and the reading of a codec blob as the fields a kernel decodes.
 *
 * A chunk of the codec (docs/COL-CODEC.md) that a kernel can read is a constant, a vertical bit-packed array (FORBIT, width 1..32) or an
 * array of 1, 2 or 4 byte fields (FORBYTE), under an optional SCALE, or a DICT of those (sorted values, codes in such an array). Its value i
 * is base + mul * field(i); of a dictionary chunk, values[base + mul * field(i)]. A compressed wrapper (LZ4 / zstd) around any of
 * these is undone on the host. Every other shape (DELTA, RLE, ALP, floats) is not read: the unit is left to the CPU.
 */
#ifndef SIFT_COL_GPU_H
#define SIFT_COL_GPU_H

#include "sift_col.h"

/* ---- the engine: units (sift_col_scan.c) ---- */

typedef struct SiftColGpuUnit
{
	uint64		store;			/* database and relation file of the index: what the chunk bytes belong to */
	uint64		gen;			/* the segment generation: a segment is immutable, a new generation is new storage */
	uint32		unit;			/* the unit's number in the segment */
	uint32		nrows;
} SiftColGpuUnit;

/*
 * The identity of the unit if its chunks may stand for the rows a query sees: stored in an immutable segment, no dead row, every page of it
 * all-visible to the snapshot (sift_col_unit_exact). False for any other unit (write buffer, delta batch, dirty page): the CPU reads it.
 */
extern bool sift_col_unit_gpu(const SiftColUnit *unit, SiftColGpuUnit *g);

typedef struct SiftColGpuChunk
{
	const uint8 *blob;			/* the codec blob, 8-aligned, valid until the next chunk read of the source; NULL for a constant chunk */
	uint32		len;
	uint32		n;
	bool		is_const;		/* the chunk stores no bytes: every row has cval */
	int64		cval;
} SiftColGpuChunk;

/*
 * The codec blob of leaf col in the unit, and false when the kernel cannot read the leaf: some value is NULL or overflowed (the zone says
 * so, no chunk is read), or the chunk is not a codec blob (an inline text leaf). Reads the chunk's bytes on first use.
 */
extern bool sift_col_unit_gpu_chunk(const SiftColUnit *unit, int col, SiftColGpuChunk *c);

/* The zone says the leaf has no NULL and no overflowed value in the unit (a count of the leaf is the count of the rows); no chunk is read */
extern bool sift_col_unit_gpu_dense(const SiftColUnit *unit, int col);

/* The dictionary of a text leaf in the unit's segment (the one its codes are of) */
extern void sift_col_unit_gpu_dict(const SiftColUnit *unit, int col, SiftColDict *dict);

/* ---- the codec blob (sift_col_gpu.c) ---- */

#define SIFT_CGL_CONST	0
#define SIFT_CGL_BIT	1
#define SIFT_CGL_BYTE	2

/*
 * A leaf the kernel decodes: value = base + mul * field; the field words are the nwords 32-bit words at byte offset woff of the chunk blob,
 * or of wbuf when the node sat under a compressed wrapper (LZ4 / zstd): then the plan owns the decompressed bytes.
 */
typedef struct SiftCGpuLeaf
{
	uint8		kind;
	uint8		W;				/* bits (BIT) or bytes (BYTE) of a field; 0 for a constant */
	int64		base;
	int64		mul;			/* >= 1 */
	uint32		woff;
	uint32		nwords;
	const uint8 *wbuf;			/* palloc'd, 8-aligned; NULL: the chunk blob */
} SiftCGpuLeaf;

#define SIFT_CGPU_DICT_MAX	4096	/* entries of a dictionary chunk the planner reads: a larger one is left to the CPU */

typedef struct SiftCGpuPlan
{
	SiftCGpuLeaf leaf;			/* the values, or the codes of a dictionary chunk */
	uint32		n;
	int64		vmin;			/* the least and greatest value */
	int64		vmax;
	uint32		d;				/* dictionary chunk: its entries, ascending (0: not a dictionary chunk) */
	int64	   *dval;			/* palloc'd */
} SiftCGpuPlan;

/* Reads the blob (len bytes); false when the kernel cannot decode it or it is malformed */
extern bool sift_cgpu_plan(const uint8 *blob, uint32 len, SiftCGpuPlan *p);
/* The plan of a chunk of n rows that all have value v (stored as no bytes at all) */
extern void sift_cgpu_plan_const(int64 v, uint32 n, SiftCGpuPlan *p);

/* The largest value of a field of the leaf (2^bits - 1, 0 for a constant) */
extern uint32 sift_cgpu_fmax(const SiftCGpuLeaf *lf);
/* The largest field value some row of the chunk holds, from its greatest value (the key of a group: its field is the group's slot) */
extern uint32 sift_cgpu_fhi(const SiftCGpuPlan *p);
/* The value of rows whose field is f; false for a field outside the dictionary */
extern bool sift_cgpu_value(const SiftCGpuPlan *p, uint32 f, int64 *v);

typedef enum SiftCGpuRange
{
	SIFT_CGR_NONE,				/* no row of the chunk has a value in [lo, hi] */
	SIFT_CGR_ALL,				/* every row has */
	SIFT_CGR_RANGE				/* the rows whose field f has fl <= f <= fh */
} SiftCGpuRange;

/* The fields whose values lie in [lo, hi] (inclusive) */
extern SiftCGpuRange sift_cgpu_range(const SiftCGpuPlan *p, int64 lo, int64 hi, uint32 *fl, uint32 *fh);

#endif							/* SIFT_COL_GPU_H */
