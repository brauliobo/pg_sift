/*
 * agg/settings.c - sift.enable_agg and sift.agg_exact_float, which every aggregate scan reads, and the testing setting sift.agg_force
 */
#include "postgres.h"

#include "utils/guc.h"

#include "agg/settings.h"

bool		sift_enable_agg = true;
bool		sift_agg_exact_float = false;
bool		sift_agg_force = false;
double		sift_agg_scale_horizon = 100.0;

#define FORCE_SCALE	1e-6

void
sift_v6_force_price(Path *p)
{
	if (sift_agg_force)
	{
		p->startup_cost *= FORCE_SCALE;
		p->total_cost *= FORCE_SCALE;
	}
}

void
sift_v6_define_gucs(void)
{
	DefineCustomBoolVariable("sift.enable_agg",
							 "Enables aggregate pushdown into sift_col column indexes (Custom Scan (SiftAgg)).",
							 NULL, &sift_enable_agg, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_exact_float",
							 "Adds float8 values in the order of the rows, in one process, instead of the order of the vector kernels, so that a sum repeats to the last bit.",
							 NULL, &sift_agg_exact_float, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#ifdef SIFT_TESTING
	DefineCustomBoolVariable("sift.agg_force",
							 "A diagnostic: takes the aggregate scan wherever one is possible, whatever the plans cost (an arm that is refused for its shape stays refused).",
							 "The scan is priced at a millionth of its cost (the paths with workers and without keep their order) and the inline plan of a shared CTE is kept when it has a scan, to time the plan the cost model refused.",
							 &sift_agg_force, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
#endif
}
