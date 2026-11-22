/*
 * table/v1/v1_ops.c - the Format 1 table of the table access method (table/dispatch.h)
 */
#include "postgres.h"

#include "table/v1/scan_int.h"

const SiftTableFormatOps sift_tbl_v1_ops = {
	.format_major = 1,
	.name = "v1",
	.tam = &sift_tbl_v1_methods,
	.option_names = sift_tbl_v1_option_names,
	.option_default = sift_tbl_v1_option_default,
	.filter_page = sift_tbl_filter_page,
	.nvblocks = sift_tbl_nvblocks,
	.check = sift_tbl_v1_check,
};

StaticAssertDecl(offsetof(SiftTblScan, ops) == offsetof(SiftTblScanHeader, ops), "a scan starts with SiftTblScanHeader");
