/*
 * test/unit/col/f2/gpu/plan.c - the parts the device may read under column deltas (col/f2/gpu/plan.c): the columns a query reads are named as a column delta names them (a sift_col index by
 * the leaf plus one, col/f2/col_delta/idx.h; a sift_table by attribute), so a jsonb column of several leaves before them leaves no patch unseen; and a sift_col part whose rows a column delta
 * moved to new heap addresses is left to the CPU when the partition has pending deletes, which name rows by their heap address (S120-18).
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#include "col/f2/gpu/plan.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

volatile sig_atomic_t InterruptPending = false;

static SiftF2HostOps host;
static int16 attno[4] = {1, 1, 2, 3};
static SiftF2Store store = {.host = &host,.ncols = 4,.leaf_attno = attno};
static SiftColDeltaPartMap map;

SiftF2Store *
sift_f2_store_open(Relation rel)
{
	return &store;
}

void
sift_tbl_f2_vis_begin(SiftTblVis *v, Relation rel, Snapshot snap)
{
}

SiftColDeltaPartMap *
sift_col_delta_load_part_map(Relation rel, const SiftPartitionEntry *entry, uint64 part_gen, uint32 ngranules, SiftTblVis *vis, uint64 snap_manifest_ver)
{
	return &map;
}

void
sift_col_delta_part_map_free(SiftColDeltaPartMap *m)
{
}

/* doc (two leaves), dim_a_id, v: the query reads v, leaf 3 of attribute 3 */
static void
test_read_leaf_named_as_the_delta(void)
{
	SiftGpuQuery q = {.ncols = 1,.leaf = {3}};

	host.host_kind = SIFT_F2_HOST_INDEX;
	SIFT_CHECK(att_bloom(&q) == sift_cd_att_bit(4));
	host.host_kind = SIFT_F2_HOST_TABLE;
	SIFT_CHECK(att_bloom(&q) == sift_cd_att_bit(3));
}

/* a delta that only moved rows: a part of a sift_col index with pending deletes is patched, one without them or of a sift_table is not */
static bool
moved_rows_patch(SiftF2HostKind kind, bool pending)
{
	RelationData heap;
	SiftPart	part = {0};
	SiftF2PartPin pin = {.part = &part};
	SiftPartitionEntry e = {.flags = SIFT_PARTENTRY_HAS_COL_DELTA};
	SiftGpuQuery q = {.heap = kind == SIFT_F2_HOST_INDEX ? &heap : NULL,.ncols = 1,.leaf = {3}};
	Admit		a = {.q = &q,.e = &e,.pdel = pending ? (const SiftPendDelIndex *) &part : NULL};

	host.host_kind = kind;
	a.att_bloom = att_bloom(&q);
	map.dirty_attnum_bloom = sift_cd_att_bit(SIFT_CD_ATT_TID_REMAP);
	return col_delta_patches(&a, &pin);
}

static void
test_moved_rows_with_pending_deletes(void)
{
	SIFT_CHECK(moved_rows_patch(SIFT_F2_HOST_INDEX, true));
	SIFT_CHECK(!moved_rows_patch(SIFT_F2_HOST_INDEX, false));
	SIFT_CHECK(!moved_rows_patch(SIFT_F2_HOST_TABLE, true));
}

int
main(void)
{
	test_read_leaf_named_as_the_delta();
	test_moved_rows_with_pending_deletes();
	puts("test_gpu_plan: ok");
	return 0;
}
