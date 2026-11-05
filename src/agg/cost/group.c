/*
 * agg/cost/group.c - the groups of the scan
 * Their bytes and their number, the spill of a group table, the vetting of the rows and
 * the workers that share the scan.
 */
#include "postgres.h"

#include <math.h>

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"

#include "agg/acc_layout.h"
#include "agg/cost/int.h"

/* what the cost of a parallel scan is divided by: the workers and the leader, which scans as one of them (it passes no tuples: 3 participants run 1.5 times faster than 2, with the leader out of the scan 2 workers took 13.6 ms against 8.9) */
double
parallel_divisor(int workers)
{
	return workers + (parallel_leader_participation ? 1.0 : 0.0);
}

/*
 * Each process aggregates its share into a table of its own; the leader merges them. The lookup tables are
 * read once, by the leader, who writes them to the shared memory (agg/lookshare.c): the divisor is not theirs.
 */
void
cost_parallel(AggCtx *c, SiftAggSpec *s, CostAcc *k, Cost *startup)
{
	double		tables = s->nworkers + 1;
	double		load = Min(k->load_cost * cpu_operator_cost, k->scan_cost);
	double		merge = AGG_MERGE_GROUP;
	double		setup = parallel_setup_cost * AGG_SETUP_FACTOR;
	bool		priced = parallel_setup_cost > 0;	/* 0 asks for the workers, whatever they save */

	/*
	 * Starting the workers takes 3 ms and 0.5 ms for each, which a low parallel_setup_cost prices below the plans of a few milliseconds; the
	 * leader merges the pairs of a count(DISTINCT) into its own sets one by one, so they do not scale with the workers (below)
	 */
	for (int j = 0; j < s->nkeys; j++)
		merge += AGG_MERGE_KEY + (get_typbyval(s->key[j].typid) ? 0.0 : AGG_MERGE_STR);
	if (priced)
		setup = Max(setup, (AGG_START_BASE_MS + AGG_START_WORKER_MS * s->nworkers) * AGG_START_MS);
	/* the participants that split the groups by a key each read every row and aggregate a share of the keys, and the leader adds their disjoint tables to its own */
	k->scan_cost = (k->scan_cost - load + (priced ? k->cold_ops * AGG_SCAN_SPEED * cpu_operator_cost : 0)) *
		(s->gpart > 0 ? AGG_SPLIT_SHARE + (1.0 - AGG_SPLIT_SHARE) / parallel_divisor(s->nworkers) : 1.0 / parallel_divisor(s->nworkers)) + load + k->load_rows * AGG_SHARE_ROW * AGG_SCAN_SPEED * cpu_operator_cost +
		(s->gpart > 0 ? Min(k->rows_out, k->agg_rows) : tables * Min(k->rows_out, Max(k->agg_rows / tables, 1.0))) * (priced ? merge : 0.0) * AGG_SCAN_SPEED * cpu_operator_cost;
	if (priced && s->gpart == 0)
		k->scan_cost += Min(k->rows_out, Max(k->agg_rows / tables, 1.0)) * AGG_TABLE_GROUP * AGG_SCAN_SPEED * cpu_operator_cost;
	for (int i = 0; i < s->naggs; i++)
	{
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			k->scan_cost += Min(k->agg_rows, k->rows_out * s->agg[i].est_distinct) * AGG_ORDERED_MERGE * AGG_SCAN_SPEED * cpu_operator_cost;
		if (priced && s->agg[i].kind == SAA_COUNT_DISTINCT)
		{
			double		nd = distinct_text_values(c, &s->agg[i], k);

			k->scan_cost += k->agg_rows * AGG_DISTINCT_MERGE * AGG_SCAN_SPEED * cpu_operator_cost;
			if (nd > 0)
				k->scan_cost += s->nworkers * nd * -expm1(-k->agg_rows / tables / nd) * AGG_DIST_SEND * AGG_SCAN_SPEED * cpu_operator_cost;
		}
		/* the pairs of the sets of a joined column are at most one a row, and at most the values of each group: a column of few values sends few */
		if (priced && s->agg[i].kind == SAA_M_DISTINCT)
			k->scan_cost += Min(k->agg_rows, k->rows_out * s->agg[i].est_distinct) * AGG_DISTINCT_MERGE * AGG_SCAN_SPEED * cpu_operator_cost;
	}
	*startup += setup * k->lift;
	c->par_start = setup * k->lift;
}

