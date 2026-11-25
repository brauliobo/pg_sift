/*
 * test/scaling/cases/qset.c - core/quarantine/set.c: is this relation quarantined? A lookup per relation of the statement, against the set of Q quarantined relations (slots of one shared
 * table, SIFT_QSET_SLOTS). The shared table is a plain array in this harness (sift_dsm_join).
 */
#include "postgres.h"

#include <stdlib.h>

#include "core/quarantine/set.h"
#include "op_counters.h"

#define LOOKUPS 4096

void *
sift_dsm_join(const char *name, Size size, void (*init) (void *ptr))
{
	void	   *p = calloc(1, size);

	init(p);
	return p;
}

static void
run_qset(uint32_t q)
{
	for (uint32_t i = 0; i < q; i++)
		sift_qset_add(&(RelFileLocator) {.spcOid = 1663, .dbOid = 5, .relNumber = 16384 + i});
	OP_BEGIN();
	for (uint32_t i = 0; i < LOOKUPS; i++)
		if (sift_qset_has(&(RelFileLocator) {.spcOid = 1663, .dbOid = 5, .relNumber = 16384 + (i * 7) % (2 * q)}) && i == UINT32_MAX)
			abort();
	OP_END();
	for (uint32_t i = 0; i < q; i++)
		sift_qset_remove(&(RelFileLocator) {.spcOid = 1663, .dbOid = 5, .relNumber = 16384 + i});
}

const ScalingCase case_qset = {
	.name = "quarantine_set", .under_test = "core/quarantine_set.c lookups, set x8", .n_small = 32, .factor = 8, .run = run_qset,
	GATES({OP_STEPS, 1.25, "Spec 66 #9: hashed lookup (63A)"}, {OP_ALLOC_BYTES, 1.25, NULL}),
};
