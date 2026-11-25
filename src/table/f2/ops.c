/*
 * table/f2/ops.c - the Format 2.0 table of the table access method (table/dispatch.h) and the host of the shared columnar engine (col/f2/store/store.h).
 */
#include "postgres.h"

#include "access/tableam.h"

#include "core/compat.h"
#include "col/f2/f2.h"
#include "table/dispatch.h"
#include "table/f2/blob.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/cost_hook.h"
#include "table/f2/tbl.h"

SIFT_TAM_DELETE_ADAPTER(sift_tbl_f2_tuple_delete)
SIFT_TAM_UPDATE_ADAPTER(sift_tbl_f2_tuple_update)

const TableAmRoutine sift_tbl_v2_methods = {
	.type = T_TableAmRoutine,
	.slot_callbacks = sift_tbl_f2_slot_callbacks,
	.scan_begin = sift_tbl_f2_scan_begin,
	.scan_end = sift_tbl_f2_scan_end,
	.scan_rescan = sift_tbl_f2_scan_rescan,
	.scan_getnextslot = sift_tbl_f2_scan_getnextslot,
	.scan_set_tidrange = sift_tbl_f2_scan_set_tidrange,
	.scan_getnextslot_tidrange = sift_tbl_f2_scan_getnextslot_tidrange,
	.parallelscan_estimate = sift_tbl_f2_parallelscan_estimate,
	.parallelscan_initialize = sift_tbl_f2_parallelscan_initialize,
	.parallelscan_reinitialize = sift_tbl_f2_parallelscan_reinitialize,
	.index_fetch_begin = sift_tbl_f2_index_fetch_begin,
	.index_fetch_reset = sift_tbl_f2_index_fetch_reset,
	.index_fetch_end = sift_tbl_f2_index_fetch_end,
	.index_fetch_tuple = sift_tbl_f2_index_fetch_tuple,
	.tuple_fetch_row_version = sift_tbl_f2_fetch_row_scoped,
	.tuple_tid_valid = sift_tbl_f2_tuple_tid_valid,
	.tuple_get_latest_tid = sift_tbl_f2_tuple_get_latest_tid,
	.tuple_satisfies_snapshot = sift_tbl_f2_tuple_satisfies_snapshot,
	.index_delete_tuples = sift_tbl_f2_index_delete_tuples,
	.tuple_insert = sift_tbl_f2_tuple_insert,
	.tuple_insert_speculative = sift_tbl_f2_tuple_insert_speculative,
	.tuple_complete_speculative = sift_tbl_f2_tuple_complete_speculative,
	.multi_insert = sift_tbl_f2_multi_insert,
	.tuple_delete = SIFT_TAM(sift_tbl_f2_tuple_delete),
	.tuple_update = SIFT_TAM(sift_tbl_f2_tuple_update),
	.tuple_lock = sift_tbl_f2_tuple_lock,
	.finish_bulk_insert = sift_tbl_f2_finish_bulk_insert,
	.relation_set_new_filelocator = sift_tbl_f2_relation_set_new_filelocator,
	.relation_nontransactional_truncate = sift_tbl_f2_relation_nontransactional_truncate,
	.relation_copy_data = sift_tbl_f2_relation_copy_data,
	.relation_copy_for_cluster = sift_tbl_f2_relation_copy_for_cluster,
	.relation_vacuum = sift_tbl_f2_relation_vacuum,
	.scan_analyze_next_block = sift_tbl_f2_scan_analyze_next_block,
	.scan_analyze_next_tuple = sift_tbl_f2_scan_analyze_next_tuple,
	.index_build_range_scan = sift_tbl_f2_index_build_range_scan,
	.index_validate_scan = sift_tbl_f2_index_validate_scan,
	.relation_size = sift_tbl_f2_relation_size,
	.relation_needs_toast_table = sift_tbl_f2_relation_needs_toast_table,
	.relation_estimate_size = sift_tbl_f2_relation_estimate_size,
#if PG_VERSION_NUM >= 180000
	.scan_bitmap_next_tuple = sift_tbl_f2_scan_bitmap_next_tuple,
#else
	.scan_bitmap_next_block = sift_tbl_f2_scan_bitmap_next_block,
	.scan_bitmap_next_tuple = sift_tbl_f2_scan_bitmap_next_tuple_17,
#endif
	.scan_sample_next_block = sift_tbl_f2_scan_sample_next_block,
	.scan_sample_next_tuple = sift_tbl_f2_scan_sample_next_tuple,
};

static int
sift_tbl_f2_fetch_filter_page(IndexFetchTableData *fetch, Snapshot snapshot, BlockNumber vblock, uint64 *offmask)
{
	int			left = 0;

	sift_tbl_f2_filter_page(fetch->rel, snapshot, vblock, offmask);
	for (int w = 0; w < SIFT_TBL_OFFMASK_WORDS; w++)
		left += pg_popcount64(offmask[w]);
	return left;
}

const SiftTableFormatOps sift_tbl_v2_ops = {
	.format_major = 2,
	.name = "v2",
	.tam = &sift_tbl_v2_methods,
	.option_check_set = sift_tbl_f2_option_check_set,
	.option_known = sift_tbl_f2_option_known,
	.option_names = sift_tbl_f2_option_names_array,
	.option_default = sift_tbl_f2_option_default,
	.ddl_before = sift_tbl_f2_ddl_before,
	.ddl_after = sift_tbl_f2_ddl_after,
	.ddl_rename_column = sift_tbl_f2_ddl_rename_column,
	.paths_changed = sift_tbl_f2_paths_changed,
	.filter_page = sift_tbl_f2_fetch_filter_page,
	.fetch_order = sift_tbl_f2_fetch_order,
	.nvblocks = sift_tbl_f2_nvblocks,
	.check = sift_f2_check_relation,
};

const SiftF2HostOps sift_tbl_host_ops = {
	.host_kind = SIFT_F2_HOST_TABLE,
	.am_kind = SIFT_AM_TABLE,
	.base_compat_flags = SIFT_COMPAT_PARTITIONED | SIFT_COMPAT_NATIVE_MVCC | SIFT_COMPAT_TRACK_DELETES | SIFT_COMPAT_NATIVE_STRINGS,
	.build_col_map = sift_tbl_f2_build_col_map,
	.build_col_remap = sift_tbl_f2_build_col_remap,
	.reclaim_dead_blobs = sift_tbl_blob_reclaim,
	.on_vacuum_dead_tids = sift_tbl_f2_vacuum_dead_tids,
	.read_wide_blob = sift_tbl_blob_read,
	.spill_wide_blobs = sift_tbl_blob_spill_rows,
	.load_options = sift_tbl_f2_load_options,
};

void
sift_tbl_v2_init(void)
{
	sift_tbl_f2_locks_init();
	sift_col_delta_fold_install();
	sift_tbl_f2_cost_hook_init();
}
