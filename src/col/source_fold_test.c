/*
 * col/source_fold_test.c - test-only SQL functions sift._test_store_agg and sift._test_store_par, compiled only with SIFT_TESTING: the zone and parallel scans of the storage seam (core/store_ops.h) as
 * the aggregate engine drives them, over whatever format owns the relation. _test_store_agg groups the rows of a sift_col index or a sift_table by one column with count(*) and sum(int column) and
 * an optional "column > value" qual, through the zone ops; _test_store_par does the same over a sift_col index through the parallel ops, with real workers claiming runs of the units of the plan,
 * and gives the groups of the key (an integer between 0 and 62; NULL is 63) and the workers launched. _test_store_dict says whether an attribute is dictionary-coded. The tests hold both to the heap's own answer.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "port/atomics.h"
#include "storage/shm_toc.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "agg/v6/v6.h"
#include "core/compat.h"
#include "core/store_ops.h"

PG_FUNCTION_INFO_V1(sift_test_store_agg);
PG_FUNCTION_INFO_V1(sift_test_store_par);
PG_FUNCTION_INFO_V1(sift_test_store_dict);
PGDLLEXPORT void sift_store_par_test_worker(dsm_segment *seg, shm_toc *toc);

#define KEY_SHARED	UINT64CONST(0x5352504152)
#define PAR_KEYS	64
#define PAR_RUN		1

typedef struct ParQuery
{
	Oid			relid;
	bool		is_table;
	int			key_att;
	int			sum_att;		/* 0: count(*) alone */
	int			qual_att;		/* 0: no qual */
	int64		qual_gt;
} ParQuery;

static Oid
att_type(Oid relid, bool is_table, int att)
{
	SiftStoreSchema schema;

	sift_store_ops_for_oid(relid, is_table)->schema(relid, is_table, &schema);
	return schema.atts[att - 1].typid;
}

static SiftV6Spec *
make_spec(const ParQuery *q)
{
	SiftV6Spec *spec = palloc0(sizeof(SiftV6Spec));
	const Oid	key_type = att_type(q->relid, q->is_table, q->key_att);

	spec->relid = q->relid;
	spec->is_table = q->is_table;
	spec->nkeys = 1;
	spec->key = palloc0(sizeof(SiftV6Col));
	spec->key[0] = (SiftV6Col) {.attno = q->key_att,.typid = key_type};
	spec->naggs = q->sum_att > 0 ? 2 : 1;
	spec->agg = palloc0(spec->naggs * sizeof(SiftV6Agg));
	spec->agg[0] = (SiftV6Agg) {.kind = SV6_COUNT_STAR,.restype = INT8OID};
	if (q->sum_att > 0)
		spec->agg[1] = (SiftV6Agg) {.kind = SV6_SUM,.arg = {.attno = q->sum_att,.typid = att_type(q->relid, q->is_table, q->sum_att)},.restype = INT8OID};
	return spec;
}

static Datum
int_datum(Oid typid, int64 v)
{
	return typid == INT2OID ? Int16GetDatum((int16) v) : typid == INT4OID ? Int32GetDatum((int32) v) : Int64GetDatum(v);
}

/* column > value as the index qual of a sift_table (a Var of INDEX_VAR) */
static List *
table_qual(const ParQuery *q)
{
	const Oid	t = att_type(q->relid, true, q->qual_att);
	Var		   *var = makeVar(INDEX_VAR, q->qual_att, t, -1, InvalidOid, 0);
	Const	   *c = makeConst(t, -1, InvalidOid, t == INT2OID ? 2 : t == INT4OID ? 4 : 8, int_datum(t, q->qual_gt), false, t != INT8OID || FLOAT8PASSBYVAL);

	return list_make1(make_opclause(OpernameGetOprid(list_make1(makeString(">")), t, t), BOOLOID, false, (Expr *) var, (Expr *) c, InvalidOid, InvalidOid));
}

/* column > value as the scan key of a sift_col index over the attributes of its table in order */
static int
index_keys(const ParQuery *q, ScanKeyData *key)
{
	if (q->qual_att == 0)
		return 0;
	ScanKeyInit(key, q->qual_att, BTGreaterStrategyNumber, F_INT8GT, int_datum(att_type(q->relid, false, q->qual_att), q->qual_gt));
	return 1;
}

static TupleDesc
group_desc(const SiftV6Spec *spec)
{
	TupleDesc	desc = CreateTemplateTupleDesc(1 + spec->naggs);

	TupleDescInitEntry(desc, 1, "k", spec->key[0].typid, -1, 0);
	for (int j = 0; j < spec->naggs; j++)
		TupleDescInitEntry(desc, 2 + j, "a", INT8OID, -1, 0);
	sift_tupdesc_finalize(desc);
	return BlessTupleDesc(desc);
}

