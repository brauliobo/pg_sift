/*
 * col/f2/preagg_v2.h - SEC_PREAGG version 2: the pre-aggregates of the granules of a part, kept as cubes.
 *
 * A cube groups the rows of every granule by 0 to 4 dimensions (an integer, or the code of a string in the dictionary of the part; a NULL key is a group of its own) and keeps, for each group, its
 * row count, up to 8 measures (the exact count, sum, least and greatest of an integer or a scaled numeric; the compensated sum, least and greatest of a float with the flags of its NaN and its
 * infinities) and, for up to 4 columns, the exact set of their distinct values. The section holds up to 8 cubes: the one of no dimension, the one of all the dimensions the part is read by, and
 * one for each of them alone, so a query that groups by a subset of the dimensions or tests one of them finds a cube to add up. A granule with more groups than a cube may hold, or whose
 * columns carry a value the cube cannot keep (an exception), has no cells in that cube (cell_first[g] == cell_first[g + 1]): the reader scans that granule and no other.
 *
 * Layout: SiftPreaggSecHdr, SiftPreaggCubeDesc[ncubes] at cubes_off, then for each cube uint32 cell_first[ngranules + 1] at gran_dir_off and the cells at cells_off, each cell a
 * SiftPreaggGroupHdr, SiftPreaggMeasure[nmeasures] and SiftPreaggDistRef[ndistinct]; the sets of distinct values are one stream at dist_off. Every offset is from the start of the section.
 */
#ifndef SIFT_PREAGG_V2_H
#define SIFT_PREAGG_V2_H

#include "core/host.h"

#include "col/granule/granule.h"

#define SIFT_PREAGG_MAGIC		UINT32_C(0x32474150)	/* "PAG2" */
#define SIFT_PREAGG_VERSION		2
#define SIFT_PREAGG_MAX_CUBES	8
#define SIFT_PREAGG_MAX_DIMS	4
#define SIFT_PREAGG_MAX_MEAS	8
#define SIFT_PREAGG_MAX_DIST	4

#define SIFT_PAG_HAS_FLOAT		(UINT32_C(1) << 0)
#define SIFT_PAG_HAS_DISTINCT	(UINT32_C(1) << 1)
#define SIFT_PAG_HAS_MULTIKEY	(UINT32_C(1) << 2)

typedef struct SiftPreaggSecHdr
{
	uint32		magic;
	uint16		version;
	uint16		ncubes;
	uint32		ngranules;
	uint32		flags;			/* SIFT_PAG_* */
	uint64		cubes_off;		/* SiftPreaggCubeDesc[ncubes], 8-byte aligned */
	uint64		dist_off;		/* the stream of the sets of distinct values (0: none) */
} SiftPreaggSecHdr;

StaticAssertDecl(sizeof(SiftPreaggSecHdr) == 32, "SiftPreaggSecHdr must be 32 bytes");

typedef enum SiftPreaggDimKind
{
	SIFT_PDIM_INT64 = 1,		/* int2, int4, int8, bool, date, timestamp */
	SIFT_PDIM_DICT32 = 2		/* the code of a string in the dictionary of the part */
} SiftPreaggDimKind;

typedef enum SiftPreaggMeasKind
{
	SIFT_PMEAS_INT64 = 1,		/* exact 128-bit sum, int64 least and greatest */
	SIFT_PMEAS_NUM64 = 2,		/* a scaled numeric: the same, with the scale */
	SIFT_PMEAS_F4 = 3,			/* float4: a float8 compensated sum and the flags of NaN and infinities */
	SIFT_PMEAS_F8 = 4			/* float8: the same */
} SiftPreaggMeasKind;

#define SIFT_PCUBE_NULL_GROUPS	(UINT8_C(1) << 0)	/* some group of the cube has a NULL dimension */

typedef struct SiftPreaggCubeDesc
{
	uint8		ndims;			/* 0: the cube of all rows */
	uint8		nmeasures;
	uint8		ndistinct;
	uint8		flags;			/* SIFT_PCUBE_* */
	uint32		gran_dir_off;	/* uint32 cell_first[ngranules + 1] */
	uint64		cells_off;
	uint16		dim_leaf[SIFT_PREAGG_MAX_DIMS];
	uint8		dim_kind[SIFT_PREAGG_MAX_DIMS];
	uint8		meas_kind[SIFT_PREAGG_MAX_MEAS];
	uint16		meas_leaf[SIFT_PREAGG_MAX_MEAS];
	uint16		dist_leaf[SIFT_PREAGG_MAX_DIST];
	uint32		max_groups_per_gran;
} SiftPreaggCubeDesc;

StaticAssertDecl(sizeof(SiftPreaggCubeDesc) == 64, "SiftPreaggCubeDesc must be 64 bytes");

