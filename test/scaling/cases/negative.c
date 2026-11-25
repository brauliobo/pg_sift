/*
 * test/scaling/case_negative.c - the planted regression of `check_scaling_c --negative-test`: a loop over every pair of elements where one pass is enough. The gate must fail it.
 */
#include "postgres.h"

#include <stdlib.h>

#include "op_counters.h"

static volatile uint64_t op_sink;

static void
run_quadratic(uint32_t n)
{
	uint32_t   *v = malloc(sizeof(uint32_t) * n);
	uint64_t	dup = 0;

	for (uint32_t i = 0; i < n; i++)
		v[i] = i * 2654435761u % 1000003;
	OP_BEGIN();
	for (uint32_t i = 0; i < n; i++)
		for (uint32_t j = 0; j < i; j++)
			dup += v[i] == v[j];
	OP_END();
	free(v);
	op_sink = dup;
}

/* a linear loop whose gate still says pending: the marker is stale and the gate must say so */
static void
run_linear(uint32_t n)
{
	uint64_t	sum = 0;

	OP_BEGIN();
	for (uint32_t i = 0; i < n; i++)
		sum += i * 2654435761u;
	OP_END();
	op_sink = sum;
}

const ScalingCase case_planted_stale = {
	.name = "planted_stale", .under_test = "a linear loop marked pending", .n_small = 512, .factor = 8, .run = run_linear,
	GATES({OP_STEPS, 9.5, "Spec 66 selftest"}),
};

const ScalingCase case_planted_quadratic = {
	.name = "planted_quadratic", .under_test = "a duplicate scan of every pair", .n_small = 512, .factor = 8, .run = run_quadratic,
	GATES({OP_STEPS, 9.0, NULL}, {OP_CMPS, 9.0, NULL}),
};
