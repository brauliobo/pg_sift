/*
 * col/f2/scope/test.c - the test-only SQL functions that read the scope summaries of an index of format major 2 (col/f2/scope/scope.h) the way a query will: the cube of every key, the
 * newest rows of a key, the state of the object.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "col/f2/scope/scope.h"
#include "col/f2/test_int.h"

PG_FUNCTION_INFO_V1(sift_test_f2_scope_keys);
PG_FUNCTION_INFO_V1(sift_test_f2_scope_latest);
PG_FUNCTION_INFO_V1(sift_test_f2_scope_info);
PG_FUNCTION_INFO_V1(sift_test_f2_scope_current);
PG_FUNCTION_INFO_V1(sift_test_f2_scope_refresh);

static double
measure_sum(const SiftScopeCubeAcc *c, uint32 m)
{
	return (c->shape.float_meas >> m & 1) ? c->leaf[m].sum_f : (double) c->leaf[m].sum;
}

/* (index, scope, month): for every key of the summary the rows, the time bounds and the sums of the measures of its cube, month 0 for all time; the deletions a snapshot sees are taken out */
Datum
sift_test_f2_scope_keys(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	ReturnSetInfo *rsi = f2t_srf_begin(fcinfo);
	SiftF2Scope *sc = sift_f2_scope_open(rel, PG_GETARG_INT32(1), GetActiveSnapshot(), 0);
	int32		month = PG_ARGISNULL(2) ? SIFT_SCOPE_NO_MONTH : PG_GETARG_INT32(2);
	SiftScopeQueryNeeds q = {.minmax = false,.month = month,.distinct = false};

	if (sc == NULL)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("index \"%s\" has no summary for scope %d", RelationGetRelationName(rel), PG_GETARG_INT32(1))));
	for (uint32 i = 0; i < sc->meta.hdr.nkeys; i++)
	{
		const SiftScopeL2Root *root = &sc->meta.roots[i];
		SiftScopeCubeAcc cube;
		SiftScopeDeltaStats st = {0};
		Datum		v[5];
		bool		n[5] = {0};
		Datum	   *sums;

		if (!sift_scope_cube_live(&sc->meta, root, &q, &sc->entity, &cube, &st))
			continue;
		sums = palloc(Max(cube.shape.nmeas, 1) * sizeof(Datum));
		for (uint32 m = 0; m < cube.shape.nmeas; m++)
			sums[m] = Float8GetDatum(measure_sum(&cube, m));
		v[0] = Int64GetDatum(root->scope_key);
		v[1] = Int64GetDatum((int64) cube.row_count);
		v[2] = Int64GetDatum(cube.has_time ? cube.time_min : 0);
		v[3] = Int64GetDatum(cube.has_time ? cube.time_max : 0);
		v[4] = PointerGetDatum(sift_construct_float8_array(sums, cube.shape.nmeas));
		n[2] = n[3] = !cube.has_time;
		f2t_srf_put(rsi, v, n);
		sift_scube_free(&cube);
	}
	sift_f2_scope_close(sc);
	index_close(rel, NoLock);
	return (Datum) 0;
}

/* (index, scope, key, n): the times of the newest n rows of the key, newest first */
Datum
sift_test_f2_scope_latest(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftF2Scope *sc = sift_f2_scope_open(rel, PG_GETARG_INT32(1), GetActiveSnapshot(), 0);
	const SiftScopeL2Root *root;
	uint32		n = PG_GETARG_INT32(3),
				got,
				ring;
	SiftScopeNewest *out;
	Datum	   *times;
	ArrayType  *a;

	if (sc == NULL)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("index \"%s\" has no summary for scope %d", RelationGetRelationName(rel), PG_GETARG_INT32(1))));
	root = sift_scope_l2_find(&sc->meta, PG_GETARG_INT64(2));
	out = palloc(Max(n, 1) * sizeof(SiftScopeNewest));
	got = root == NULL ? 0 : sift_scope_latest(&sc->meta, root, n, false, &sc->entity, out, &ring);
	times = palloc(Max(got, 1) * sizeof(Datum));
	for (uint32 i = 0; i < got; i++)
		times[i] = Int64GetDatum(out[i].time);
	a = construct_array_builtin(times, got, INT8OID);
	sift_f2_scope_close(sc);
	index_close(rel, NoLock);
	PG_RETURN_ARRAYTYPE_P(a);
}

/* (index, scope): "keys parts granules flags rings bytes" of the object and the refreshes this backend has made (whole/parts/pairs) */
Datum
sift_test_f2_scope_info(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	SiftF2Scope *sc = sift_f2_scope_open(rel, PG_GETARG_INT32(1), GetActiveSnapshot(), 0);
	text	   *t;

	if (sc == NULL)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("index \"%s\" has no summary for scope %d", RelationGetRelationName(rel), PG_GETARG_INT32(1))));
	t = cstring_to_text(psprintf("keys=%u parts=%u granules=%llu flags=%x rings=%u bytes=%llu refreshes=%d/%d/%d combined=%d sections=l0:%llu l1:%llu topk:%llu monthmap:%llu l2:%llu keydir:%llu dict:%llu route:%llu",
								 sc->meta.hdr.nkeys, sc->meta.hdr.nparts,
								 (unsigned long long) sc->meta.hdr.total_granules, sc->meta.hdr.flags, sc->meta.ntopk_rings, (unsigned long long) sc->meta.len,
								 sift_f2_scope_refreshes.full, sift_f2_scope_refreshes.parts, sift_f2_scope_refreshes.dirty, sift_f2_scope_refreshes.combined,
								 (unsigned long long) sc->meta.l0.len, (unsigned long long) sc->meta.l1.len, (unsigned long long) sc->meta.topk.len,
								 (unsigned long long) sc->meta.monthmap.len, (unsigned long long) sc->meta.l2.len, (unsigned long long) sc->meta.keydir.len,
								 (unsigned long long) sc->meta.dict.len, (unsigned long long) sc->meta.ent_route.len));
	sift_f2_scope_close(sc);
	index_close(rel, NoLock);
	PG_RETURN_TEXT_P(t);
}

Datum
sift_test_f2_scope_current(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, AccessShareLock);
	bool		current = sift_f2_scope_current(rel, PG_GETARG_INT32(1));

	index_close(rel, NoLock);
	PG_RETURN_BOOL(current);
}

/* (index): the summaries that no longer described the parts or the pairs, built again */
Datum
sift_test_f2_scope_refresh(PG_FUNCTION_ARGS)
{
	Relation	rel = f2t_open_index(fcinfo, RowExclusiveLock);
	int			n = sift_f2_scope_refresh(rel);

	index_close(rel, NoLock);
	PG_RETURN_INT32(n);
}

#endif							/* SIFT_TESTING */
