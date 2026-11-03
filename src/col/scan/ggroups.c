/*
 * col/scan/ggroups.c - the values of the group column of a scan that reads the groups of a second order one after the other (an index-only scan ordered by the group and the key)
 */
#include "postgres.h"
#include "col/point.h"
#include "col/wbuf.h"
#include "core/cmp.h"
#include "col/scan_int.h"

static int
gorder_cmp_i64(const void *a, const void *b)
{
	return pg_cmp_s64(*(const int64 *) a, *(const int64 *) b);
}

/* The ascending values a (na) and b (nb) as one ascending run of each value once, in out; the length */
static uint64
gorder_merge_runs(const int64 *a, uint64 na, const int64 *b, uint64 nb, int64 *out)
{
	uint64		i = 0,
				j = 0,
				n = 0;

	while (i < na || j < nb)
	{
		int64		v = j >= nb || (i < na && a[i] <= b[j]) ? a[i] : b[j];

		if (n == 0 || out[n - 1] != v)
			out[n++] = v;
		i += i < na && a[i] == v;
		j += j < nb && b[j] == v;
	}
	return n;
}

/*
 * The values of the leaf gcol that the rows of the scan have, ascending, once each: the groups of the second order of leaf kcol grouped by gcol in every stored segment (a block of every
 * row, so that the table of groups lists every value) and the rows of the write buffers. The groups of a scan that reads kcol group after group, in the order of gcol (an index-only scan
 * ordered by both). False when a segment has no such block, or a buffer holds an overflow or no value of gcol: the order of the pair is not there to read. With only, the one group it
 * names is the result (a scan with g = value reads that group alone) and the groups of the segments are not listed.
 */
bool
sift_col_gorder_group_values(SiftColSource *src, int gcol, int kcol, const int64 *only, int64 **vals, uint32 *nvals)
{
	uint64		total = 0,
				longest = 0,
				memrows = 0,
				n = 0;
	int64	   *acc,
			   *run,
			   *tmp;

	for (int j = 0; j < src->nseg; j++)
	{
		int			slot = sift_cseg_gorder_slot(src->seg[j].seg, kcol, true);
		uint32		entries,
					values;

		if (slot < 0 || sift_cseg_gorder_group(src->seg[j].seg, slot) != gcol || !sift_cseg_gorder(src->seg[j].seg, slot, &entries, &values) || entries != (uint32) src->seg[j].seg->entry.ndocs)
			return false;
		if (only != NULL)
			continue;
		sift_cseg_gorder_group_table(src->seg[j].seg, slot);
		longest = Max(longest, sift_cseg_gorder_ngroups(src->seg[j].seg, slot));
		total += sift_cseg_gorder_ngroups(src->seg[j].seg, slot);
	}
	if (!src->mem_loaded)
		scan_load_wbuf(src);
	for (int i = 0; i < src->nmem; i++)
	{
		const SiftColBatch *b = src->mem[i].b;
		const SiftCVals *gv = &b->col[gcol];

		for (uint32 r = 0; r < b->nrows; r++)
			if ((gv->ovf && (gv->ovf[r / 64] >> (r % 64) & 1)) || (gv->nulls && (gv->nulls[r / 64] >> (r % 64) & 1)))
				return false;
		total += b->nrows;
		memrows += b->nrows;
	}
	if (only != NULL)
	{
		*vals = palloc(sizeof(int64));
		(*vals)[0] = *only;
		*nvals = 1;
		return true;
	}
	/* the runs of the segments, each ascending, are merged one into the result: a few runs of many groups */
	acc = palloc((total + 1) * sizeof(int64));
	run = palloc((Max(longest, memrows) + 1) * sizeof(int64));
	tmp = palloc((total + 1) * sizeof(int64));
	for (int j = 0; j < src->nseg; j++)
	{
		int			slot = sift_cseg_gorder_slot(src->seg[j].seg, kcol, true);
		uint32		ng = sift_cseg_gorder_ngroups(src->seg[j].seg, slot);
		int64	   *t;

		for (uint32 gi = 0; gi < ng; gi++)
		{
			uint32		vs,
						ve,
						ns,
						ne;

			sift_cseg_gorder_group_at(src->seg[j].seg, slot, gi, &run[gi], &vs, &ve, &ns, &ne);
		}
		n = gorder_merge_runs(acc, n, run, ng, tmp);
		t = acc;
		acc = tmp;
		tmp = t;
	}
	if (src->nmem > 0)
	{
		uint64		m = 0;
		int64	   *t;

		for (int i = 0; i < src->nmem; i++)
		{
			const SiftColBatch *b = src->mem[i].b;

			for (uint32 r = 0; r < b->nrows; r++)
				run[m++] = b->col[gcol].key[r];
		}
		qsort(run, m, sizeof(int64), gorder_cmp_i64);
		n = gorder_merge_runs(acc, n, run, m, tmp);
		t = acc;
		acc = tmp;
		tmp = t;
	}
	pfree(run);
	pfree(tmp);
	*vals = acc;
	*nvals = (uint32) n;
	return true;
}
