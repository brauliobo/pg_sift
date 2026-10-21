/*
 * agg/back_host.c - the part of agg/agg.c that serves the backends of agg/back.h: the sums of a unit they took, and the second run
 * of a scan whose backend failed
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "parser/parse_agg.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/back.h"
#include "agg/kern.h"
#include "agg/part.h"
#include "agg/spec.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

/* the sums of the groups of a unit, as the pre-aggregates of a unit give them (preagg_hist) */
void
sift_agg_fold_sums(void *host, const SiftAggUnitSums *r)
{
	AggExec    *ex = host;
	int			naggs = Max(ex->spec->naggs, 1);
	PaVal	   *v = palloc0(naggs * sizeof(PaVal));
	uint32	   *ids = palloc0(Max(r->nkept, 1) * sizeof(uint32)),
			   *gid = palloc0(Max(r->nkept, 1) * sizeof(uint32));

	if (r->keys)
	{
		preagg_key_ids(ex, 0, r->keys->key, NULL, r->keys->ngroups - r->keys->has_null, r->keys->dict, r->kept, r->nkept, ids);
		ids_to_gids(ex, r->nkept, ids, gid);
	}
	for (int j = 0; j < r->nkept; j++)
	{
		for (int ai = 0; ai < ex->spec->naggs; ai++)
			v[ai] = (PaVal) {.rows = r->rows[j], .nn = r->rows[j], .sum = r->sum[ai] ? r->sum[ai][j] : 0};
		preagg_fold(ex, gid[j], r->rows[j], v);
	}
	pfree(v);
	pfree(ids);
	pfree(gid);
}

/* Offers the scan to the backends; true when one runs it, which then runs without workers */
bool
backend_begin(AggExec *ex)
{
	SiftAggScan sc = {
		.host = ex, .spec = ex->spec, .lay = ex->lay, .nbase = ex->nbase, .src = ex->src, .index = ex->index,
		.pushed = ex->pushed, .pflt = ex->pflt, .cxt = ex->cxt,
		.row_scan = ex->index != NULL && !ex->fuse && !ex->search && !ex->topn
	};
	bool		exclusive = false;

	ex->be = sift_aggbe_begin(&sc, &ex->be_state, &exclusive);
	return exclusive;
}

/* The end of the scan: the backend runs what it queued. When it failed, the groups so far hold the units it answered and the CPU has not seen the others: start over */
void
backend_finish(AggExec *ex)
{
	if (!sift_aggbe_finish(ex->be, ex->be_state))
		return;
	table_reset(ex);
	ex->n_units = ex->n_pre_units = ex->n_rows = ex->n_tuples = 0;
	pg_atomic_write_u32(&ex->shared->next, 0);
	scan_items(ex);
}