/*
 * The bytes a group holds in the table of the scan: its keys' ids and values, its row in the group arrays, the arrays' room to grow.
 * *state: of them, what a partial state of the group carries when the table is written out (its aggregates and its key values).
 */
static double
group_bytes(const SiftAggSpec *s, double *state)
{
	double		b = sizeof(int64);	/* seen */
	double		vals = 0;

	for (int i = 0; i < s->naggs; i++)
		b += sift_agg_acc_layout_bytes(sift_agg_acc_layout(&s->agg[i], false));
	*state = b;
	/* the id tuple of the hash table and its slot, at most half full */
	b += s->nkeys * sizeof(uint32) + 4 * sizeof(uint32);
	for (int j = 0; j < s->nkeys; j++)
		vals += get_typavgwidth(s->key[j].typid, s->key[j].typmod);
	*state += vals;
	return b * AGG_GROUP_SLACK + s->nkeys * AGG_GROUP_KEY_BYTES + vals;
}

/*
 * The groups the scan makes when PostgreSQL's rows of its join are the floor it gives an unnesting function over a lookup it has no statistics for (a CTE or
 * sub-select of ids is one row, the function 100): the rows of the table keep hundreds of thousands, and the groups cannot be more than they hold. The tuples that
 * reach the groups are taken at the geometric mean of the floor and the elements of the table's rows (the lookup can only narrow them), and the groups are no
 * more than the combinations of the values the keys read: the distinct values of the index for a leaf (the fields of one element go together), PostgreSQL's
 * statistics for a column, the buckets of a calendar key. 0: PostgreSQL's rows stand (grouping sets, a key no leaf gives, a leaf whose values are not known).
 */
double
tuple_group_count(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel)
{
	double		rows = Max(input_rel->rows, 1.0),
				scan = Max(c->rel->rows, 1.0) * (s->group >= 0 ? elems_of(c, s->group) : 1.0),
				nd = 1.0,
				parent_nd[SIFT_COLSRC_MAX_LEAVES] = {0};
	Bitmapset  *seen = NULL;
	int			nbase = c->lay->nleaves - c->nvleaf;

	if (s->group < 0 || s->nsets > 0 || rows > AGG_UNNEST_FLOOR || scan <= rows)
		return 0;
	for (int j = 0; j < s->nkeys; j++)
	{
		const SiftAggKey *k = &s->key[j];
		double		b = key_groups(c, k);
		int			leaves[SIFT_AGG_MAX_CLEAVES],
					nleaves = 0;

		if (b > 0)
		{
			nd *= b;
			continue;
		}
		if (k->kind == SAK_LEAF || k->kind == SAK_LUT)
			leaves[nleaves++] = k->leaf;
		else if (k->kind == SAK_COMBO)
			for (int i = 0; i < s->combo[k->leaf].nleaves; i++)
				if (s->combo[k->leaf].leaf[i] >= 0)
					leaves[nleaves++] = s->combo[k->leaf].leaf[i];
		if (nleaves == 0)
			return 0;
		for (int i = 0; i < nleaves; i++)
		{
			const SiftSrcLeaf *lf;
			Node	   *e;
			double		d;

			if (leaves[i] >= nbase || bms_is_member(leaves[i], seen))
				continue;
			seen = bms_add_member(seen, leaves[i]);
			lf = &c->lay->leaf[leaves[i]];
			if (lf->parent >= 0 || lf->path != NIL)
			{
				if ((d = c->stats.ndistinct[leaves[i]]) <= 0)
					return 0;
				if (lf->parent >= 0)
				{
					parent_nd[lf->parent] = Max(parent_nd[lf->parent], d);
					continue;
				}
			}
			else if ((e = list_nth(c->attexpr, lf->attno - 1)) == NULL || !IsA(e, Var) || (d = column_distinct(c, (Var *) e)) <= 0)
				return 0;
			nd *= d;
		}
	}
	for (int i = 0; i < SIFT_COLSRC_MAX_LEAVES; i++)
		if (parent_nd[i] > 0)
			nd *= parent_nd[i];
	return Max(Min(sqrt(rows * scan), nd), 1.0);
}

