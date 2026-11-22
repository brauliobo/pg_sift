/*
 * col/f2test/f2test.h - what the test-only SQL functions of format major 2 share (col/format2_test.c and the files of col/f2test/): the generated rows of a test part and the helpers of the functions.
 */
#ifndef SIFT_F2TEST_H
#define SIFT_F2TEST_H

#ifdef SIFT_TESTING

#include "access/genam.h"
#include "access/relation.h"
#include "access/transam.h"
#include "access/xact.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/numeric.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "col/f2/col_meta.h"
#include "col/f2/compact.h"
#include "col/f2/delete.h"
#include "col/f2/f2.h"
#include "col/f2/f2_cache.h"
#include "col/pg.h"
#include "col/f2/l0_stage.h"
#include "col/f2/part_vis.h"
#include "col/f2/preagg_delta.h"
#include "core/catname.h"
#include "core/extlock.h"
#include "core/f2/fsm.h"
#include "core/quarantine_set.h"
#include "core/page_io.h"

#define F2_NCOLS 4

extern Relation f2t_open_index(FunctionCallInfo fcinfo, int lockmode);
extern Datum f2t_record_result(FunctionCallInfo fcinfo, Datum *values, bool *nulls);
extern ReturnSetInfo *f2t_srf_begin(FunctionCallInfo fcinfo);
extern void f2t_srf_put(ReturnSetInfo *rsi, Datum *values, bool *nulls);
extern SiftPartDesc *f2t_find_parts(Relation rel, FunctionCallInfo fcinfo, int idarg, int loarg, int *n, SiftPartitionEntry *entry);
extern SiftPartDesc *f2t_find_gen(SiftPartDesc *parts, int n, uint64 gen);
extern void f2t_gen_row(int mode, int64 k, int64 *m, bool *mnull, int64 *d, bool *dnull, char *s, bool *snull);
extern uint64 f2t_tid(int64 base, int i);
extern SiftPartSpec f2t_spec(bool preagg);
extern uint32 f2t_xmin_of(int64 arg);
extern void f2t_key(int64 v, uint8 *out);
extern const uint8 *f2t_arg_bound(FunctionCallInfo fcinfo, int n, uint8 *buf);
extern uint16 *f2t_dir_rows(Relation rel, const SiftPart *p);

#endif							/* SIFT_TESTING */
#endif							/* SIFT_F2TEST_H */
