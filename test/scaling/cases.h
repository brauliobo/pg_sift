/* test/scaling/cases.h - the cases of check_scaling_c.c, one source each (cases.mk lists the files and the sources under test). */
#ifndef SIFT_SCALING_CASES_H
#define SIFT_SCALING_CASES_H

#include "op_counters.h"

extern const ScalingCase case_card,
			case_cube,
			case_inset,
			case_months,
			case_pack,
			case_plan,
			case_qset,
			case_lookahead,
			case_regex,
			case_stream,
			case_topn,
			case_planted_quadratic,
			case_planted_stale;

static const ScalingCase *const cases[] = {
	&case_card,
	&case_cube,
	&case_inset,
	&case_months,
	&case_pack,
	&case_plan,
	&case_qset,
	&case_lookahead,
	&case_regex,
	&case_stream,
	&case_topn,
};

#define ncases ((int) (sizeof(cases) / sizeof(cases[0])))

#endif
