/*
 * agg/kern/gid.h - the grouped accumulation of the levels without a scatter (AVX2, NEON), in plain C so that every level of the architecture
 * compiles the same code. Two shapes, both in row order (what the scalar table specifies):
 *
 * - a hot low-cardinality batch (the gids of the active rows all below SIFT_AK6_BANK_G, and rows enough to pay for the fold) adds into four
 *   banks of private accumulators, row j into bank j % 4, so that consecutive rows of one group are not a chain of store-to-load forwards of
 *   one cell; the banks are folded into the output at the end. Integer sums, counts, minimums and maximums are exact in any order; a
 *   float sum is not, so it never takes this shape.
 * - any other batch runs the loop four rows an iteration: the cells are independent of each other, so the out-of-order core overlaps the misses of
 *   a table that lives in L2 or L3 (a software prefetch of the cell of a row ahead measured no faster than that, up to 65536 groups).
 */
#ifndef SIFT_AGGKERN6_GID_H
#define SIFT_AGGKERN6_GID_H

#include "agg/kern/int.h"

#define SIFT_AK6_BANKS		4
#define SIFT_AK6_BANK_G		256
/* the batch is probed on its first rows: all below this, the whole batch is scanned for its largest group */
#define SIFT_AK6_BANK_PROBE	16
#define SIFT_AK6_BANK_MIN_N	256

/* The attribute of the functions of the includer's level (its target) and its scan for the largest group of rows without a selection vector */
#ifndef AK6_GID_ATTR
#define AK6_GID_ATTR
#endif

#ifndef AK6_GID_MAX_NOSEL
static inline uint32
ak6_gid_max_nosel(const uint32 *gid, int from, int n)
{
	uint32		m = 0;

	for (int i = from; i < n; i++)
		m = Max(m, gid[i]);
	return m;
}

#define AK6_GID_MAX_NOSEL	ak6_gid_max_nosel
#endif

#define AK6_G	static __attribute__((unused)) AK6_GID_ATTR

/*
 * Whether the four banks pay: a batch of rows enough whose groups, on its first rows, are few (a table of a hot low-cardinality GROUP BY, or a
 * column sorted by group) is scanned for its largest group, and the banks are used when that is below SIFT_AK6_BANK_G and the fold (four cells a
 * group) is a small part of the rows. A batch of many groups does not pay for the scan and takes the loop. *mg: the largest group.
 */
static inline AK6_GID_ATTR bool
ak6_use_banks(const uint32 *gid, const uint16 *sel, uint16 n, uint32 *mg)
{
	uint32		m = 0;

	if (n < SIFT_AK6_BANK_MIN_N)
		return false;
	for (int i = 0; i < SIFT_AK6_BANK_PROBE; i++)
		m = Max(m, gid[ROW_OF_SEL(sel, i)]);
	if (m >= SIFT_AK6_BANK_PROBE)
		return false;
	if (sel)
		for (int i = SIFT_AK6_BANK_PROBE; i < n; i++)
			m = Max(m, gid[sel[i]]);
	else
		m = Max(m, AK6_GID_MAX_NOSEL(gid, SIFT_AK6_BANK_PROBE, n));
	*mg = m;
	return m < SIFT_AK6_BANK_G && n >= 8 * (m + 1);
}

/* UPD(g, r) for the active rows in order, four rows an iteration */
#define AK6_GID_LOOP(gid, sel, n, UPD) \
	do { \
		int i_ = 0; \
		for (; i_ + 4 <= (n); i_ += 4) \
		{ \
			const int r0_ = ROW_OF_SEL(sel, i_), r1_ = ROW_OF_SEL(sel, i_ + 1), r2_ = ROW_OF_SEL(sel, i_ + 2), r3_ = ROW_OF_SEL(sel, i_ + 3); \
			UPD((gid)[r0_], r0_); \
			UPD((gid)[r1_], r1_); \
			UPD((gid)[r2_], r2_); \
			UPD((gid)[r3_], r3_); \
		} \
		for (; i_ < (n); i_++) \
		{ \
			const int r_ = ROW_OF_SEL(sel, i_); \
			UPD((gid)[r_], r_); \
		} \
	} while (0)

