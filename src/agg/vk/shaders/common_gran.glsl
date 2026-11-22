/*
 * common_gran.glsl - what every aggregate kernel shares, the expression machine or not: the granule of a batch (its row count, its id, its
 * deleted rows), the first error of the scan, and the exact accumulators. A SUM is held as two exact limbs (the sum of the low 32 bits,
 * unsigned, and of the high 32 bits, signed: 128 bits in all, whatever the order of the additions); a COUNT, MIN, MAX is one word and every
 * aggregate keeps the rows it counted (non-null, passing its FILTER) in its third word.
 */
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_EXT_shader_subgroup_extended_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

#include "common_decode.glsl"

#define WG 256u

uint g_gran_id;
uint g_nrows;
uint g_dead;

/* the granule gi of the batch: its descriptors' word offset; sets the row count and the id */
uint granule_open(uint gi)
{
	uint gt = ctl[J_GT] + gi * 4u;

	g_nrows = ctl[gt];
	g_gran_id = ctl[gt + 2u];
	g_dead = ctl[gt + 1u];
	return ctl[J_DT] + gi * ctl[J_NCOLS] * 8u;
}

bool row_dead(uint gi, uint row)
{
	return g_dead != DC_ALL_VALID && ((ctl[ctl[J_DEAD] + g_dead + (row >> 5)] >> (row & 31u)) & 1u) != 0u;
}

void report_error(uint row, uint code, uint op)
{
	atomicMin(res[RES_ERR], int64_t((uint64_t(g_gran_id) << 40) | (uint64_t(row) << 24) | (uint64_t(op) << 16) | uint64_t(code)));
}

int64_t acc_init(uint kind, uint j)
{
	if (j == 0u && (kind == AK_MIN || kind == AK_MIN_F))
		return I64_MAX;
	if (j == 0u && (kind == AK_MAX || kind == AK_MAX_F))
		return I64_MIN;
	return 0l;
}

int64_t sum_lo(int64_t v)
{
	return int64_t(uint64_t(v) & 0xFFFFFFFFul);
}

int64_t sum_hi(int64_t v)
{
	return v >> 32;
}
