/*
 * test/scaling/cases/cube.c - col/f2/cube/cube.c: a scope cube takes one row per distinct group in ascending key order, the order a part is written in: a binary search
 * and an append per row, O(G log G), and no tail shifted.
 */
#include "postgres.h"

#include <stdlib.h>

#include "col/f2/cube/meta.h"
#include "op_counters.h"

static void
run_cube(uint32_t g)
{
	SiftScopeShape shape = {.nmeas = 1, .ndims = 1};
	SiftScopeCubeAcc *cube = calloc(1, sizeof(SiftScopeCubeAcc));
	SiftScopeRows rows;
	int64	   *key = malloc(sizeof(int64) * g);
	int64	   *val = malloc(sizeof(int64) * g);

	for (uint32_t i = 0; i < g; i++)
	{
		key[i] = i;
		val[i] = 1;
	}
	memset(&rows, 0, sizeof(rows));
	rows.n = g;
	rows.dim[0] = key;
	rows.meas[0] = val;
	sift_scube_init(cube, shape);
	OP_BEGIN();
	sift_scube_add_rows(cube, &rows, NULL, g, NULL);
	OP_END();
	if (sift_scube_group(cube, 0, g - 1) == NULL)
		abort();
	sift_scube_free(cube);
	free(cube);
	free(key);
	free(val);
}

const ScalingCase case_cube = {
	.name = "cube", .under_test = "col/f2/cube/cube.c, groups added in key order", .n_small = 256, .factor = 8, .run = run_cube,
	GATES({OP_STEPS, 11.5, NULL}, {OP_CMPS, 11.5, NULL}, {OP_MOVE_BYTES, 9.0, NULL}, {OP_ALLOC_BYTES, 9.0, NULL}),
};