/* UPD(bank, g, r) for the active rows in order, row j into bank j % 4 */
#define AK6_GID_BANKED(gid, sel, n, UPD) \
	do { \
		int i_ = 0; \
		for (; i_ + 4 <= (n); i_ += 4) \
		{ \
			const int r0_ = ROW_OF_SEL(sel, i_), r1_ = ROW_OF_SEL(sel, i_ + 1), r2_ = ROW_OF_SEL(sel, i_ + 2), r3_ = ROW_OF_SEL(sel, i_ + 3); \
			UPD(0, (gid)[r0_], r0_); \
			UPD(1, (gid)[r1_], r1_); \
			UPD(2, (gid)[r2_], r2_); \
			UPD(3, (gid)[r3_], r3_); \
		} \
		for (; i_ < (n); i_++) \
		{ \
			const int r_ = ROW_OF_SEL(sel, i_); \
			UPD(0, (gid)[r_], r_); \
		} \
	} while (0)

AK6_G void
ak6_g_count_gid(int64 *cnt, const uint32 *gid, const uint16 *sel, uint16 n)
{
	uint32		mg;

#define UPD_B(k, g, r)	b[k][g]++
#define UPD_1(g, r)		cnt[g]++
	if (ak6_use_banks(gid, sel, n, &mg))
	{
		int64		b[SIFT_AK6_BANKS][SIFT_AK6_BANK_G];

		for (int k = 0; k < SIFT_AK6_BANKS; k++)
			memset(b[k], 0, (mg + 1) * sizeof(int64));
		AK6_GID_BANKED(gid, sel, n, UPD_B);
		for (uint32 g = 0; g <= mg; g++)
			cnt[g] += b[0][g] + b[1][g] + b[2][g] + b[3][g];
		return;
	}
	AK6_GID_LOOP(gid, sel, n, UPD_1);
#undef UPD_B
#undef UPD_1
}

