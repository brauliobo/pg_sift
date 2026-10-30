/*
 * agg/dist/stream.c - count(DISTINCT): the stream of a sorted distinct column
 * The superblocks in the order of a distinct column's values and the scan that counts the
 * distinct values of a group from them.
 */
#include "postgres.h"

#include "miscadmin.h"

#include "agg/agg.h"
#include "agg/dist/int.h"

/*
 * A streaming count(DISTINCT x): the superblocks are visited in ascending order of the zone minimum of x, so once the ones before are done no
 * value below the minimum of the next can come again, and the set of the groups drops what is below it (dist_compact, when the set would
 * outgrow hash memory). The count of a group is taken when a value is first seen, so nothing is lost on the way. With x close to the heap order
 * the set holds the values of the superblocks whose zones overlap, not all the values of the scan; with x at random in every superblock
 * nothing is dropped and the scan is the one without the order (the choice below sees that in the zones, before any row is read).
 */
typedef struct DsSb
{
	int64		min;
	int64		max;
	uint32		sb;
	uint32		rows;
} DsSb;

static int
dssb_by_min(const void *a, const void *b)
{
	const DsSb *x = a,
			   *y = b;

	if (x->min != y->min)
		return x->min < y->min ? -1 : 1;
	return x->sb < y->sb ? -1 : x->sb > y->sb;
}

static int
dssb_by_max(const void *a, const void *b)
{
	const DsSb *x = a,
			   *y = b;

	return x->max < y->max ? -1 : x->max > y->max;
}

/* Is the scan one that streams, and in what order? Sets dsorder, dsmin, ndsorder and dsagg; false: the scan goes on as it would (with workers) */
bool
dist_stream_begin(AggExec *ex)
{
	const SiftAggSpec *s = ex->spec;
	int			ai = sift_aggspec_dstream(s);
	double		mem = (double) get_hash_memory_limit(),
				total = 0,
				done = 0,
				expired = 0,
				before = 0,
				peak = 0;
	uint32		nsb,
				n = 0,
				p = 0;
	DsSb	   *z,
			   *zm;
	int			leaf;

	ex->ndsorder = 0;
	if (ex->dsorder)
	{
		pfree(ex->dsorder);
		pfree(ex->dsmin);
		ex->dsorder = NULL;
		ex->dsmin = NULL;
	}
	if (ai < 0 || !sift_agg_dist_stream || ex->be != &sift_agg_cpu_backend || ex->search || ex->topn || !sift_colsrc_has_zone_range(ex->src))
		return false;
	leaf = s->agg[ai].leaf;
	if (leaf >= ex->nbase || ex->lay->leaf[leaf].vtype != SCV_I32)
		return false;
	/* a table of groups far past hash memory is the spill's to handle, not this */
	if (s->est_groups * 256.0 > 4.0 * mem)
		return false;
	nsb = sift_colsrc_nsb(ex->src);
	/* the write buffers are read in by the first begin; until then a range says it may hold anything */
	sift_colsrc_begin(ex->src, 0, 1, ex->prune, ex->nzone);
	z = palloc(Max(nsb, 1) * sizeof(DsSb));
	for (uint32 sb = ex->tid_sb_lo; sb < Min(nsb, ex->tid_sb_hi); sb++)
	{
		SiftSrcZone zone;
		DsSb	   *e = &z[n];

		if (sift_colsrc_zone_range(ex->src, sb, sb + 1, leaf, &zone) == 0)
			continue;
		e->sb = sb;
		if (zone.inexact)
		{
			e->min = PG_INT64_MIN;
			e->max = PG_INT64_MAX;
			e->rows = zone.nvals;
		}
		else if (zone.nnull >= zone.nvals)
		{
			e->min = PG_INT64_MAX;
			e->max = PG_INT64_MIN;
			e->rows = 0;
		}
		else
		{
			e->min = zone.min;
			e->max = zone.max;
			e->rows = zone.nvals - zone.nnull;
		}
		total += e->rows;
		n++;
	}
	/* a set of at most this many 16-byte pairs fits hash memory twice over: the scan with workers is the better one */
	if (n < 2 || total * 16.0 <= mem / 2)
	{
		pfree(z);
		return false;
	}
	qsort(z, n, sizeof(DsSb), dssb_by_min);
	zm = palloc(n * sizeof(DsSb));
	memcpy(zm, z, n * sizeof(DsSb));
	qsort(zm, n, sizeof(DsSb), dssb_by_max);
	/* the pairs the sweep holds at most: the rows of the superblocks done, less those whose zone ends below the minimum of the next */
	for (uint32 k = 0; k < n; k++)
	{
		int64		m = k + 1 < n ? z[k + 1].min : PG_INT64_MAX;

		peak = Max(peak, before + z[k].rows);
		done += z[k].rows;
		for (; p < n && zm[p].max < m; p++)
			expired += zm[p].rows;
		before = done - expired;
	}
	pfree(zm);
	if (peak * 2.0 > total)
	{
		pfree(z);
		return false;
	}
	ex->dsorder = MemoryContextAlloc(ex->cxt, n * sizeof(uint32));
	ex->dsmin = MemoryContextAlloc(ex->cxt, n * sizeof(int64));
	for (uint32 k = 0; k < n; k++)
	{
		ex->dsorder[k] = z[k].sb;
		ex->dsmin[k] = z[k].min;
	}
	pfree(z);
	ex->ndsorder = n;
	ex->dsagg = ai;
	return true;
}

/*
 * The sweep. The spill of the group table is held back: a partial state carries the set of a group, and the pairs this scan dropped are not in
 * it. If the table would have spilled the sweep is given up and the scan starts over as it would without it.
 */
void
dist_stream_scan(AggExec *ex)
{
	struct AggSpill *sp = ex->spill;

	ex->spill = NULL;
	for (uint32 k = 0; k < ex->ndsorder; k++)
	{
		scan_units(ex, ex->dsorder[k], ex->dsorder[k] + 1);
		arms_flush(ex);
		if (sp && spill_would(ex, sp))
		{
			ex->spill = sp;
			ex->dsabort = true;
			table_reset(ex);
			ex->n_units = ex->n_pre_units = ex->n_super = ex->n_rows = ex->n_tuples = 0;
			pg_atomic_write_u32(&ex->shared->next, 0);
			scan_items(ex);
			return;
		}
		ex->dist[ex->dsagg]->wmark = k + 1 < ex->ndsorder ? ex->dsmin[k + 1] : PG_INT64_MAX;
	}
	ex->spill = sp;
}