#define SIFT_PM_HAS_NAN			(UINT32_C(1) << 0)
#define SIFT_PM_HAS_PINF		(UINT32_C(1) << 1)
#define SIFT_PM_HAS_NINF		(UINT32_C(1) << 2)
#define SIFT_PM_SUM_OVERFLOW	(UINT32_C(1) << 3)
#define SIFT_PM_IS_FLOAT		(UINT32_C(1) << 4)
#define SIFT_PM_IS_NUM64		(UINT32_C(1) << 5)
#define SIFT_PM_HAS_NONNAN		(UINT32_C(1) << 6)	/* a float: min_f and max_f are those of the values that are not NaN, and there is one */

typedef struct SiftPreaggMeasure
{
	uint64		count_nonnull;
	uint32		flags;			/* SIFT_PM_* */
	uint32		scale_or_pad;	/* the scale of a numeric */
	union
	{
		struct
		{
			int64		sum_lo;
			int64		sum_hi;
			int64		min_i;
			int64		max_i;
		}			i;
		struct
		{
			double		sum_hi;
			double		sum_lo;
			double		min_f;
			double		max_f;
		}			f;
	}			u;
} SiftPreaggMeasure;

StaticAssertDecl(sizeof(SiftPreaggMeasure) == 48, "SiftPreaggMeasure must be 48 bytes");

#define SIFT_PGRP_DIST_OVERFLOW	(UINT8_C(1) << 0)	/* some set of distinct values of the group is SIFT_PDIST_OVERFLOW */

typedef struct SiftPreaggGroupHdr
{
	uint64		row_count;		/* count(*) of the group, rows with a NULL measure included */
	uint8		dim_null_mask;	/* bit d: dimension d is NULL */
	uint8		flags;			/* SIFT_PGRP_* */
	uint16		pad;
	uint32		pad2;
	int64		keys[SIFT_PREAGG_MAX_DIMS];	/* the unused and the NULL ones 0; a string is the zero-extended code */
} SiftPreaggGroupHdr;

StaticAssertDecl(sizeof(SiftPreaggGroupHdr) == 48, "SiftPreaggGroupHdr must be 48 bytes");

#define SIFT_PDIST_EMPTY		0
#define SIFT_PDIST_DICT_BM		1	/* a bitmap over the codes [0, n) of the dictionary of the part */
#define SIFT_PDIST_U32_LIST		2	/* the codes, ascending */
#define SIFT_PDIST_I64_LIST		3	/* the values, ascending */
#define SIFT_PDIST_OVERFLOW		4	/* more than the granule's budget: the reader scans the granule */

/* A signed 128-bit sum as its low and high words */
static inline void
sift_pag_sum_add(uint64 *lo, int64 *hi, int64 v)
{
	*hi += (int64) (*lo + (uint64) v < *lo) + (v < 0 ? -1 : 0);
	*lo += (uint64) v;
}

#define SIFT_PAG_DIST_LIST_MAX	64
#define SIFT_PAG_DIST_BM_BITS	32768

typedef struct SiftPreaggDistRef
{
	uint32		off;			/* from SiftPreaggSecHdr.dist_off */
	uint16		len;
	uint8		mode;			/* SIFT_PDIST_* */
	uint8		pad;
} SiftPreaggDistRef;

StaticAssertDecl(sizeof(SiftPreaggDistRef) == 8, "SiftPreaggDistRef must be 8 bytes");

static inline uint32
sift_pag_cell_size(const SiftPreaggCubeDesc *c)
{
	return sizeof(SiftPreaggGroupHdr) + c->nmeasures * sizeof(SiftPreaggMeasure) + c->ndistinct * sizeof(SiftPreaggDistRef);
}

static inline const SiftPreaggGroupHdr *
sift_pag_group(const SiftPreaggCubeDesc *c, const void *cells, uint32 i)
{
	return (const SiftPreaggGroupHdr *) ((const char *) cells + (uint64) i * sift_pag_cell_size(c));
}

static inline const SiftPreaggMeasure *
sift_pag_measure(const SiftPreaggCubeDesc *c, const SiftPreaggGroupHdr *grp, int m)
{
	return (const SiftPreaggMeasure *) ((const char *) grp + sizeof(*grp)) + m;
}

static inline const SiftPreaggDistRef *
sift_pag_dist(const SiftPreaggCubeDesc *c, const SiftPreaggGroupHdr *grp, int d)
{
	return (const SiftPreaggDistRef *) ((const char *) grp + sizeof(*grp) + c->nmeasures * sizeof(SiftPreaggMeasure)) + d;
}

/* ---- writing ---- */

