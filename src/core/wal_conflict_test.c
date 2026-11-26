/*
 * core/wal_conflict_test.c - sift._test_wal_conflict(rel, horizon) (pg_sift_test): logs the snapshot conflict of rel at horizon through sift_wal_snapshot_conflict, true when it
 * logged one (test/t/core/wal_conflict_cover.sh).
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/relation.h"
#include "fmgr.h"
#include "utils/xid8.h"

#include "core/catname.h"
#include "core/wal_conflict.h"

PG_FUNCTION_INFO_V1(sift_test_wal_conflict);

Datum
sift_test_wal_conflict(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	bool		logged = sift_wal_snapshot_conflict(rel, InvalidBlockNumber, PG_GETARG_FULLTRANSACTIONID(1));

	relation_close(rel, AccessShareLock);
	PG_RETURN_BOOL(logged);
}

#endif
