/*
 * col/build/btree_guc.c - the testing settings of the build through a btree on the key (btree.c, btree_pick.c, btree_probe.c)
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "utils/guc.h"
#include "col/build.h"

typedef struct IntGuc
{
	const char *name;
	const char *desc;
	int		   *var;
	int			boot,
				min;
} IntGuc;

static const IntGuc int_gucs[] = {
	{"sift.test_col_build_btree", "Testing: the build of a sift_col index with key_cut by a btree on the key: 0 the chooser decides, 1 whenever a btree can, -1 never.", &sift_test_col_build_btree, 0, -1},
	{"sift.test_col_build_source_used", "Testing: where the last sift_col build took its rows from: 0 the heap, 1 the heap through a btree, 2 the index it replaces.", &sift_test_col_build_source_used, 0, 0},
	{"sift.test_col_build_spill_said", "Testing: 1 when the NOTICE of the last build's choice said that the segment writer sorts entries on temporary files.", &sift_test_col_build_spill_said, 0, 0},
	{"sift.test_col_choose_mutant", "Testing: a defect in the choice of the path of a build (1: the dearest path is taken, 2: the room of the temporary files is not looked at).", &sift_test_col_choose_mutant, 0, 0},
	{"sift.test_col_btree_entries", "Testing: entries of the btree the last build read the heap through.", &sift_test_col_btree_entries, 0, 0},
	{"sift.test_col_btree_visible", "Testing: tuples the last build by btree found visible.", &sift_test_col_btree_visible, 0, 0},
	{"sift.test_col_btree_blocks", "Testing: heap blocks the last build by btree asked for (a run of entries of one block counts once).", &sift_test_col_btree_blocks, 0, 0},
	{"sift.test_col_pick_cache_blocks", "Testing: heap blocks the cache of the build's choice between the btree and the key ranges holds (-1 = shared_buffers).", &sift_test_col_pick_cache_blocks, -1, -1},
	{"sift.test_col_pick_entries", "Testing: entries of the btree the last choice walked.", &sift_test_col_pick_entries, 0, 0},
	{"sift.test_col_pick_blocks", "Testing: heap blocks the last choice counted along the entries (a run of entries of one block counts once).", &sift_test_col_pick_blocks, 0, 0},
	{"sift.test_col_pick_reads", "Testing: heap blocks the last choice predicted the btree reads.", &sift_test_col_pick_reads, 0, 0},
	{"sift.test_col_pick_random", "Testing: of those, the blocks the last choice predicted are read at random.", &sift_test_col_pick_random, 0, 0},
	{"sift.test_col_pick_spool_bytes", "Testing: bytes of temporary files the last choice estimated the key ranges write.", &sift_test_col_pick_spool_bytes, 0, 0},
	{"sift.test_col_pick_walk_bytes", "Testing: bytes the walk of the last choice held (the simulated cache of heap blocks and the keys of the boundaries).", &sift_test_col_pick_walk_bytes, 0, 0},
};

void
sift_col_btree_test_gucs(void)
{
	for (int i = 0; i < lengthof(int_gucs); i++)
		DefineCustomIntVariable(int_gucs[i].name, int_gucs[i].desc, NULL, int_gucs[i].var, int_gucs[i].boot, int_gucs[i].min, INT_MAX, PGC_USERSET,
								GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_col_btree_gate_open",
							 "Testing: a btree passes for one that gives the key's order whatever its columns and order (a mutant of the gate).",
							 NULL, &sift_test_col_btree_gate_open, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.test_col_pick_random_us",
							 "Testing: what the choice takes a random read of the heap to cost, in microseconds (-1 = measure it as a build does).",
							 NULL, &sift_test_col_pick_random_us, 80.0, -1.0, 1e6, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.test_col_pick_seq_us",
							 "Testing: what the choice takes a block of a sequential read of the heap to cost, in microseconds (-1 = measure it as a build does).",
							 NULL, &sift_test_col_pick_seq_us, 10.0, -1.0, 1e6, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}
#endif
