/*
 * col/preagg/pagg_build_int.h - what the files that build the pre-aggregates of a unit share
 * the types of the builders and the statics one file calls from another
 */
#ifndef SIFT_COL_PREAGG_BUILD_INT_H
#define SIFT_COL_PREAGG_BUILD_INT_H

#include "col/preagg_int.h"

typedef struct GroupKey
{
	int64		key;
	uint32		g;
	uint32		isnull;
} GroupKey;

typedef struct DimBuild
{
	int			spec;				/* index into spec->dim */
	StringInfoData blob;
	int			nm;					/* measures in the blob */
	bool		ok;
	bool		keep;
	int			grain;				/* a calendar dim: the grain of its cells (SIFT_CAL_*), else 0 */
	uint32		ngroups;
} DimBuild;

extern int	cpagg_cmp_group(const void *a, const void *b);

extern bool	cpagg_has_ovf(const SiftCPaggCol *c);

extern bool	cpagg_cal_days(const SiftCPaggCol *c, bool date, uint32 nrows, int64 *out);

extern void	cpagg_cal_cells(const SiftCPaggCol *c, uint32 nrows, int grain, const int64 *days, int64 *out);

extern void	cpagg_group_rows(const uint32 *gid, uint32 n, const uint32 *cnt, uint32 ng, uint32 *pos, uint32 *perm, uint32 *gstart);
extern bool	cpagg_measure_int(const SiftCPaggCol *mc, const uint32 *perm, const uint32 *gstart, uint32 ng, uint32 nrows, SiftCI128 *si, uint32 *nn, int64 *mn, int64 *mx, bool minmax);
extern void	cpagg_emit_cube(const SiftCPaggSpec *spec, const DimBuild *db, int nd, StringInfo out);

extern void	cpagg_build_dims(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, uint32 cubeg, Size cal_budget, DimBuild *db);

extern void	cpagg_emit_rollups(const SiftColSchema *s, const SiftCPaggSpec *spec, const SiftCPaggCol *cols, uint32 nrows, bool limited, Size room, StringInfo out);

static inline bool
is_null(const uint64 *nulls, uint32 i)
{
	return nulls != NULL && ((nulls[i / 64] >> (i % 64)) & 1);
}

static inline bool
col_usable(const SiftCPaggCol *c, uint32 nrows)
{
	return c->ready && c->n == nrows && !cpagg_has_ovf(c);
}

#endif