/*
 * The groups the table of the scan can hold: the estimate, and no more than the elements the index has (the normal plan expects 100 rows of
 * every call of an unnesting function, and the groups of a scan of 12,000 rows are not a million)
 */
double
group_count(AggCtx *c, const SiftAggSpec *s)
{
	double		rows = Max(c->stats.nrows, Max(c->rel->tuples, 1.0)) * (s->group >= 0 ? elems_of(c, s->group) : 1.0);

	return Min(Max(s->est_groups, 1.0), Max(rows, 1.0));
}

/* The bytes of the group table of the scan for the groups it is expected to hold. *state: of a group, what its partial state carries */
double
group_table_bytes(const SiftAggSpec *s, double groups, double *state)
{
	return groups * group_bytes(s, state);
}

/*
 * The group table of a spillable spec (agg/spill.c) that outgrows the memory a hash aggregate gets writes the partial states of its groups
 * to partitions on disk and reads them back by the end: the price is that of the spill of the normal plan (cost_agg): the pages of what is
 * written and read back and the tuples through them. A group in no partition is in the table: the states written are the share of the
 * rows whose group is not in it, and no more than a state for each group of each time the table fills. The table holds the ids of the keys
 * (key_entries), not only the groups that have rows: it fills with them, and each time it is walked for the groups it writes.
 */
double
group_spill_cost(const SiftAggSpec *s, double groups, double ids, double rows, double *table)
{
	double		mem = (double) get_hash_memory_limit(),
				state,
				runs,
				written;

	*table = group_table_bytes(s, Max(groups, ids), &state);
	if (*table <= mem || !sift_aggspec_spillable(s))
		return 0;
	runs = ceil(*table / mem);
	written = Min(rows * (1.0 - mem / *table), groups * runs);
	return written * state / BLCKSZ * (random_page_cost + seq_page_cost) + 2.0 * cpu_tuple_cost * written +
		written * log2(runs + 1.0) * 2.0 * cpu_operator_cost + runs * ids * AGG_ID_SWEEP * cpu_operator_cost;
}

/* the shares of a table's pages that shared buffers hold, and that those and the cache of the operating system (effective_cache_size) hold */
static void
page_residency(double pages, double *buffered, double *cached)
{
	*buffered = Min(NBuffers / pages, 1.0);
	*cached = Max(Min(effective_cache_size / pages, 1.0), *buffered);
}

/* the operators one visit to a heap page costs a read through an index in the order of its keys: found in the buffers, in the cache of the operating system, or read from the disk */
double
heap_visit_ops(double pages)
{
	double		buffered,
				cached;

	pages = Max(pages, 1.0);
	page_residency(pages, &buffered, &cached);
	return buffered * AGG_VISIT_PAGE * AGG_SCAN_SPEED + (cached - buffered) * AGG_VISIT_OS_PAGE * AGG_SCAN_SPEED + (1.0 - cached) * random_page_cost / cpu_operator_cost;
}

/*
 * The heap check of the rows on pages that are not all-visible (vis is their share), a page at a time: the pages of the units the zones keep
 * (zone_kept; a unit cut by a declared key spans the whole heap, whatever its zone) that hold a selected row, which the rows reach by chance (Poisson). A page is in shared buffers, in the cache the operating system keeps
 * (PostgreSQL's own assumption for it is effective_cache_size), or read from the disk: a sequential read when the scan reaches most of the pages it
 * passes and a random one when it skips them.
 */