/* (rel, is_table, key_att, sum_att, qual_att, qual_gt): the groups of the key with their count and sum */
Datum
sift_test_store_agg(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	const ParQuery q = {.relid = PG_GETARG_OID(0),.is_table = PG_GETARG_BOOL(1),.key_att = PG_GETARG_INT32(2),.sum_att = PG_ARGISNULL(3) ? 0 : PG_GETARG_INT32(3),
		.qual_att = PG_ARGISNULL(4) ? 0 : PG_GETARG_INT32(4),.qual_gt = PG_ARGISNULL(5) ? 0 : PG_GETARG_INT64(5)};
	const SiftStoreOps *ops = sift_store_ops_for_oid(q.relid, q.is_table);
	SiftV6Spec *spec = make_spec(&q);
	SiftV6ScanStats stats = {0};
	TupleDesc	desc = group_desc(spec);
	SiftV6Group *g = sift_v6_group_new(spec, CurrentMemoryContext, desc);
	Snapshot	snap = GetActiveSnapshot();
	Relation	rel = relation_open(q.relid, AccessShareLock);
	Datum		values[3];
	bool		nulls[3];
	Oid			outfn;
	bool		isvarlena;

	if (ops == NULL || ops->zone == NULL)
		ereport(ERROR, (errmsg("no storage format with a zone scan owns relation %u", q.relid)));
	InitMaterializedSRF(fcinfo, 0);
	if (q.is_table)
	{
		spec->indexqual = q.qual_att > 0 ? table_qual(&q) : NIL;
		ops->zone->scan_table(spec, g, rel, ops->zone->compile_quals(rel, spec->indexqual, CurrentMemoryContext), snap, NULL, 0, &stats, NULL);
	}
	else
	{
		ScanKeyData key;
		const int	nkeys = index_keys(&q, &key);
		Relation	heap = relation_open(rel->rd_index->indrelid, AccessShareLock);

		ops->zone->scan_run(spec, g, heap, rel, &key, nkeys, snap, NULL, 0, NULL, &stats, NULL, NULL, NULL);
		relation_close(heap, AccessShareLock);
	}
	sift_v6_group_finish(g);
	getTypeOutputInfo(spec->key[0].typid, &outfn, &isvarlena);
	while (sift_v6_group_next(g, values, nulls))
	{
		Datum		out[4] = {nulls[0] ? (Datum) 0 : CStringGetTextDatum(OidOutputFunctionCall(outfn, values[0])), values[1], spec->naggs > 1 ? values[2] : (Datum) 0,
		Int64GetDatum((int64) stats.batches)};
		bool		on[4] = {nulls[0], false, spec->naggs < 2, false};

		tuplestore_putvalues(rsi->setResult, rsi->setDesc, out, on);
	}
	relation_close(rel, AccessShareLock);
	PG_RETURN_NULL();
}

/* (rel, is_table, att): whether the owner of the relation keeps the attribute as the codes of a dictionary */
Datum
sift_test_store_dict(PG_FUNCTION_ARGS)
{
	const Oid	relid = PG_GETARG_OID(0);
	const bool	is_table = PG_GETARG_BOOL(1);
	const SiftStoreOps *ops = sift_store_ops_for_oid(relid, is_table);

	PG_RETURN_BOOL(ops->dict != NULL && ops->dict->has_dict32(relid, is_table, PG_GETARG_INT32(2)));
}

/* ---- the parallel scan ---- */

typedef struct Participant
{
	int64		n[PAR_KEYS];
	int64		sum[PAR_KEYS];
	int64		batches;
	int64		runs;
} Participant;

typedef struct Shared
{
	ParQuery		q;
	pg_atomic_uint32 next_run;
	uint32		ngranules;
	uint32		nstaged;
	int			nparticipants;
	Size		staged_off;
	Participant part[FLEXIBLE_ARRAY_MEMBER];
} Shared;

static Size
shared_size(int nparticipants, uint32 nstaged)
{
	return add_size(add_size(offsetof(Shared, part), mul_size(nparticipants, sizeof(Participant))), mul_size(nstaged, sizeof(uint64)));
}

static uint64 *
staged_of(Shared *s)
{
	return (uint64 *) ((char *) s + s->staged_off);
}

static void
count_rows(void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	Participant *p = arg;

	p->batches++;
	for (int r = 0; r < bs->n_raw; r++)
	{
		const SiftVec *k = keyvec[0];
		int			slot;

		if (!bs->all_active && !((bs->mask[r >> 6] >> (r & 63)) & 1))
			continue;
		slot = !k->all_valid && !((k->valid[r >> 6] >> (r & 63)) & 1) ? PAR_KEYS - 1 : (int) ((const int64 *) k->data)[r];
		SIFT_INVARIANT(slot >= 0 && slot < PAR_KEYS, "the key of the parallel test is an integer between 0 and %d", PAR_KEYS - 2);
		p->n[slot]++;
		if (argvec[1] != NULL && (argvec[1]->all_valid || ((argvec[1]->valid[r >> 6] >> (r & 63)) & 1)))
			p->sum[slot] += ((const int64 *) argvec[1]->data)[r];
	}
}