#define AK6_G_SUM_I64(NAME, VT, UPD)	\
AK6_G void \
NAME(int64 *acc, const uint32 *gid, const VT *v, const uint16 *sel, uint16 n) \
{ \
	uint32		mg; \
	if (ak6_use_banks(gid, sel, n, &mg)) \
	{ \
		int64		b[SIFT_AK6_BANKS][SIFT_AK6_BANK_G]; \
		for (int k = 0; k < SIFT_AK6_BANKS; k++) \
			memset(b[k], 0, (mg + 1) * sizeof(int64)); \
		AK6_GID_BANKED(gid, sel, n, UPD##_B); \
		for (uint32 g = 0; g <= mg; g++) \
			acc[g] = (int64) ((uint64) acc[g] + (uint64) b[0][g] + (uint64) b[1][g] + (uint64) b[2][g] + (uint64) b[3][g]); \
		return; \
	} \
	AK6_GID_LOOP(gid, sel, n, UPD##_1); \
}

#define UPD32_B(k, g, r)	ak6_upd_sum_i32(b[k], g, v[r])
#define UPD32_1(g, r)		ak6_upd_sum_i32(acc, g, v[r])
#define UPD64_B(k, g, r)	ak6_upd_sum_i64(b[k], g, v[r])
#define UPD64_1(g, r)		ak6_upd_sum_i64(acc, g, v[r])
AK6_G_SUM_I64(ak6_g_sum_gid_i32, int32, UPD32)
AK6_G_SUM_I64(ak6_g_sum_gid_i64, int64, UPD64)

#define AK6_G_SUM_I128(NAME, VT, FN)	\
AK6_G void \
NAME(SiftI128 *acc, const uint32 *gid, const VT *v, const uint16 *sel, uint16 n) \
{ \
	uint32		mg; \
	if (ak6_use_banks(gid, sel, n, &mg)) \
	{ \
		SiftI128	b[SIFT_AK6_BANKS][SIFT_AK6_BANK_G]; \
		for (int k = 0; k < SIFT_AK6_BANKS; k++) \
			memset(b[k], 0, (mg + 1) * sizeof(SiftI128)); \
		AK6_GID_BANKED(gid, sel, n, FN##_B); \
		for (uint32 g = 0; g <= mg; g++) \
			acc[g] = ak6_i128_of(ak6_u128_of(acc[g]) + ak6_u128_of(b[0][g]) + ak6_u128_of(b[1][g]) + ak6_u128_of(b[2][g]) + ak6_u128_of(b[3][g])); \
		return; \
	} \
	AK6_GID_LOOP(gid, sel, n, FN##_1); \
}

#define UPD6412_B(k, g, r)	ak6_upd_sum_i64_i128(b[k], g, v[r])
#define UPD6412_1(g, r)		ak6_upd_sum_i64_i128(acc, g, v[r])
#define UPD128_B(k, g, r)	ak6_upd_sum_i128(b[k], g, v[r])
#define UPD128_1(g, r)		ak6_upd_sum_i128(acc, g, v[r])
AK6_G_SUM_I128(ak6_g_sum_gid_i64_to_i128, int64, UPD6412)
AK6_G_SUM_I128(ak6_g_sum_gid_i128, SiftI128, UPD128)

AK6_G void
ak6_g_minmax_gid_i64(int64 *mn, int64 *mx, const uint32 *gid, const int64 *v, const uint16 *sel, uint16 n)
{
	uint32		mg;

#define UPDMM_B(k, g, r)	ak6_upd_minmax_i64(bmn[k], bmx[k], g, v[r])
#define UPDMM_1(g, r)		ak6_upd_minmax_i64(mn, mx, g, v[r])
	if (ak6_use_banks(gid, sel, n, &mg))
	{
		int64		bmn[SIFT_AK6_BANKS][SIFT_AK6_BANK_G],
					bmx[SIFT_AK6_BANKS][SIFT_AK6_BANK_G];

		for (int k = 0; k < SIFT_AK6_BANKS; k++)
			for (uint32 g = 0; g <= mg; g++)
			{
				bmn[k][g] = INT64_MAX;
				bmx[k][g] = INT64_MIN;
			}
		AK6_GID_BANKED(gid, sel, n, UPDMM_B);
		for (uint32 g = 0; g <= mg; g++)
			for (int k = 0; k < SIFT_AK6_BANKS; k++)
				ak6_upd_minmax_i64_pair(mn, mx, g, bmn[k][g], bmx[k][g]);
		return;
	}
	AK6_GID_LOOP(gid, sel, n, UPDMM_1);
#undef UPDMM_B
#undef UPDMM_1
}

/* Whether the first rows come in runs of one group: at least half of them have the group of the row before */
static inline AK6_GID_ATTR bool
ak6_runs_likely(const uint32 *gid, const uint16 *sel, uint16 n)
{
	int			same = 0;

	if (n < 64)
		return false;
	for (int i = 1; i < 32; i++)
		same += gid[ROW_OF_SEL(sel, i)] == gid[ROW_OF_SEL(sel, i - 1)];
	return same >= 16;
}

/*
 * A float sum is order dependent (and a NaN sum is the canonical NaN, ak6_add_f64), so the rows go in order, one cell after the other. The cell of a run of rows of one group (a column sorted or
 * clustered by group) stays in a register, which adds the same values in the same order and is not a chain of store-to-load forwards; a batch
 * that does not come in runs would only mispredict the change of group and takes the plain loop.
 */
AK6_G void
ak6_g_sum_gid_f64_fast(double *acc, const uint32 *gid, const double *v, const uint16 *sel, uint16 n)
{
	uint32		cg;
	double		cur;

#define UPDF_1(g, r)	acc[g] = ak6_add_f64(acc[g], v[r])
	if (!ak6_runs_likely(gid, sel, n))
	{
		AK6_GID_LOOP(gid, sel, n, UPDF_1);
		return;
	}
#undef UPDF_1
	cg = gid[ROW_OF_SEL(sel, 0)];
	cur = acc[cg];
	for (int i = 0; i < n; i++)
	{
		const int	r = ROW_OF_SEL(sel, i);

		if (gid[r] != cg)
		{
			acc[cg] = cur;
			cg = gid[r];
			cur = acc[cg];
		}
		cur = ak6_add_f64(cur, v[r]);
	}
	acc[cg] = cur;
}

AK6_G void
ak6_g_minmax_gid_f64(double *mn, double *mx, const uint32 *gid, const double *v, const uint16 *sel, uint16 n)
{
#define UPDF_1(g, r)	ak6_upd_minmax_f64(mn, mx, g, v[r])
	AK6_GID_LOOP(gid, sel, n, UPDF_1);
#undef UPDF_1
}

#endif							/* SIFT_AGGKERN6_GID_H */