double
vet_cost(AggCtx *c, double vis, double sel_rows, double zone_kept)
{
	double		pages = Max(rel_pages(c), 1.0),
				kept = zone_kept + (1.0 - zone_kept) * c->stats.keyed_frac,
				vet_pages = vis * Min(Max(pages * kept, 1.0), pages),
				rows = vis * sel_rows,
				buffered,
				cached,
				touched,
				seq,
				disk,
				op = AGG_SCAN_SPEED * cpu_operator_cost;

	if (vet_pages <= 0)
		return 0;
	page_residency(pages, &buffered, &cached);
	touched = vet_pages * (1.0 - exp(-rows / vet_pages));
	/* the pages are read in order and skip the all-visible ones: never dearer than the random fetch of the same pages, where a configuration prices a sequential page above a random one */
	seq = Min(seq_page_cost, random_page_cost);
	disk = seq + (random_page_cost - seq) * (1.0 - touched / vet_pages);
	return touched * (buffered * AGG_VET_PAGE * op + (cached - buffered) * AGG_VET_OS_PAGE * op + (1.0 - cached) * disk) + rows * AGG_VET_ROW * op;
}

/* The tables of the IN lists the scan builds when it starts: they depend on the range of the values, not on the rows read or the units kept */
double
in_tables_cost(AggCtx *c, const SiftAggPred *p)
{
	double		bytes = 0;

	if (p->kind == SAP_AND || p->kind == SAP_OR)
		for (int i = 0; i < p->nchild; i++)
			bytes += in_tables_cost(c, p->child[i]);
	else
		bytes = in_list_table_bytes(c, p) * AGG_IN_TABLE_BYTE * cpu_operator_cost;
	return bytes;
}

/*
 * PostgreSQL's count of a join of lookups that it has no statistics for (a CTE or sub-select of ids is estimated through its own defaults: 100 rows where
 * 1,000,000 pass) caps the tuples that reach the groups (cost_tuples), and the scan is priced at milliseconds when it takes hundreds, so the workers it
 * would need (3 to 4 ms to start) never pay. The price of the scan stays the capped one, which is what the plan above and the normal plan are priced
 * with; the path with workers is priced at the share of it that the workers take of the scan when the tuples are the rows the scan reads: the
 * same scan, serial and with workers, priced without the cap, and the capped serial price times their ratio. The start of the workers is a time, not a
 * share of the scan (3.4 ms for two, whatever they read): it is added after the ratio, or a scan of 3 ms that the cap prices at 2,600 units took
 * workers that cost 2 ms more than it.
 */
void
workers_ratio(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, Path *p, double rows_out, double out_cost)
{
	SiftAggSpec serial = *s;
	Path		sc = {0},
				sw = {0},
				pw = {0};
	double		total;

	serial.nworkers = 0;
	pricing_scratch = true;
	PG_TRY();
	{
		agg_cost(c, &serial, input_rel, &sc, rows_out);
		pricing_wide = true;
		agg_cost(c, &serial, input_rel, &sw, rows_out);
		agg_cost(c, s, input_rel, &pw, rows_out);
	}
	PG_FINALLY();
	{
		pricing_wide = pricing_scratch = false;
	}
	PG_END_TRY();
	if (sw.total_cost <= 0 || pw.total_cost >= sw.total_cost)
		return;
	total = sc.total_cost * (pw.total_cost - c->par_start) / sw.total_cost + c->par_start;
	if (total < p->total_cost)
	{
		p->total_cost = total;
		p->startup_cost = Max(total - out_cost, 0.0);
		s->wide_unit_rows = Max(c->stats.nrows / Max(c->stats.nunits, 1.0), 1.0);
	}
}

/*
 * The time PostgreSQL leaves out of a hash aggregate whose table outgrows the cache of a core: it prices a row at one operator whatever the groups, and a row
 * is a hash, a probe that misses (the bucket, the stored key), and the table doubling as it fills. In its units, for the rows the path hashes; 0 for a table
 * a core's cache holds. A table that spills keeps the memory of a hash aggregate (cost_agg prices the pages it writes).
 */