static void
participate(Shared *s, int slot)
{
	Participant *p = &s->part[slot];
	const SiftParOps *par = sift_store_ops_for_oid(s->q.relid, false)->par;
	Relation	index = index_open(s->q.relid, AccessShareLock);
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	SiftV6ScanStats stats = {0};
	ScanKeyData key;
	const int	nkeys = index_keys(&s->q, &key);
	SiftV6Spec *spec = make_spec(&s->q);
	SiftV6Scan *scan = par->begin(spec, heap, index, &key, nkeys, GetActiveSnapshot(), &stats, count_rows, p);
	SiftParPartRef ref = {.g_base = 0,.ngranules = s->ngranules};
	uint32		run;

	par->morsel_begin(scan, staged_of(s), s->nstaged);
	while ((run = pg_atomic_fetch_add_u32(&s->next_run, 1)) * PAR_RUN < s->ngranules)
	{
		par->morsel(scan, NULL, &ref, run * PAR_RUN, Min(PAR_RUN, s->ngranules - run * PAR_RUN));
		p->runs++;
	}
	par->end(scan);
	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
}

void
sift_store_par_test_worker(dsm_segment *seg, shm_toc *toc)
{
	Shared	   *s = shm_toc_lookup(toc, KEY_SHARED, false);

	participate(s, ParallelWorkerNumber + 1);
}

/* (rel, key_att, sum_att, qual_att, qual_gt, workers): the workers launched, then the groups of the key: its value, its count and sum, and the runs the participants took (the same on every row) */
Datum
sift_test_store_par(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	const ParQuery q = {.relid = PG_GETARG_OID(0),.is_table = false,.key_att = PG_GETARG_INT32(1),.sum_att = PG_ARGISNULL(2) ? 0 : PG_GETARG_INT32(2),
		.qual_att = PG_ARGISNULL(3) ? 0 : PG_GETARG_INT32(3),.qual_gt = PG_ARGISNULL(4) ? 0 : PG_GETARG_INT64(4)};
	const int	workers = PG_GETARG_INT32(5);
	const SiftParOps *par = sift_store_ops_for_oid(q.relid, false)->par;
	Relation	index = index_open(q.relid, AccessShareLock);
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	SiftV6ScanStats stats = {0};
	ScanKeyData key;
	const int	nkeys = index_keys(&q, &key);
	SiftV6Scan *scan = par->begin(make_spec(&q), heap, index, &key, nkeys, GetActiveSnapshot(), &stats, NULL, NULL);
	SiftF2ParPlan plan;
	ParallelContext *pcxt;
	Shared	   *s;
	Size		size;
	int64		runs = 0,
				n[PAR_KEYS] = {0},
				sum[PAR_KEYS] = {0};
	int			launched;

	InitMaterializedSRF(fcinfo, 0);
	par->plan(scan, &plan);
	size = shared_size(workers + 1, plan.nstaged);
	EnterParallelMode();
	pcxt = CreateParallelContext("pg_sift", "sift_store_par_test_worker", workers);
	shm_toc_estimate_chunk(&pcxt->estimator, size);
	shm_toc_estimate_keys(&pcxt->estimator, 1);
	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
		elog(ERROR, "no shared memory for the parallel test");
	s = shm_toc_allocate(pcxt->toc, size);
	memset(s, 0, size);
	s->q = q;
	pg_atomic_init_u32(&s->next_run, 0);
	s->ngranules = plan.nrefs > 0 ? plan.refs[0].ngranules : 0;
	s->nstaged = plan.nstaged;
	s->nparticipants = workers + 1;
	s->staged_off = shared_size(workers + 1, 0);
	memcpy(staged_of(s), plan.staged_dead, plan.nstaged * sizeof(uint64));
	shm_toc_insert(pcxt->toc, KEY_SHARED, s);
	LaunchParallelWorkers(pcxt);
	launched = pcxt->nworkers_launched;
	participate(s, 0);
	WaitForParallelWorkersToFinish(pcxt);
	for (int i = 0; i <= workers; i++)
	{
		runs += s->part[i].runs;
		for (int k = 0; k < PAR_KEYS; k++)
		{
			n[k] += s->part[i].n[k];
			sum[k] += s->part[i].sum[k];
		}
	}
	for (int k = 0; k < PAR_KEYS; k++)
		if (n[k] > 0)
		{
			Datum		values[5] = {Int32GetDatum(launched), k == PAR_KEYS - 1 ? (Datum) 0 : Int64GetDatum(k), Int64GetDatum(n[k]), Int64GetDatum(sum[k]), Int64GetDatum(runs)};
			bool		nulls[5] = {false, k == PAR_KEYS - 1, false, q.sum_att == 0, false};

			tuplestore_putvalues(rsi->setResult, rsi->setDesc, values, nulls);
		}
	DestroyParallelContext(pcxt);
	ExitParallelMode();
	par->end(scan);
	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	PG_RETURN_NULL();
}

#endif							/* SIFT_TESTING */