/*
 * The cubes the user declared with preagg = 'cube(dimension: measure, ...; dimension ...)': the leaves of the dimensions and the measures (those of every entry together). A part that has a declaration
 * keeps a cube of no dimension, one for each declared dimension it can hold (an integer, or a string it has a dictionary of) and one of them all when their groups fit, each with the declared
 * measures (the numeric columns when none is declared) and no sets of distinct values.
 */
typedef struct SiftPreaggDecl
{
	int			ndims;
	int			dim[SIFT_PREAGG_MAX_DIMS];
	int			nmeas;
	int			meas[SIFT_PREAGG_MAX_MEAS];
} SiftPreaggDecl;

struct SiftColSchema;
/*
 * The option preagg read: NULL when it is valid, else why not. decl is what it declares (ndims 0: nothing, the cubes are chosen from the data), off whether it is 'off'. A column that is not a leaf of the
 * schema is an error when strict and left out otherwise; no schema: the syntax alone is checked.
 */
extern const char *sift_preagg_decl_parse(const char *opt, const struct SiftColSchema *schema, bool strict, SiftPreaggDecl *decl, bool *off);

/* The cubes a part is written with: the columns of the rows it holds (a declaration of the rows, else chosen from the data) */
typedef struct SiftPreaggPlan
{
	int			ncubes;
	SiftPreaggCubeDesc cube[SIFT_PREAGG_MAX_CUBES];
} SiftPreaggPlan;

/* False when the rows have no column a cube can hold */
extern bool sift_preagg_v2_plan(const SiftRows *rows, const SiftDictEnc *const *dicts, SiftPreaggPlan *plan);

typedef struct SiftPreaggBuild SiftPreaggBuild;

/* The cells and sets of all the granules may take SIFT_PAG_BUDGET_PERCENT of the bytes of the granules written so far and SIFT_PAG_BUDGET_SLACK more: a granule that would go over is left out of the cube it does not fit */
#define SIFT_PAG_BUDGET_PERCENT	4
#define SIFT_PAG_BUDGET_SLACK	65536

extern SiftPreaggBuild *sift_preagg_v2_begin(const SiftPreaggPlan *plan, uint32 ngranules);
/* The granule g: its columns (nrows rows), the dictionaries of the part per column and the bytes of the granules written so far, this one included. True when every cube holds it; false when one left it out (the caller marks SIFT_GDESC_PREAGG_SKIP) */
extern bool sift_preagg_v2_add_granule(SiftPreaggBuild *b, uint32 g, const SiftColIn *cols, int nrows, const SiftDictEnc *const *dicts, uint64 data_bytes);
/* The section, palloc'd in the context the build was begun in */
extern void *sift_preagg_v2_finish(SiftPreaggBuild *b, uint64 *len);

/* The bytes of a section from its parts: the cubes (their offsets are made here), first[k] the cell_first of cube k, the cells of each cube and the stream of the sets of distinct values */
struct StringInfoData;
extern void *sift_preagg_v2_assemble(int ncubes, const SiftPreaggCubeDesc *cube, uint32 ng, uint32 flags, uint32 *const *first, const struct StringInfoData *cells, const struct StringInfoData *dist,
									 uint64 *len);

/* ---- joining ---- */

struct SiftPart;
/* The section of the parts joined end to end, or NULL when their cubes are not the same */
extern void *sift_preagg_v2_join(Relation rel, struct SiftPart *const *src, int n, uint64 *len);

/* ---- reading ---- */

typedef struct SiftPreaggPart
{
	const struct SiftPart *part;
	SiftPreaggSecHdr hdr;
	SiftPreaggCubeDesc cube[SIFT_PREAGG_MAX_CUBES];
	uint64		sec_len;
} SiftPreaggPart;

/* The header and the cubes of the section of the part, validated against its size; NULL when the part has none */
extern SiftPreaggPart *sift_preagg_part_open(Relation rel, const struct SiftPart *part);

typedef struct SiftPreaggGran
{
	uint32		ncells;
	void	   *cells;			/* ncells * sift_pag_cell_size cells; ncells 0: the cube left the granule out */
} SiftPreaggGran;

extern void sift_preagg_gran_read(Relation rel, const SiftPreaggPart *pp, int cube, uint32 g, SiftPreaggGran *out);

/* The bytes of the set of distinct values a reference names */
extern uint8 *sift_preagg_dist_read(Relation rel, const SiftPreaggPart *pp, const SiftPreaggDistRef *ref);

/* The values of the set: the codes (U32_LIST, DICT_BM) or the integers (I64_LIST), ascending; at most cap of them, else cap + 1 */
extern uint32 sift_preagg_dist_values(const SiftPreaggDistRef *ref, const uint8 *bytes, int64 *out, uint32 cap);

#endif							/* SIFT_PREAGG_V2_H */