double
hash_agg_extra(const AggPath *a)
{
	double		groups = Max(a->numGroups, 1.0),
				width = 0,
				bytes,
				misses;
	PathTarget *pt = a->subpath->pathtarget;
	bool		byval = true;
	int			i = 0;

	foreach_ptr(Expr, e, pt->exprs)
	{
		if (get_pathtarget_sortgroupref(pt, i++) != 0)
		{
			Oid			t = exprType((Node *) e);
			int16		len;
			bool		bv;

			get_typlenbyval(t, &len, &bv);
			byval &= bv;
			width += get_typavgwidth(t, exprTypmod((Node *) e));
		}
	}
	bytes = Min(groups * (width + AGG_HASH_ENTRY), (double) get_hash_memory_limit());
	if (bytes <= AGG_HASH_CACHE)
		return 0;
	misses = (AGG_HASH_MISS + (byval ? 0 : AGG_HASH_MISS_KEY + 0.02 * width)) * bytes / (bytes + AGG_HASH_CACHE);
	return a->subpath->rows * (misses * AGG_HASH_MISS_NS + AGG_HASH_GROW_NS * log2(groups)) * 1e-6 * AGG_NORMAL_MS;
}

static bool
table_input(const Path *p)
{
	return p->parent->reloptkind != RELOPT_BASEREL || p->parent->rtekind == RTE_RELATION;
}

/*
 * the rows the joins of tables below a path of the normal plan add to those of their larger input (a key that repeats in the table joined), whatever sorts, hashes and
 * appends them: the scan prices that fan for a table it looks up, and no other input (the rows of a function, a sub-select or a CTE are an estimate of PostgreSQL's,
 * 100 for each call of jsonb_each_text where its three keys make three)
 */
static double
joined_rows(const Path *p)
{
	double		rows = 0;

	if (p == NULL)
		return 0;
	switch (nodeTag(p))
	{
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
		{
			const JoinPath *j = (const JoinPath *) p;

			if (!table_input(j->outerjoinpath) || !table_input(j->innerjoinpath))
				return joined_rows(j->outerjoinpath) + joined_rows(j->innerjoinpath);
			return Max(p->rows - Max(j->outerjoinpath->rows, j->innerjoinpath->rows), 0.0) + joined_rows(j->outerjoinpath) + joined_rows(j->innerjoinpath);
		}
		case T_SortPath:
			return joined_rows(((const SortPath *) p)->subpath);
		case T_IncrementalSortPath:
			return joined_rows(((const IncrementalSortPath *) p)->spath.subpath);
		case T_MemoizePath:
			return joined_rows(((const MemoizePath *) p)->subpath);
		case T_MaterialPath:
			return joined_rows(((const MaterialPath *) p)->subpath);
		case T_GatherPath:
			return joined_rows(((const GatherPath *) p)->subpath);
		case T_GatherMergePath:
			return joined_rows(((const GatherMergePath *) p)->subpath);
		case T_AppendPath:
			foreach_ptr(Path, q, ((const AppendPath *) p)->subpaths)
				rows += joined_rows(q);
			return rows;
		default:
			return 0;
	}
}

/*
 * The time PostgreSQL leaves out of the joins of a normal plan that multiply the rows: it prices a joined tuple at a few operators, and the executor
 * spends 1,700 to 2,200 instructions on each in the join and the grouping above it (a hash probe and the tuple built, or a nested loop's rescan, then the
 * hash of the group and the transition). In its units, for the tuples the joins add to the rows of the larger input (those the scan counts as the fan of
 * the lookup, cost_work), which the scan prices for itself.
 */
double
join_agg_extra(const AggPath *a)
{
	return joined_rows(a->subpath) * AGG_JOIN_NS * 1e-6 * AGG_NORMAL_MS;
}
