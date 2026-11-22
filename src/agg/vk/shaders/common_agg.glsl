/*
 * common_agg.glsl - what the kernels that run the expression program share: the row driver (liveness of the row, the program, its error,
 * the WHERE slot) and the aggregate descriptions of the job header; the granules and the accumulators are common_gran.glsl.
 */
#include "common_gran.glsl"
#include "common_vexpr.glsl"

/* true when the row is live, the program ran without error and the WHERE slot holds */
bool row_begin(uint gi, uint dtrow, uint row)
{
	if (row_dead(gi, row))
		return false;
	if (!vm_run(dtrow, row))
	{
		report_error(row, vm_err, vm_err_op);
		return false;
	}
	uint w = ctl[J_WHERE];
	return w == NONE8 || mtrue(w);
}

bool agg_in(uint a)
{
	uint f = ctl[J_AGGFREG + a];

	return f == NONE8 || mtrue(f);
}

bool agg_vok(uint a)
{
	uint v = ctl[J_AGGVREG + a];

	return v == NONE8 || rvalid(v);
}

int64_t agg_v(uint a)
{
	return R[ctl[J_AGGVREG + a]];
}

/* the order-preserving key the MIN or MAX of an aggregate compares: the value, or the key of the double */
int64_t agg_key(uint a, uint kind)
{
	int64_t v = agg_v(a);

	return (kind == AK_MIN_F || kind == AK_MAX_F) ? f8_key(dbl(v)) : v;
}
