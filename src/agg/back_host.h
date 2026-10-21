/*
 * agg/back_host.h - the part of agg/agg.c that serves the backends of agg/back.h: the view of the scan they see, the fold of the sums of a
 * unit they took, and the second run of a scan whose backend failed. Not a header: it is included by agg/agg.c where the group table, the key
 * ids and the pre-aggregate fold (which it uses) are visible.
 */

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
static bool
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
static void
backend_finish(AggExec *ex)
{
	if (!sift_aggbe_finish(ex->be, ex->be_state))
		return;
	table_reset(ex);
	ex->n_units = ex->n_pre_units = ex->n_rows = ex->n_tuples = 0;
	pg_atomic_write_u32(&ex->shared->next, 0);
	scan_items(ex);
}
