/*
 * agg/plan_cost.c - the cost model of the scan and its constants
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "executor/nodeHash.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "col/point.h"
#include "agg/colsrc.h"
#include "agg/dist_int.h"
#include "agg/search.h"
#include "agg/lookup_int.h"
#include "agg/plan_int.h"
#include "agg/agg_int.h"

/*
 * The executor's cost per value in units of cpu_operator_cost (about 3 ns: docs/COL-PLAN.md, "Cost", fitted on whole table runs of
 * one to three keys and aggregates), and the elements per row of an array whose statistics are missing.
 */
#define AGG_SCAN_LEAF		0.7		/* a leaf read (decode, null flags) */
#define AGG_SET_USEFUL		0.85	/* the share of the units a leaf's small-set summaries must keep at most, for the cost to count the units they skip */
#define AGG_SPARSE_MAX		0.125	/* the share of the rows a unit keeps up to which its leaves are decoded at the kept rows only */
#define AGG_SPARSE_LEAF		1.5		/* a leaf read at the kept rows costs this many times the share of the rows it reads (the positions are gathered) */
#define AGG_FILTER_VALUE	0.5		/* a predicate on a value */
#define AGG_OP_NS			3.0		/* the nanoseconds an operator is counted at (AGG_SCAN_SPEED takes the share the scan really spends) */
#define AGG_IN_TEST_BASE	1.0		/* the nanoseconds a row costs the test of a list in the cache, which AGG_FILTER_VALUE already counts */
#define AGG_TUPLE			2.5		/* a tuple that passes the filters: selection, ordinals, group ids */
#define AGG_AGG_ROW			1.2		/* an aggregate update */
#define AGG_KEY_ROW			1.0		/* a group key id */
#define AGG_COMBO_ROW		2.5		/* a tuple's entry in a combo: gathers and a hash probe */
#define AGG_SEMI_ROW		1.0		/* a tuple tested against a semi or anti join: one byte of the lookup table, or a hash probe */
#define AGG_ORDERED_ROW		6.0		/* a value kept for an ordered-set aggregate (histogram probe) */
#define AGG_NUM_STEP		0.5		/* an int128 add, subtract, multiply on a value */
#define AGG_NUM_DIV			8.0		/* round, trunc, floor, ceil: an int128 division */
#define AGG_VLEAF_ELEM		0.6		/* an element of an object looked up by key (a metric of a free key object), per row and key: 1, 2 and 4 keys of an object of 12 elements took 4.7 ms, 7.3 ms and 12.5 ms more than the group alone over 1,000,000 rows (0.55 ns an element) */
#define AGG_EVAL_SLOT		60.0	/* PostgreSQL evaluating an expression for a value combination it has not seen: the datums, a few operators, the id or the copy of the result */
#define AGG_JOIN_PROBE		58.0	/* a tuple's probe of a lookup table of 250,000 rows by its join key: the key's hash and the rows it finds, in the operators AGG_SCAN_SPEED scales (35 ns measured); it grows with the square root of the rows, as the table leaves the cache */
#define AGG_ENTRY_ROW		70.0	/* a new entry of a combo of a join that reads values beyond its key (a CASE of a lookup column and a measure): the hash insert and the id of its value, 215 ns measured with 2.8 million entries (2,040 instructions with one expression, 5,450 with seven) */
#define AGG_ENTRY_SLOT		30.0	/* ... and each expression of the combo evaluated for it: 88 ns measured, 570 instructions of a numeric CASE */
#define AGG_FAN_ROW			120.0	/* a joined tuple beyond the rows the scan reads (a lookup table with many rows for a key: a fact table joined from a date scan), for a lookup table past what the cache holds: the columns of the row bound and the ids of its values (1,330 instructions measured at 720,000 rows and at 2.9 million, 910 at 290,000); it grows with the square root of the rows past the cache up to AGG_FAN_CAP times */
#define AGG_FAN_CACHE		150000.0	/* the rows of a lookup table whose binds stay in the cache of a core: the price starts past them (a price at any size, and for correlated or CTE lookups, flipped 12 statements of the lab corpus to the normal plan, which takes more than 60 s on them) */
#define AGG_FAN_CAP			2.0	/* ... and no more than twice: the cost per tuple measured is the same at 720,000 rows as at 2.9 million */
#define AGG_FAN_SLOT		80.0	/* ... and each expression of the combos evaluated for it: 97 ns measured at 2.9 million rows, a numeric CASE of a column of the lookup table */
#define AGG_WIDE_LEAF		3.0		/* a 64-bit leaf of a combo, per tuple: the hash probe for the id of its value */
#define AGG_DIRECT_NUM		2.0		/* the transition of a PostgreSQL-evaluated value (numeric_add): the normal plan pays it per row too, at the price PostgreSQL gives a transition function */
#define AGG_DIRECT_CMP		1.0		/* the type's comparison of one (min, max): the same in both plans */
#define AGG_REAL_CALL		5.0		/* what the executor really spends on a function or operator node (the fmgr call and its arguments), in operators */
#define AGG_REAL_NUMERIC	40.0	/* the same for a numeric function: the allocation and the digit arithmetic */
#define AGG_REAL_JSON		70.0	/* the same for a function that builds a jsonb value: 1,400 instructions measured for jsonb_build_array of one element, against 100 for an fmgr call */
#define AGG_PLAIN_CREDIT	0.5		/* the share of the normal plan's under-priced evaluation the scan is credited with */
#define AGG_BYTE			0.1		/* a stored byte read and decompressed: about 0.3 ns, 3 GB/s */
#define AGG_UNNEST_FLOOR	100.0	/* the rows PostgreSQL expects of an unnesting function over one row (jsonb_array_elements, jsonb_each): the join of a lookup it has no statistics for is that many */
#define AGG_SCAN_SPEED		0.2		/* the share of the operators above the scan really takes: the shapes it runs alone (no lookup tables) take 0.4 to 0.9 ns for an operator counted at 3 ns */
#define AGG_UNIT_COST		200.0
#define TOPN_RUN_UNIT		8000.0	/* operators to read the first rows of a unit through its sorted run: the seek of the range, a window of 32 keys, their addresses (about 25 us a unit measured: 1,000,000 rows in 50 units, 1.26 ms) */
#define TOPN_RUN_ROW		12.0	/* ... and for each row wanted past the window */
#define AGG_PREAGG_HLL		40000.0	/* a unit's stored sketch merged into the aggregate's: up to 16,384 registers unpacked and maxed (about 60 us for a dense one) */
#define AGG_PREAGG_GROUP	25.0	/* a group of a unit's histogram folded into the table: its key id and the aggregates (about 80 ns) */
#define AGG_START_COST		100000.0	/* opening the source, the lookup tables and the first units: 1 to 1.5 ms measured on a scan pruned to a few units the first time a backend runs it, 0.07 to 0.3 ms from the second; charged 0.25 ms, which a selective btree plan does not pay and which put the scan above the btree plan for a lookup of 500 to 700 rows (the scan runs it in 60 to 100 us, the btree plan in 200 to 370) */
#define AGG_GORD_START		28000.0	/* a top-N that reads the order array of its key opens no unit: the executor starts, 0.17 M instructions measured (max(ts) of 500,000 rows with a global order: 0.31 M, of 3,000,000 rows 0.79 M) ... */
#define AGG_GORD_SEG		7300.0	/* ... and the array of each segment is opened and its first chunk decoded, 44 thousand instructions a segment of about AGG_GORD_SEG_ROWS rows. The pre-aggregates of the units answer min(x), max(x) of a table in 0.27 M, so they win where no condition bounds the key */
#define AGG_GORD_SEG_ROWS	222464.0
#define AGG_START_PLAIN		40000.0	/* the same for an aggregate with no groups */
#define AGG_RESTART_COST	60000.0	/* a scan an enclosing query restarts for each value: PostgreSQL's nested loop charges the price of the inner path for every row of the outer. A probe takes 74 us (1,000,000 rows) to 120 us (100,000) where a btree's takes 6 us, which PostgreSQL prices at about 12: the restart is priced at 150 so that a btree on the key and the order wins where there is one (the start over itself is 4.5 us) */
#define AGG_PART_PAIR		4.0		/* times the square of the partitions the scan reads: opening them costs the scan 0.75 ms at 200, 9 ms at 800, 27 ms at 1,600 and 111 ms at 3,200 more than the normal plan's Append (11 ns for each pair, measured on partitions of a few rows) */
#define AGG_ARM_TUPLE		150.0	/* a tuple through an arm of a two level query: its cell started or added to, and flushed into the outer groups when the row ends (35 ns measured) */
#define AGG_ARM_HIDDEN		330.0	/* ... in an arm with hidden keys: the cells of the row are numbered in a table of their own (70 ns) */
#define AGG_MERGE_GROUP		8.0		/* a group of a partial table of a worker: dumped, sent, merged */
#define AGG_MERGE_KEY		25.0	/* ... and, when the workers are priced, each key of it: 35 ns with one key over 30,000 groups and 85 ns with three over 100,000 per table measured (the CPU two workers add beyond their 3.35 ms start) */
#define AGG_ORDERED_MERGE	1.5		/* a value kept for an ordered-set aggregate, as the leader merges the histograms of the workers (the distinct pairs, at most one per value) */
#define AGG_DISTINCT_MERGE	7.0		/* a (group, value) pair of a count(DISTINCT), as the leader merges the sets of the workers into its own: one by one, as costly as the pair was to insert (2,000,000 rows of 1,000 groups, 0 to 8 workers: 820 to 890 ms) */
#define AGG_HASH_ENTRY		64.0	/* the bytes of an entry of a hash aggregate besides its key: the bucket, the stored tuple's header, its allocation */
#define AGG_HASH_CACHE		1048576.0	/* the hash table that stays in the cache of a core */
#define AGG_HASH_MISS		4.4		/* the memory misses of a row into a table far past the cache, for an integer key: 1.9 at 720 kB, 4.2 at 7 MB, 4.6 at 36 MB (perf, 1,000,000 rows, 10,000 to 500,000 groups) */
#define AGG_HASH_MISS_KEY	2.1		/* ... and what a key held apart from its entry adds (text: 3.7 at 810 kB, 6.3 at 8 MB, 6.8 at 40 MB), more for a wide one */
#define AGG_HASH_MISS_NS	80.0	/* a miss, in ns */
#define AGG_HASH_GROW_NS	20.0	/* the table doubling when it fills, for each doubling of the groups: 68 instructions a row */
#define AGG_NORMAL_MS		150.0	/* a millisecond of the normal plan, in its cost units: its aggregates ran 23 (1,000,000 rows, 439,000 groups) to 100 units a ms */
#define AGG_START_MS		1100.0	/* a millisecond in this path's estimates, for starting workers: a scan of 4 to 13 ms (1,000,000 rows, 9 to 30,000 groups) took 0.6 to 1.0 ns an operator, 500 to 830 units a ms, and the workers add 3.35 ms and 6 to 16 ms of CPU for no wall: the 223 SiftAgg nodes over 1 ms of the 156-statement replay ran 0.002 (10th percentile), 0.0098 (median) and 0.055 ms (90th) a cost unit, and the workers take 3 ms and 0.35 ms each to start; not below 1,100: 100,000 rows (a group by key, 3 to 5 ms alone, estimated 6,100) took 2 workers that made it 5 to 10 ms and 11 million instructions dearer, while a scalar group of 3,000,000 rows (20 ms alone, estimated 8,300) still gains 22% with 5 */
#define AGG_CHILD_RUN		0.5		/* the share of PostgreSQL's cost of a LIMIT 1 child that a run takes: 0.6 to 2.4 us a distinct input on tables of 100 to 122,000 rows, 3 us in a mapping child, against 2.5 to 3.6 units of 2.9 us it was charged; not below 0.45, where the planner's latest-fact lookup is priced under its measured ratio */
#define AGG_SHARE_ROW		1.0		/* a row of a lookup table written to the shared memory by the leader and mapped by each worker: a quarter of the cost of reading it */
#define AGG_CUT_SHARE		0.5		/* the share of the index that the keys of a lookup leave, at least, for the normal plan to keep its price: a table of a few units reads them all whatever the key */
#define AGG_CUT_UNITS		8.0		/* the units the keys of a lookup leave, at most: an index cut by the key (key_cut) reads a handful for one key and the scan costs a millisecond or two more than the probes, where an index not cut by it reads tens to hundreds (1,000,000 rows, 3,000,000: 9 units against 45-417) */
#define AGG_PROBE_KEYS		4.0		/* the keys of a lookup that the normal plan still reads one btree probe each, priced as they run: the scan starts later and costs more than a few probes (1,000,000 rows, 3 keys: 6.8 ms against 7.1 for the scan, 5 keys: 10.5 against 8.3) */
#define AGG_KEY_SKEW_FLOOR	2.0		/* the rows a key owns beyond the mean's double are skew: a statistics sample and a mild unevenness of a column are not */
#define AGG_SETUP_FACTOR	2.0	/* times parallel_setup_cost: the least a scan is charged for workers; a scan the estimate puts under ~4,000 units (8 to 40 ms measured) stays in one process. 22 priced them at 215 ms for the median node: no replay statement took workers, and a shape of 3,200,000 rows that runs 2.5 times faster with them stayed serial */
#define AGG_DEFAULT_ELEMS	3.0
#define AGG_RECHECK_FRAC	0.02	/* share of the rows of a flagged unit the source reads again from the heap when no sample of the unit's overflow slots says (statistics: ovf_frac) */
#define AGG_RECHECK_ROW		500.0	/* a row read again: the heap fetch by TID, the document walked again and the unit it makes, in the operators AGG_SCAN_SPEED scales: about 1.6 microseconds a row, which the normal plan's cost prices at 0.25: the two plans take the same time at about 14% of the rows read again */
#define AGG_VET_PAGE		400.0	/* a heap page the check reads from shared buffers: the buffer lookup, its lock and the line pointers (1 to 1.5 us measured on 1,000 to 24,000 pages, 185 rows or 8 to a page), in the operators AGG_SCAN_SPEED scales */
#define AGG_VET_OS_PAGE		1000.0	/* ... from the cache of the operating system: the read call and the copy of the page (3.5 us measured, a heap of 185 MB under 16 MB of buffers) */
#define AGG_VET_ROW			1.2		/* a row of such a page tested against the snapshot (4 ns measured at 185 rows to a page) */
#define AGG_WIDE_ROW		3.0		/* a row of a wide leaf: its lanes decoded, the hash probe for its entry in the unit's dictionary */
#define AGG_WIDE_ENTRY		60.0	/* a new entry of it (a numeric built from a 128-bit lane, its probe in the table of ids and its transition: 130 ns with a thousand values, 370 ns with a hundred thousand), per distinct value */
#define AGG_WIDE_NUM		16.0	/* ... of a numeric image that only sums and averages read (SiftAggAgg.wsum): its digits become an integer and a scale once for each distinct value of a unit, and no numeric is built (a row 33 ns when the values repeat, 132 ns when each is new: 100 ns an entry, 500,000 rows) */
#define AGG_WIDE_IMAGE		180.0	/* ... of a numeric stored as an image (unconstrained, or past 38 digits): its digits are read and the numeric built from them, 0.45 us measured at a hundred thousand values */
#define AGG_WIDE_COPY		5.0		/* ... when its readers only copy the entry (array_agg, string_agg, count of the column, a group key): 0 to 100 ns an entry measured over numeric images, 128-bit numerics, uuid, interval and bytea, 1,000,000 rows in 4 groups (a list of 550,000 two-decimal values 54 ms more than one of 1,000 values) */
#define AGG_WIDE_KEYSUM		25.0	/* ... and a sum or average of the column is added in the same scan (42 ns an entry more than the key alone) */
#define AGG_WIDE_COLD		35.0	/* a row of such an image leaf, read by a worker: the unit's dictionary of images is inflated (zstd) and indexed in the worker, whose cache is empty, where the backend that repeats a scan finds it decoded (2,000,000 rows of almost unique values: 24 ns inflated, 6 ns of fresh pages, 3 ns indexed, a row; 20 ns with 30,000 values over 300,000 rows: the 0.13 power of the values over 2,000,000, as the dictionaries fit the cache) */
#define AGG_WIDE_BIG		10.0	/* ... and, for each million distinct values the column has past 2,000,000, this many more up to AGG_WIDE_BIG_MAX: the dictionary of a segment outgrows the cache and a row is a miss for its entry (975 cycles a row in one segment of 10,000,000 almost unique values, 573 in 2,000,000), so the parallel scan of 5,000,000 or 10,000,000 such rows takes 1.5 to 2.3 times as long as PostgreSQL's plan with 4 or 8 workers */
#define AGG_WIDE_BIG_MAX	40.0
#define AGG_DIST_CHUNK		400.0	/* a chunk of the bitmap of a count(DISTINCT) set allocated and zeroed for a group: 0.4 to 1.1 us measured (1,000 to 4,907 groups of 2 to 200 rows), in the 4 us a unit of a normal plan's cost takes on the same hosts */
#define AGG_DIST_CHUNK_BITS	4096.0	/* the values a chunk holds (agg/dist.c) */
#define AGG_DIST_CHUNK_BYTES	512.0
#define AGG_ARR_KNEE		64		/* the bytes of an array from which the unit's dictionary entry for it costs more than the generic wide entry: 1,670 to 2,030 instructions a row for arrays of 44 to 78 bytes, 4,020 for 122 (1,000,000 rows of almost unique arrays, forced SiftAgg; the heap plan takes about 950 whatever the width) */
#define AGG_ARR_BIG		6.0		/* the operators a byte of an array beyond the knee adds to a new entry: hash, intern and inflate (about 8.6 instructions an operator counted at 3 ns) */
#define AGG_ARR_WIDTH		32		/* the bytes of an array whose column has no statistics */
#define AGG_WIDE_TEST		1.0		/* a conjunct on a wide leaf of images, per dictionary entry: the engine makes its LUT once per segment (about 3 ns an entry) */
#define AGG_INLINE_WIDTH	200		/* average bytes of a text column from which its units store the strings themselves */
#define AGG_INLINE_ROW		56.0	/* a row of such a leaf read: the unit's strings are interned into a dictionary of its own */
#define AGG_IN_TABLE_BYTE	0.75	/* a byte of the table of an IN list over a range of values, allocated and cleared when the scan starts: 1.9 ns measured (a 1 MB table costs 1.9 ms, 4 KB nothing visible), the pages it faults in */
#define AGG_INLINE_TEST		3.0		/* a row of it tested on the encoded strings (the prefixes decide most rows) */
#define AGG_KEY_ENTRY		40.0	/* an entry of a unit's dictionary given the id of a group key (the text built, hashed and looked up) in a table of 2,000 ids: 39 ns measured, which the read of the leaf prices; 90 ns at 20,000 ids and 200 ns at 200,000, as the table outgrows the caches (the 0.35 power of the ids over AGG_KEY_IDS): the excess over the first is charged */
#define AGG_KEY_WIDE		45.0	/* an entry of a unit's dictionary of a wide column, given the id of a group key, in a table of 30,000 ids (107 ns measured on a numeric of two decimals); it grows with the 0.42 power of the ids (460 ns at 1,000,000 ids) */
#define AGG_KEY_IDS			2000.0
#define AGG_DIST_SEND		250.0	/* a distinct text value a worker sends and the leader merges into its own set of a count(DISTINCT): 0.4 to 1.2 us (1,500 to 2,600 instructions) measured with 100,000 to 736,000 values over 1,000,000 and 3,000,000 rows, 2 workers; the leader hashes the strings again */
#define AGG_DIST_ID			35.0	/* an entry of a unit's dictionary given the id of a count(DISTINCT) of a text column, in a table of 100,000 ids: 83 ns measured (10,000 ids: 6 ns, 30,000: 23, 245,000: 110, 289,000: 216, 632,000: 259, 736,000 ids over 3,000,000 rows: 199 ns), as the table outgrows the caches: the 0.9 power up to 100,000 ids, the 0.6 power past them */
#define AGG_ID_SWEEP		8.0		/* an id of the key table, per spill of the group table: the table is walked and its arrays reset whatever number of its groups hold rows (45 ns measured), in cpu_operator_cost */

double
elems_of(AggCtx *c, int grp)
{
	return c->stats.elems_per_row[grp] > 0 ? c->stats.elems_per_row[grp] : AGG_DEFAULT_ELEMS;
}

static double
num_cost(const SiftAggNum *o)
{
	double		cost = 0;

	for (int k = 0; k < o->n; k++)
		cost += o->step[k].op >= SNX_ROUND ? AGG_NUM_DIV : o->step[k].op > SNX_CONST ? AGG_NUM_STEP : 0;
	return cost;
}

/* the numeric programs of a predicate tree, per row that reaches them */
static double
pred_num_cost(const SiftAggPred *p)
{
	double		cost = p == NULL ? 0 : num_cost(&p->num);

	for (int i = 0; p && i < p->nchild; i++)
		cost += pred_num_cost(p->child[i]);
	return cost;
}

/* the numeric operations of a program, and of the predicate tree (one more for the comparison of each): PostgreSQL's numeric functions in the normal plan */
static int
num_ops(const SiftAggNum *o)
{
	int			n = 0;

	for (int k = 0; k < o->n; k++)
		n += o->step[k].op > SNX_CONST;
	return n;
}

static int
pred_num_ops(const SiftAggPred *p)
{
	int			n = p == NULL ? 0 : p->num.n > 0 ? num_ops(&p->num) + 1 : 0;

	for (int i = 0; p && i < p->nchild; i++)
		n += pred_num_ops(p->child[i]);
	return n;
}

/*
 * The distinct values of a column under the table's own conditions on it: the values of the table (PostgreSQL's statistics) times the
 * share the conditions keep (a range of dates keeps the dates of the range). 0 when there are no statistics.
 */
double
column_distinct(AggCtx *c, Var *v)
{
	List	   *clauses = NIL;
	HeapTuple	tup;
	double		nd;

	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid), Int16GetDatum(v->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tup))
		return 0.0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	ReleaseSysCache(tup);
	nd = nd > 0 ? nd : -nd * c->rel->tuples;
	foreach_node(RestrictInfo, ri, c->rel->baserestrictinfo)
	{
		Bitmapset  *atts = NULL;

		pull_varattnos((Node *) ri->clause, c->relid, &atts);
		if (bms_membership(atts) == BMS_SINGLETON && bms_is_member(v->varattno - FirstLowInvalidHeapAttributeNumber, atts))
			clauses = lappend(clauses, ri);
	}
	return nd * (clauses == NIL ? 1.0 : clauselist_selectivity(c->root, clauses, 0, JOIN_INNER, NULL));
}

/* the distinct values of column col of a joined table by PostgreSQL's statistics, its rows when there are none */
static double
joined_distinct(AggCtx *c, JoinCtx *jc, int col)
{
	RelOptInfo *rel = c->root->simple_rel_array[jc->rti];
	HeapTuple	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(jc->rti, c->root)->relid),
									  Int16GetDatum(jc->j.colatt[col]), BoolGetDatum(false));
	double		nd = 0;

	if (HeapTupleIsValid(tup))
	{
		nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
		ReleaseSysCache(tup);
		nd = nd > 0 ? nd : -nd * rel->tuples;
	}
	return nd > 0 ? Min(nd, Max(rel->rows, 1.0)) : Max(rel->rows, 1.0);
}

/*
 * distinct value combinations of the inputs of a combo among `tuples`: what the statistics say of the columns, the tuples when an input is
 * not one; a column of an earlier lookup has at most as many values as the lookup has rows. The combo of a lookup per element is not
 * evaluated for the tuples but for the combinations of the values of the elements (the index counts those of each leaf) and of the column
 * of the lookup it reads. `counted` prices the elements of an unscoped lookup by those counts too, not by PostgreSQL's default.
 */
static double
combo_entries(AggCtx *c, const SiftAggSpec *s, const SiftAggCombo *cb, double tuples, bool counted)
{
	List	   *exprs = NIL;
	double		entries = 1.0,
				elem_nd = 1.0;
	Bitmapset  *lookups = NULL;
	bool		scoped = cb->join >= 0 && ((JoinCtx *) list_nth(c->joins, cb->join))->j.scoped;
	int			parents[SIFT_AGG_MAX_CLEAVES],
				nparents = 0;
	double		parent_nd[SIFT_AGG_MAX_CLEAVES] = {0};

	for (int p = 0; p < cb->nleaves; p++)
	{
		SiftSrcLeaf *lf;
		Node	   *e;
		double		nd;

		if (cb->leaf[p] < 0)
		{
			foreach_ptr(JoinCtx, jc, c->joins)
				if (jc->combo == cb->refcombo[p])
				{
					/* the columns of one row of a lookup are one value combination of it, however many the combo reads */
					if (scoped)
						entries *= joined_distinct(c, jc, cb->refcol[p]);
					else if (!bms_is_member(jc->combo, lookups))
					{
						double		rows = Max(c->root->simple_rel_array[jc->rti]->rows, 1.0);

						/* a correlated child gives its rows for each distinct input of its own combo */
						entries *= jc->j.params != NIL ? rows * combo_entries(c, s, &s->combo[jc->combo], tuples, counted) : rows;
					}
					lookups = bms_add_member(lookups, jc->combo);
				}
			continue;
		}
		lf = leaf_of(c, cb->leaf[p]);
		/* the values of a member or an element have no statistics (but the counts of the index, where asked): PostgreSQL's default number of distinct values, as for a column it knows nothing of */
		if (lf->parent >= 0 || lf->path != NIL)
		{
			nd = (scoped || counted) && c->stats.ndistinct[cb->leaf[p]] > 0 ? c->stats.ndistinct[cb->leaf[p]] : DEFAULT_NUM_DISTINCT;
			/* the fields of one element go together (a category and its kind): the combinations are about as many as the values of the widest */
			if (scoped)
				elem_nd = Max(elem_nd, nd);
			else if (counted && lf->parent >= 0)
			{
				int			i = 0;

				while (i < nparents && parents[i] != lf->parent)
					i++;
				if (i == nparents)
					parents[nparents++] = lf->parent;
				parent_nd[i] = Max(parent_nd[i], nd);
			}
			else
				entries *= nd;
			continue;
		}
		if (lf->attno > list_length(c->attexpr) || (e = list_nth(c->attexpr, lf->attno - 1)) == NULL)
			return tuples;
		if (IsA(e, Var) && (nd = column_distinct(c, (Var *) e)) > 0)
			entries *= nd;
		else
			exprs = lappend(exprs, e);
	}
	for (int i = 0; i < nparents; i++)
		entries *= parent_nd[i];
	if (exprs != NIL)
		entries *= estimate_num_groups(c->root, exprs, tuples, NULL, NULL);
	return Max(Min(entries * elem_nd, tuples), 1.0);
}

/*
 * The rows of the path of a joined plan. PostgreSQL assumes 1000 rows a call of a set-returning function in the select list (a date
 * spine), a ceiling nobody generates up to: the rows a call makes are taken as the geometric mean of one and that estimate.
 */
static double
child_rows(Path *p)
{
	for (;;)
	{
		if (IsA(p, SubqueryScanPath))
			p = ((SubqueryScanPath *) p)->subpath;
		else if (IsA(p, ProjectionPath))
			p = ((ProjectionPath *) p)->subpath;
		else if (IsA(p, ProjectSetPath))
		{
			Path	   *sub = ((ProjectSetPath *) p)->subpath;

			return child_rows(sub) * sqrt(Max(p->rows / Max(sub->rows, 1.0), 1.0));
		}
		else
			return p->rows;
	}
}

/* what a test of the rest of an ON clause spends beyond the price of the slot: the jsonb values it builds, in operators PostgreSQL's executor runs */
static bool
json_build_walker(Node *n, double *ops)
{
	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr) && ((FuncExpr *) n)->funcresulttype == JSONBOID)
		*ops += AGG_REAL_JSON;
	return expression_tree_walker(n, json_build_walker, ops);
}

/* the leaves of the combo of a join that are not its key: the values its expressions read, which make an entry for each distinct combination */
static int
join_extra_leaves(const SiftAggCombo *cb, const SiftAggJoin *j)
{
	int			extra = 0;

	for (int p = 0; p < cb->nleaves; p++)
	{
		bool		key = false;

		for (int k = 0; k < j->nkey; k++)
			key |= j->keypos[k] == p;
		extra += !key;
	}
	return extra;
}

/* the expressions of a combo that PostgreSQL evaluates to a numeric: an entry of those allocates and compares digits (an integer one was not measured to cost as much) */
static int
combo_numeric_slots(const SiftAggCombo *cb)
{
	int			n = 0;

	for (int sl = 0; sl < cb->nslots; sl++)
		n += exprType((Node *) cb->slot[sl].expr) == NUMERICOID;
	return n;
}

/* the combo is only the argument of sums and averages that add the entries of their dictionary leaf (SiftAggAgg.wsum): no tuple is evaluated by it */
bool
combo_wsum_only(const SiftAggSpec *s, int ci)
{
	const SiftAggCombo *cb = &s->combo[ci];
	bool		only = cb->nslots > 0;

	for (int sl = 0; sl < cb->nslots; sl++)
	{
		bool		used = false;

		only &= cb->slot[sl].direct;
		for (int ai = 0; ai < s->naggs; ai++)
			if ((s->agg[ai].kind >= SAA_X_COUNT && s->agg[ai].kind <= SAA_X_AVG) && s->agg[ai].combo == ci && s->agg[ai].slot == sl)
			{
				only &= s->agg[ai].wsum != 0;
				used = true;
			}
		only &= used;
	}
	return only;
}

static bool real_cost_walker(Node *n, double *excess);

/* does the rest of the ON clause of combo ci compare the tuple's values with the table's? Then it is tested for each tuple (a condition on the table alone is tested when the table loads) */
static bool
on_tests_tuple(const SiftAggSpec *s, int ci)
{
	const SiftAggCombo *cb = &s->combo[ci];

	return cb->join >= 0 && s->join[cb->join].fslot >= 0 && cb->nleaves > 1;
}

/*
 * The evaluation PostgreSQL does for the combos with a 64-bit leaf or a direct slot: a hash probe per tuple for the id of each wide
 * leaf, and the expressions once per distinct combination of the leaves (every tuple when the column has as many values). Combos of
 * dictionary and small integer leaves have few combinations and keep the price they had.
 */
static double
combo_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows, double agg_rows, double scan_rows, double kept_rows, double alive, double *entry_cost)
{
	double		cost = 0;

	*entry_cost = 0;

	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggCombo *cb = &s->combo[ci];
		int			wide = 0;
		bool		direct = false,
					filter = false,
					lookup_ref = false;
		double		tuples;

		if (cb->join >= 0)
		{
			const SiftAggJoin *j = &s->join[cb->join];
			JoinCtx    *jc = list_nth(c->joins, cb->join);

			/*
			 * With equality keys, every tuple the filters keep probes the table before the joins narrow the rows to the ones that reach the groups
			 * (a join keyed by the output of an earlier lookup sees what that one let through: the mean of the two counts), and the rest of the
			 * ON clause is evaluated for each distinct combination of the values it reads, which for a key of many values is nearly every tuple.
			 */
			if (j->nkey > 0 && j->params == NIL)
			{
				double		probes = alive;

				for (int p = 0; p < cb->nleaves; p++)
					if (cb->leaf[p] < 0)
						probes = Min(alive, sqrt(alive * Max(agg_rows, 1.0)));
				cost += probes * AGG_JOIN_PROBE * sqrt(Max(c->root->simple_rel_array[jc->rti]->rows, 1.0) / 250000.0);
				/* the rest of the ON clause is PostgreSQL's expression, for each entry; the executor's evaluation does not get AGG_SCAN_SPEED's share (110 ns an entry measured, 36 counted) */
				if (on_tests_tuple(s, ci) && !combo_wsum_only(s, ci))
				{
					double		real = 0;

					real_cost_walker((Node *) cb->slot[j->fslot].expr, &real);
					*entry_cost += combo_entries(c, s, cb, probes, false) * (AGG_EVAL_SLOT + real) / AGG_SCAN_SPEED;
				}
				else if (join_extra_leaves(cb, j) > 0 && combo_numeric_slots(cb) > 0)
					*entry_cost += combo_entries(c, s, cb, probes, false) * (AGG_ENTRY_ROW + cb->nslots * AGG_ENTRY_SLOT) / AGG_SCAN_SPEED;
			}
			/* Without equality keys, every cached input tests every child row. */
			if (j->nkey == 0 && j->params == NIL)
			{
				double		candidates = jc->cpath ? child_rows(jc->cpath) : c->root->simple_rel_array[jc->rti]->tuples,
							built = 0;

				/* agg_cost scales the scan by AGG_SCAN_SPEED, which the executor's jsonb builds do not get */
				if (j->fslot >= 0)
					json_build_walker((Node *) cb->slot[j->fslot].expr, &built);
				cost += combo_entries(c, s, cb, scan_rows, false) * Max(candidates, 1.0) * (AGG_EVAL_SLOT + built / AGG_SCAN_SPEED);
			}
			continue;
		}
		for (int p = 0; p < cb->nleaves; p++)
		{
			if (cb->leaf[p] >= 0)
				wide += leaf_of(c, cb->leaf[p])->vtype == SCV_I64 || leaf_of(c, cb->leaf[p])->vtype == SCV_F64;
			else
				lookup_ref = true;
		}
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			direct |= cb->slot[sl].direct;
			filter |= !cb->slot[sl].is_key && !cb->slot[sl].direct;
		}
		tuples = filter ? rows : agg_rows;
		/*
		 * A test between leaves (a column compared with a column) is evaluated by PostgreSQL once per distinct combination of the leaves: a few for
		 * dictionary and small integer leaves, which keep the price they had, one per row for two dates that vary (545 instructions a row measured,
		 * 124 ms for 400,000 rows, against the 36 of a test on a constant); so is an expression of a key, for the values the groups have
		 */
		if (wide == 0 && !direct)
		{
			double		entries = !lookup_ref && (filter ? cb->nleaves > 1 : cb->nleaves > 0) ? combo_entries(c, s, cb, filter ? kept_rows : agg_rows, false) : 0;

			if (entries > AGG_KEY_IDS)
				cost += entries * cb->nslots * AGG_EVAL_SLOT;
			continue;
		}
		if (combo_wsum_only(s, ci))
			continue;
		cost += tuples * wide * AGG_WIDE_LEAF + combo_entries(c, s, cb, tuples, false) * cb->nslots * AGG_EVAL_SLOT;
	}
	return cost + *entry_cost;
}

/* a tuple of a bucket key: the number of its step, the lookup of the id */
#define AGG_BUCKET_ROW		3.0

/* the buckets of a stretch of the leaf of a key, by the statistics: the tuples when there are none */
static double
bucket_count(AggCtx *c, const SiftAggKey *k, double tuples)
{
	int64		lo,
				hi;

	return leaf_span(c, k->leaf, &lo, &hi) ? Min(tuples, (double) (hi - lo) / USECS_PER_SEC / k->bucket + 1) : tuples;
}

/* a lookup per tuple of a bucket key, and PostgreSQL evaluating the expression once per bucket */
static double
bucket_eval_cost(AggCtx *c, const SiftAggSpec *s, double rows)
{
	double		cost = 0;

	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].bucket > 0)
			cost += rows * AGG_BUCKET_ROW + bucket_count(c, &s->key[i], rows) * AGG_EVAL_SLOT;
	return cost;
}

/*
 * What the executor really spends on an expression, in operators (PostgreSQL prices a function at procost, 1 for almost all, while a
 * numeric one allocates and does digit arithmetic).
 */
static bool
real_cost_walker(Node *n, double *excess)
{
	List	   *args = NIL;

	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr))
		args = ((FuncExpr *) n)->args;
	else if (IsA(n, OpExpr) || IsA(n, DistinctExpr))
		args = ((OpExpr *) n)->args;
	if (IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, DistinctExpr))
	{
		double		real = exprType(n) == NUMERICOID ? AGG_REAL_NUMERIC : AGG_REAL_CALL;

		foreach_ptr(Node, a, args)
			if (exprType(a) == NUMERICOID)
				real = AGG_REAL_NUMERIC;
		*excess += real;
	}
	else if (IsA(n, CoerceViaIO))
		*excess += AGG_REAL_NUMERIC;
	return expression_tree_walker(n, real_cost_walker, excess);
}

/*
 * The credit of the expressions and numeric aggregates: both plans evaluate the expressions of a query, the normal plan once per row at the price PostgreSQL gives
 * them, which is the executor's cost of a call for the cheap ones and an order of magnitude less than the real one for a numeric one;
 * the scan evaluates once per distinct combination of values and is priced with the real cost of its machinery (AGG_EVAL_SLOT). The
 * rows the normal plan evaluates the expressions for, times what its price leaves out, are what the scan saves: AGG_PLAIN_CREDIT of it.
 */
static double
plain_credit(AggCtx *c, const SiftAggSpec *s, double agg_rows)
{
	double		excess = 0;

	for (int ci = 0; ci < s->ncombo; ci++)
		if (s->combo[ci].join < 0)
			for (int sl = 0; sl < s->combo[ci].nslots; sl++)
			{
				Node	   *e = (Node *) s->combo[ci].slot[sl].expr;
				QualCost	priced;
				double		real = 0;

				real_cost_walker(e, &real);
				cost_qual_eval_node(&priced, e, c->root);
				excess += Max(real - priced.per_tuple / cpu_operator_cost, 0.0);
			}
	{
		int			ops = 0;

		for (int i = 0; i < s->npred; i++)
			ops += pred_num_ops(s->pred[i]);
		for (int i = 0; i < s->naggs; i++)
			ops += num_ops(&s->agg[i].num) + pred_num_ops(s->agg[i].filter);
		excess += ops * (AGG_REAL_NUMERIC - 2.0);
		/* the key of an integer or float program is an fmgr call, not the allocation and digits of a numeric one */
		for (int i = 0; i < s->nkeys; i++)
			excess += num_ops(&s->key[i].num) * ((s->key[i].typid == NUMERICOID ? AGG_REAL_NUMERIC : AGG_REAL_CALL) - 2.0);
	}
	/* the transition of a numeric sum, average or extreme: PostgreSQL prices it as one operator, the executor allocates and adds digits */
	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];

		if (a->restype == NUMERICOID && (a->kind == SAA_SUM_NUM || a->kind == SAA_AVG_NUM || a->kind == SAA_MIN || a->kind == SAA_MAX ||
										 a->kind == SAA_X_SUM || a->kind == SAA_X_AVG || a->kind == SAA_X_MIN || a->kind == SAA_X_MAX))
			excess += AGG_REAL_NUMERIC - 2.0;
	}
	return excess * agg_rows * AGG_PLAIN_CREDIT;
}

/* the per tuple cost of the arithmetic of a PostgreSQL-evaluated aggregate */
static double
direct_agg_cost(const SiftAggAgg *a)
{
	switch (a->kind)
	{
		case SAA_X_MIN:
		case SAA_X_MAX:
			return AGG_DIRECT_CMP;
		case SAA_X_SUM:
		case SAA_X_AVG:
			return a->restype == NUMERICOID ? AGG_DIRECT_NUM : 0;
		default:
			return 0;
	}
}

/* what PostgreSQL divides the cost of a parallel scan by: the workers, and the share of the leader that scans too */
static double
parallel_divisor(int workers)
{
	double		d = workers;

	if (parallel_leader_participation)
		d += Max(1.0 - 0.3 * workers, 0.0);
	return d;
}

/* The correlation of the heap column a leaf is read from with the heap order (0: unknown, or the leaf is a jsonb path, a field of a list or an expression) */
double
leaf_correlation(AggCtx *c, int leaf)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL && lf->parent < 0 ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	HeapTuple	tp;
	AttStatsSlot sslot;
	double		corr = 0.0;

	if (e == NULL || !IsA(e, Var))
		return 0.0;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid),
						 Int16GetDatum(((Var *) e)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return 0.0;
	if (get_attstatsslot(&sslot, tp, STATISTIC_KIND_CORRELATION, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		corr = sslot.numbers[0];
		free_attstatsslot(&sslot);
	}
	ReleaseSysCache(tp);
	return corr;
}

/*
 * The share of the units whose zone a test on an integer leaf overlaps. A test on the fraction f of the leaf's range overlaps the zone
 * of a unit when its range and the zone's meet: f plus the mean width of the zones (the index stores it: a column the units follow
 * has narrow zones, one they do not has the whole range in each). A source without zones has the correlation of the heap column: the
 * units are consecutive in the heap, so their zones are narrow in the share the column is correlated with (corr^2).
 */
static double
zone_overlap(AggCtx *c, int leaf, double f)
{
	double		span = c->stats.zone_span[leaf],
				corr;

	if (span > 0)
		return Min(f + span, 1.0);
	corr = leaf_correlation(c, leaf);
	return f + (1.0 - f) * (1.0 - corr * corr);
}

/* the lookup join whose key is the earlier lookup's column `col` (a later inner or semi join: its rows are the only ones that complete a tuple); *key: that key of it */
static const JoinCtx *
dependent_join(AggCtx *c, SiftAggSpec *s, const JoinCtx *jc, int *key)
{
	foreach_ptr(JoinCtx, dj, c->joins)
	{
		const SiftAggCombo *cb = &s->combo[dj->combo];
		const SiftAggJoin *j = &s->join[cb->join];

		if (dj == jc || (j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL)
			continue;
		for (int k = 0; k < j->nkey; k++)
		{
			Node	   *kx = j->keyexpr[k];
			int			pos = kx && IsA(kx, Param) ? ((Param *) kx)->paramid : -1;

			if (pos >= 0 && pos < cb->nleaves && cb->leaf[pos] == -1 && cb->refcombo[pos] == jc->combo)
			{
				*key = k;
				return dj;
			}
		}
	}
	return NULL;
}

/* the rows of a joined table or plan: what the scan loads */
static double
lookup_rows(AggCtx *c, const JoinCtx *jc)
{
	return Max(jc->cpath ? child_rows(jc->cpath) : c->root->simple_rel_array[jc->rti]->rows, 1.0);
}

/*
 * The keys of a lookup's plan, for the pick between the scan and the normal plan. PostgreSQL clamps an estimate at one row and takes a join of two
 * lists to a fraction of the smaller one (10 ids semi-joined to 32 rows: 2 of them), so the plan is read down to its inputs: a join has as many keys as
 * the smaller of its inputs (a nested loop: as its outer side, the inner one is per row) and a CTE as the plan of its query.
 */
static double
child_keys(PlannerInfo *root, Path *p)
{
	double		rows = child_rows(p);
	JoinPath   *jp;

	for (;;)
	{
		if (IsA(p, SubqueryScanPath))
			p = ((SubqueryScanPath *) p)->subpath;
		else if (IsA(p, ProjectionPath))
			p = ((ProjectionPath *) p)->subpath;
		else if (sift_is_sorted_unique_path(p))
			p = ((SiftSortedUniquePath *) p)->subpath;
		else if (IsA(p, SortPath))
			p = ((SortPath *) p)->subpath;
		else
			break;
	}
	if (p->pathtype == T_CteScan)
	{
		RangeTblEntry *rte = planner_rt_fetch(p->parent->relid, root);
		PlannerInfo *cr = rte->rtekind == RTE_CTE ? cte_root(root, rte) : NULL;

		return cr ? Max(rows, child_keys(cr, fetch_upper_rel(cr, UPPERREL_FINAL, NULL)->cheapest_total_path)) : rows;
	}
	if (!(IsA(p, NestPath) || IsA(p, MergePath) || IsA(p, HashPath)))
		return rows;
	jp = (JoinPath *) p;
	switch (jp->jointype)
	{
		case JOIN_INNER:
		case JOIN_SEMI:
#if PG_VERSION_NUM >= 180000
		case JOIN_RIGHT_SEMI:
#endif
			return Max(rows, IsA(p, NestPath) ? child_keys(root, jp->outerjoinpath) :
					   Min(child_keys(root, jp->outerjoinpath), child_keys(root, jp->innerjoinpath)));
		case JOIN_LEFT:
		case JOIN_ANTI:
			return Max(rows, child_keys(root, jp->outerjoinpath));
		default:
			return rows;
	}
}

static double
lookup_sure(AggCtx *c, const JoinCtx *jc)
{
	return jc->cpath ? Max(child_keys(c->root, jc->cpath), 1.0) : lookup_rows(c, jc);
}

static bool lookup_counted(AggCtx *c, const JoinCtx *jc);

/* agg_cost prices a path again for the tuples its lookup joins would not filter (workers_ratio): nothing it reports comes from that pass */
static bool pricing_wide,
			pricing_scratch;
static double cte_list_keys(AggCtx *c, const JoinCtx *jc, int key);
static double cte_list_worth(AggCtx *c, const JoinCtx *jc, int k, Var *key);
static PlannerInfo *cte_inner_body(PlannerInfo *body, int *resno);
static double list_keys_worth(PlannerInfo *root, PlannerInfo *body, Var *key);

/* is the count of a table's lookup a default guess: a filter of it that PostgreSQL has no statistics for? (a CTE or sub-select is estimated through its own plan) */
static bool
lookup_guessed(AggCtx *c, const JoinCtx *jc)
{
	return planner_rt_fetch(jc->rti, c->root)->rtekind == RTE_RELATION && !lookup_counted(c, jc);
}

/*
 * The share of the units that hold one of `keys` values of the leaf (nd distinct): a unit whose zone is the width w (as a share of the leaf's
 * range) has one in it with the probability 1 - (1 - w)^keys, counted on the sample of the real zones, where the mean width (a few wide
 * zones among narrow ones make a mean that keeps every unit) would say keys times it. The units that pass keep a summary of their values
 * (set_cover of them): the n values of a unit, set_keys on average and in proportion to its zone's width (at least one, at most the
 * values the zone spans), hold one of the keys with the probability 1 - exp(-keys * n / nd). A source with no sample has the mean width.
 */
static double
zone_share(AggCtx *c, int leaf, double keys, double nd)
{
	const SiftSrcZones *z = c->stats.zones;
	double		range = c->stats.max[leaf] - c->stats.min[leaf] + 1.0,
				cover = c->stats.set_cover[leaf],
				mean_w = 0,
				kept = 0;

	if (z == NULL || z->n == 0 || z->lo[leaf] == NULL || range <= 1.0)
		return c->stats.zone_span[leaf] > 0 ? Min(keys * c->stats.zone_span[leaf], 1.0) : zone_overlap(c, leaf, Min(keys / nd, 1.0));
	for (int i = 0; i < z->n; i++)
		mean_w += Max(((double) z->hi[leaf][i] - (double) z->lo[leaf][i] + 1.0) / range, 0.0) / z->n;
	for (int i = 0; i < z->n; i++)
	{
		double		w = ((double) z->hi[leaf][i] - (double) z->lo[leaf][i] + 1.0) / range,
					in_zone,
					n,
					held = 1.0;

		if (w <= 0)
			continue;
		in_zone = w >= 1.0 ? 1.0 : -expm1(keys * log1p(-w));
		if (cover > 0)
		{
			n = Min(Max(c->stats.set_keys[leaf] * w / mean_w, 1.0), Max(w * nd, 1.0));
			held = 1.0 - cover + cover * -expm1(-keys * n / nd);
		}
		kept += in_zone * held;
	}
	return z->covered * kept / z->n + (1.0 - z->covered);
}

/* the same keys of a leaf are asked again by every path and every pricing pass of a plan, and the sum is over the sample of the zones: the last few answers are kept */
static double
zone_keys_kept(AggCtx *c, int leaf, double keys, double nd)
{
	AggZoneMemo *m;

	for (int i = 0; i < c->nzone_memo; i++)
	{
		m = &c->zone_memo[i];
		if (m->leaf == leaf && m->keys == keys && m->nd == nd)
			return m->kept;
	}
	m = &c->zone_memo[c->nzone_memo < lengthof(c->zone_memo) ? c->nzone_memo++ : 0];
	*m = (AggZoneMemo) {leaf, keys, nd, zone_share(c, leaf, keys, nd)};
	return m->kept;
}

/*
 * The share of the units the point directory of a leaf leaves for a list of `keys` of its nd distinct values: the rows it lists (keys x the rows of a key)
 * lie in the units like balls in bins, a unit with one is read (and narrowed to those rows, col/scan.c), and one without is not. A list the directory does not
 * answer (col/point.h: too many keys or rows) or a leaf without one keeps every unit. SiftColSource asks the directory of each segment; the units of one
 * that has none stay, as the share of the units that have one says (point_cover).
 */
static double
point_kept(AggCtx *c, int leaf, double keys, double nd)
{
	double		cover = c->stats.point_cover[leaf],
				rows,
				units = Max(c->stats.nunits, 1.0);

	if (cover <= 0 || nd <= 0 || keys > SIFT_CPOINT_MAX_KEYS || (keys > SIFT_CPOINT_MIN_KEYS && keys > c->stats.nrows / 16))
		return 1.0;
	rows = keys * Max(c->stats.nrows, 1.0) / nd;
	if (rows > SIFT_CPOINT_MAX_ROWS || rows > Max((double) SIFT_CPOINT_DENSE_ROWS, c->stats.nrows / SIFT_CPOINT_DENSE_SHARE))
		return 1.0;
	return 1.0 - cover + cover * -expm1(-rows / units);
}

/*
 * The units the join key filters leave (join_filters in agg/init.c): an inner or semi join on an integer leaf tests the leaf against
 * the keys of the lookup, narrowed to the rows a later inner or semi join keyed on one of its columns can match (a scope list joined
 * to the table's rows). The keys are that many of the distinct values of the leaf: the share of the leaf's range the zones keep, as for
 * a comparison. A CTE of a literal list has no more keys than the list holds: key_list_lift raises its rows to those its keys own, so a key of many
 * rows counts as many keys there, and the zones of the units hold keys, not rows. *n: the filters the scan applies; *sel_keys: the keys of the most
 * selective one (a share of the leaf's distinct values),
 * HUGE_VAL without a filter. The keys of a lookup PostgreSQL has no statistics for (a CTE or sub-select of ids) are its floor of one row,
 * whatever the list holds: they stay as estimated where they leave a handful of units (an index cut by the key reads a few for any key),
 * and are the geometric mean of them and the leaf's distinct values where they leave more (the planner cannot tell a key of ten rows from
 * one of ten thousand, and an index not cut by the key reads tens of units for one, so the scan is priced for a guess that is not a floor). The price of
 * the workers (workers_ratio) takes that mean always: a guess of a key or two that leaves three units prices a scan no worker pays for, where the keys are
 * tens and the scan reads a hundred units, and the workers that start for a scan that turns out short are called back by par_scan_small.
 */
static double
join_key_zone(AggCtx *c, SiftAggSpec *s, int *n, double *sel_keys, double *share, double *excess)
{
	double		kept = 1.0,
				selective = 1.0;

	*n = 0;
	*sel_keys = HUGE_VAL;
	*share = 1.0;
	*excess = 1.0;
	if (!sift_agg_join_filter || s->topn)
		return kept;
	/*
	 * A rest of an ON clause that compares values of the tuple holds only for the tuple: the tables are loaded whole, their keys are all there
	 * are, and the rows PostgreSQL expects of a lookup are what the join keeps, not what a filter can cut.
	 */
	for (int ci = 0; ci < s->ncombo; ci++)
		if (on_tests_tuple(s, ci))
			return kept;
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		const SiftAggCombo *cb = &s->combo[jc->combo];
		const SiftAggJoin *j = &s->join[cb->join];
		int			dkey = 0;
		const JoinCtx *dj = dependent_join(c, s, jc, &dkey);
		double		keys = Min(lookup_rows(c, jc), dj ? lookup_rows(c, dj) : HUGE_VAL),
					sure = Min(lookup_sure(c, jc), dj ? lookup_sure(c, dj) : HUGE_VAL);

		if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || cb->level != 0)
			continue;
		for (int k = 0; k < j->nkey; k++)
		{
			int			leaf = j->keypos[k] < 0 ? -1 : cb->leaf[j->keypos[k]];
			const SiftSrcLeaf *lf = leaf < 0 || leaf >= c->lay->nleaves - c->nvleaf ? NULL : &c->lay->leaf[leaf];
			Node	   *e = lf && lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
			double		nd,
						nkeys,
						list_keys,
						kept_keys;

			if (lf == NULL || lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->vtype != SCV_I32 || e == NULL || !IsA(e, Var) ||
				(nd = column_distinct(c, (Var *) e)) <= 0)
				continue;
			list_keys = Min(cte_list_keys(c, jc, k), dj ? cte_list_keys(c, dj, dkey) : HUGE_VAL);
			if (sure / nd < selective && !lookup_guessed(c, jc) && (dj == NULL || !lookup_guessed(c, dj)))
			{
				selective = sure / nd;
				*sel_keys = sure;
				*excess = list_keys < HUGE_VAL ? Max(cte_list_worth(c, jc, k, (Var *) e) / sure, 1.0) : 1.0;
				*share = selective * *excess;
			}
			nkeys = Min(keys, list_keys);
			kept_keys = Min(zone_keys_kept(c, leaf, nkeys, nd), point_kept(c, leaf, nkeys, nd));
			if (nd > keys && !lookup_counted(c, jc) && (pricing_wide || kept_keys * Max(c->stats.nunits, 1.0) > AGG_CUT_UNITS))
			{
				nkeys = Min(sqrt(keys * nd), list_keys);
				kept_keys = Min(zone_keys_kept(c, leaf, nkeys, nd), point_kept(c, leaf, nkeys, nd));
			}
			kept = Min(kept, kept_keys);
			(*n)++;
		}
	}
	return kept;
}

/* does PostgreSQL know the values a filter of a table compares (statistics of the column or of an expression index)? Its guess of what the filter keeps is a default otherwise */
static bool
filter_has_stats(PlannerInfo *root, RestrictInfo *ri)
{
	List	   *args = IsA(ri->clause, OpExpr) ? ((OpExpr *) ri->clause)->args :
		IsA(ri->clause, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) ri->clause)->args : NIL;
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft,
				known;

	if (list_length(args) != 2 || !get_restriction_variable(root, args, 0, &vardata, &other, &varonleft))
		return true;
	known = HeapTupleIsValid(vardata.statsTuple) || vardata.isunique;
	ReleaseVariableStats(vardata);
	return known;
}

/*
 * Are the rows PostgreSQL expects of a lookup its statistics' count? A table whose filters all have statistics, a VALUES list and a function of
 * constants (unnest of a list) are; a sub-select, a CTE or a function of other arguments is estimated through its own joins and defaults.
 */
static bool
lookup_counted(AggCtx *c, const JoinCtx *jc)
{
	RangeTblEntry *rte = planner_rt_fetch(jc->rti, c->root);

	if (rte->rtekind == RTE_VALUES)
		return true;
	if (rte->rtekind == RTE_FUNCTION)
	{
		RangeTblFunction *rtf = list_length(rte->functions) == 1 ? linitial_node(RangeTblFunction, rte->functions) : NULL;

		if (rtf == NULL || !IsA(rtf->funcexpr, FuncExpr))
			return false;
		foreach_ptr(Node, a, ((FuncExpr *) rtf->funcexpr)->args)
			if (!IsA(a, Const))
				return false;
		return true;
	}
	if (rte->rtekind != RTE_RELATION)
		return false;
	foreach_node(RestrictInfo, ri, c->root->simple_rel_array[jc->rti]->baserestrictinfo)
		if (!filter_has_stats(c->root, ri))
			return false;
	return true;
}

/*
 * How many times more rows than PostgreSQL expects the filters of a rel keep, when one of them has no statistics (a jsonb path, an expression,
 * a column of a sub-select's output): its guess is a default, a few hundred of a large table, and the groups of that many rows are fewer
 * than the keys have. The rows are taken as the geometric mean of the guess and the rel's rows, as child_rows does. A test for NULL keeps
 * its default only on a rel that is not a table: a table's column has statistics once it is analyzed, so a table's plan is as it was.
 */
static double
rel_filter_ratio(PlannerInfo *root, RelOptInfo *rel)
{
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
	{
		bool		known = filter_has_stats(root, ri);

		if (known && rel->rtekind != RTE_RELATION && IsA(ri->clause, NullTest))
		{
			VariableStatData vardata;

			examine_variable(root, (Node *) ((NullTest *) ri->clause)->arg, 0, &vardata);
			known = HeapTupleIsValid(vardata.statsTuple);
			ReleaseVariableStats(vardata);
		}
		if (!known)
			return sqrt(Max(rel->tuples / Max(rel->rows, 1.0), 1.0));
	}
	return 1.0;
}

double
filter_rows_ratio(AggCtx *c)
{
	return rel_filter_ratio(c->root, c->rel);
}

static bool
reads_column_index(Node *node, void *context)
{
	bool		found = false;

	if (node != NULL && IsA(node, RangeTblEntry))
	{
		RangeTblEntry *rte = (RangeTblEntry *) node;

		found = rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid);
	}
	else if (node != NULL && IsA(node, Query))
		found = query_tree_walker((Query *) node, reads_column_index, context, QTW_EXAMINE_RTES_BEFORE);
	else if (node != NULL)
		found = expression_tree_walker(node, reads_column_index, context);
	return found;
}

/*
 * The rows of a sub-select or CTE scan whose filters PostgreSQL has no statistics for (a column of a window, an aggregate or a grouping
 * sets output) are its default guesses multiplied: two of them keep a thousandth of the rows, and every join above reads that as one row and
 * rescans an aggregate for each of the rows that come. In a statement that reads a table with a column index the rows are lifted to the geometric
 * mean of the guess and the rows (rel_filter_ratio), never below PostgreSQL's own. This is a floor, not an estimate: a count carried through
 * the levels from the column index would be one.
 */
void
derived_rel_rows(PlannerInfo *root, RelOptInfo *rel)
{
	double		rows;
	PlannerInfo *top = root;

	if (!sift_enable_agg || rel->baserestrictinfo == NIL || rel->rows >= rel->tuples)
		return;
	rows = clamp_row_est(rel->rows * rel_filter_ratio(root, rel));
	if (rows <= rel->rows)
		return;
	while (top->parent_root)
		top = top->parent_root;
	if (!reads_column_index((Node *) top->parse, NULL))
		return;
	elog(DEBUG1, "sift: rows of a scan whose filters have no statistics %.0f, not %.0f (of %.0f)", rows, rel->rows, rel->tuples);
	rel->rows = rows;
	foreach_ptr(Path, p, rel->pathlist)
		p->rows = rows;
}

/*
 * the most rows a node expects among those that read a table with a column index, in a plan; *reads: this node reads one. A node that
 * keeps fewer rows than it reads (an aggregate, a grouping, a LIMIT, a set operation) ends the count: what comes out of it is its own
 * estimate, not the rows of the table below, which a body that aggregates does not return.
 */
static double
plan_leaf_rows(PlannerInfo *subroot, Plan *plan, bool *reads)
{
	double		rows = 0;
	bool		below = false;

	if (plan == NULL)
		return 0;
	check_stack_depth();
	switch (nodeTag(plan))
	{
		case T_Agg:
		case T_Group:
		case T_Unique:
		case T_Limit:
		case T_SetOp:
			{
				bool		r = false;

				plan_leaf_rows(subroot, plan->lefttree, &r);
				*reads = r;
				return plan->plan_rows;
			}
		case T_SeqScan:
		case T_IndexScan:
		case T_IndexOnlyScan:
		case T_BitmapHeapScan:
			{
				RangeTblEntry *rte = rt_fetch(((Scan *) plan)->scanrelid, subroot->parse->rtable);

				below = rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid);
				break;
			}
		case T_Append:
			foreach_ptr(Plan, c, ((Append *) plan)->appendplans)
			{
				bool		r = false;

				rows = Max(rows, plan_leaf_rows(subroot, c, &r));
				below |= r;
			}
			break;
		default:
			break;
	}
	for (int i = 0; i < 2; i++)
	{
		bool		r = false;

		rows = Max(rows, plan_leaf_rows(subroot, i == 0 ? plan->lefttree : plan->righttree, &r));
		below |= r;
	}
	*reads = below;
	return below ? Max(rows, plan->plan_rows) : rows;
}

typedef struct KeyList
{
	List	   *tlist;
	Node	   *clause;
} KeyList;

/* the arguments of an equality of a column and a constant, or of a column and any of the constants of an array */
static List **
key_clause_args(Node *clause)
{
	if (IsA(clause, OpExpr))
		return &((OpExpr *) clause)->args;
	if (IsA(clause, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) clause)->useOr)
		return &((ScalarArrayOpExpr *) clause)->args;
	return NULL;
}

/* a column the CTE returns equal to a constant or to any of the constants of an array: the clause of a literal list, which is an OpExpr for one key */
static bool
key_list_walker(Node *node, KeyList *kl)
{
	List	  **args;

	if (node == NULL)
		return false;
	if ((args = key_clause_args(node)) != NULL && list_length(*args) == 2 && IsA(linitial(*args), Var) && IsA(lsecond(*args), Const) &&
		op_mergejoinable(IsA(node, OpExpr) ? ((OpExpr *) node)->opno : ((ScalarArrayOpExpr *) node)->opno, exprType(linitial(*args))))
		foreach_node(TargetEntry, te, kl->tlist)
			if (IsA(te->expr, Var) && ((Var *) te->expr)->varno == ((Var *) linitial(*args))->varno &&
				((Var *) te->expr)->varattno == ((Var *) linitial(*args))->varattno)
			{
				kl->clause = node;
				return true;
			}
	return expression_tree_walker(node, key_list_walker, kl);
}

/*
 * A CTE of the keys a request names (a literal list, kept to the keys a sub-select allows) is estimated at the rows of two selectivities
 * multiplied: a hundred keys joined to a hundred come out as one, and everything that reads the keys (the plan that keeps the CTE) is
 * priced for one key, whatever the keys own. The rows are lifted to the keys the list is worth for the table with a column index that the
 * keys are joined to: its selectivity from the statistics of that column (the share of the table the listed keys own, a key of the
 * most common values with its own frequency), over the share of one key. A list of keys with few rows stays at the rows PostgreSQL expects.
 */
static double
list_keys_worth(PlannerInfo *root, PlannerInfo *body, Var *key)
{
	KeyList		kl = {body->parse->targetList, NULL};
	Node	   *list;
	VariableStatData vardata;
	bool		isdefault;
	double		nd;

	if (body->parse->jointree == NULL)
		return 0;
	key_list_walker((Node *) body->parse->jointree, &kl);
	if ((list = kl.clause) == NULL || key->vartype != exprType(linitial(*key_clause_args(list))) || key->varcollid != exprCollation(linitial(*key_clause_args(list))))
		return 0;
	examine_variable(root, (Node *) key, 0, &vardata);
	nd = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	if (isdefault)
		return 0;
	list = copyObject(list);
	linitial(*key_clause_args(list)) = key;
	return clamp_row_est(nd * clause_selectivity(root, list, 0, JOIN_INNER, NULL));
}

static void
key_list_lift(PlannerInfo *root, RelOptInfo *rel, PlannerInfo *body)
{
	PlannerInfo *top = root;
	Var		   *key = NULL;
	double		rows;

	foreach_node(EquivalenceClass, ec, root->eq_classes)
	{
		if (!bms_is_member(rel->relid, ec->ec_relids))
			continue;
		foreach_node(EquivalenceMember, em, ec->ec_members)
		{
			RangeTblEntry *rte;

			if (!IsA(em->em_expr, Var) || ((Var *) em->em_expr)->varlevelsup != 0)
				continue;
			rte = planner_rt_fetch(((Var *) em->em_expr)->varno, root);
			if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && has_column_source(rte->relid))
				key = (Var *) em->em_expr;
		}
	}
	if (key == NULL)
		return;
	rows = list_keys_worth(root, body, key);
	foreach_node(TargetEntry, te, body->parse->targetList)
	{
		int			resno = te->resno;
		PlannerInfo *inner = cte_inner_body(body, &resno);

		if (inner != body)
			rows = Max(rows, list_keys_worth(root, inner, key));
	}
	while (top->parent_root)
		top = top->parent_root;
	if (rows <= rel->rows || !reads_column_index((Node *) top->parse, NULL))
		return;
	elog(DEBUG1, "sift: rows of a CTE scan %.0f, not %.0f (the keys of its list own that share of the table)", rows, rel->rows);
	rel->rows = rows;
	foreach_ptr(Path, p, rel->pathlist)
		p->rows = rows;
}

/*
 * How many times derived_cte_rows raised the rows of a CTE scan in the plan being made (the largest ratio): plan_variants takes it after the
 * shared plan and passes it on, so what is left here after an error or a nested plan is not read.
 */
double		agg_cte_lift = 1.0;

/* the number of the plan of a CTE (an index of root->glob->subroots and subplans, from 1), 0 when it has none */
static int
cte_plan_id(PlannerInfo *root, RangeTblEntry *rte)
{
	PlannerInfo *cteroot = root;
	int			ndx = 0;

	for (Index lv = rte->ctelevelsup; lv > 0; lv--)
		cteroot = cteroot->parent_root;
	foreach_node(CommonTableExpr, cte, cteroot->parse->cteList)
	{
		if (strcmp(cte->ctename, rte->ctename) == 0)
			break;
		ndx++;
	}
	return ndx < list_length(cteroot->cte_plan_ids) ? list_nth_int(cteroot->cte_plan_ids, ndx) : 0;
}

/* a column of a CTE that the query equates to the column v: the key of an inner join of a table with the keys of that CTE */
static Var *
cte_equal_var(PlannerInfo *body, Node *n, Var *v)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, FromExpr))
	{
		Var		   *r;

		foreach_ptr(Node, c, ((FromExpr *) n)->fromlist)
			if ((r = cte_equal_var(body, c, v)) != NULL)
				return r;
		return cte_equal_var(body, ((FromExpr *) n)->quals, v);
	}
	if (IsA(n, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) n;
		Var		   *r;

		/* the rows of the preserved side of a left join are those of the keys, whatever it adds */
		if (j->jointype != JOIN_INNER && j->jointype != JOIN_LEFT)
			return NULL;
		if ((r = cte_equal_var(body, j->larg, v)) != NULL)
			return r;
		if (j->jointype == JOIN_LEFT)
			return NULL;
		if ((r = cte_equal_var(body, j->rarg, v)) != NULL)
			return r;
		return cte_equal_var(body, j->quals, v);
	}
	if (IsA(n, List) || (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop == AND_EXPR))
	{
		foreach_ptr(Node, a, IsA(n, List) ? (List *) n : ((BoolExpr *) n)->args)
		{
			Var		   *r = cte_equal_var(body, a, v);

			if (r != NULL)
				return r;
		}
		return NULL;
	}
	if (IsA(n, OpExpr) && list_length(((OpExpr *) n)->args) == 2 && op_mergejoinable(((OpExpr *) n)->opno, exprType(linitial(((OpExpr *) n)->args))))
	{
		Node	   *a = linitial(((OpExpr *) n)->args),
				   *b = lsecond(((OpExpr *) n)->args);

		if (IsA(a, Var) && IsA(b, Var))
		{
			Var		   *x = (Var *) a,
					   *y = (Var *) b;

			if (x->varno == v->varno && x->varattno == v->varattno && x->varlevelsup == 0 && y->varlevelsup == 0 && rt_fetch(y->varno, body->parse->rtable)->rtekind == RTE_CTE)
				return y;
			if (y->varno == v->varno && y->varattno == v->varattno && y->varlevelsup == 0 && x->varlevelsup == 0 && rt_fetch(x->varno, body->parse->rtable)->rtekind == RTE_CTE)
				return x;
		}
	}
	return NULL;
}

/*
 * The body a CTE column comes from when the CTE only passes it on: a join of the keys of another CTE with the table that names them (a CTE
 * of the table's rows for the ids of another) exports the column of the table, equal to that of the inner CTE, and the list that column holds is the inner
 * CTE's. *resno becomes the column of the inner body; body itself when the column is anything else.
 */
static PlannerInfo *
cte_inner_body(PlannerInfo *body, int *resno)
{
	for (;;)
	{
		RangeTblEntry *rte;
		Var		   *v = NULL;
		int			id;

		foreach_node(TargetEntry, te, body->parse->targetList)
			if (te->resno == *resno && !te->resjunk && IsA(te->expr, Var))
				v = (Var *) te->expr;
		if (v == NULL || v->varlevelsup != 0)
			return body;
		rte = rt_fetch(v->varno, body->parse->rtable);
		if (rte->rtekind == RTE_RELATION && body->parse->jointree != NULL)
		{
			Var		   *eq = cte_equal_var(body, (Node *) body->parse->jointree, v);

			if (eq == NULL)
				return body;
			v = eq;
			rte = rt_fetch(v->varno, body->parse->rtable);
		}
		if (rte->rtekind != RTE_CTE || rte->self_reference || (id = cte_plan_id(body, rte)) <= 0)
			return body;
		body = list_nth(body->glob->subroots, id - 1);
		*resno = v->varattno;
	}
}

/*
 * The keys a lookup that is a CTE of a literal list (key_list_walker) can hold, when its key `key` is the column the list is of: the constants of
 * the list. HUGE_VAL for any other lookup, and for a key that is another column of the CTE (the list says nothing of its values).
 */
static double
cte_list_keys(AggCtx *c, const JoinCtx *jc, int key)
{
	RangeTblEntry *rte = planner_rt_fetch(jc->rti, c->root);
	int			plan_id = rte->rtekind == RTE_CTE ? cte_plan_id(c->root, rte) : 0;
	PlannerInfo *body;
	KeyList		kl = {NULL, NULL};
	List	  **args;
	ArrayType  *arr;
	bool		found = false;
	int			resno = jc->j.keyatt[key];

	if (plan_id <= 0)
		return HUGE_VAL;
	body = cte_inner_body(list_nth(c->root->glob->subroots, plan_id - 1), &resno);
	kl.tlist = body->parse->targetList;
	if (body->parse->jointree == NULL)
		return HUGE_VAL;
	key_list_walker((Node *) body->parse->jointree, &kl);
	if (kl.clause == NULL || (args = key_clause_args(kl.clause)) == NULL || ((Const *) lsecond(*args))->constisnull)
		return HUGE_VAL;
	foreach_node(TargetEntry, te, kl.tlist)
		if (te->resno == resno)
		{
			Var		   *listed = linitial(*args);

			if (!IsA(te->expr, Var) || ((Var *) te->expr)->varno != listed->varno || ((Var *) te->expr)->varattno != listed->varattno)
				return HUGE_VAL;
			found = true;
		}
	if (!found)
		return HUGE_VAL;
	if (IsA(kl.clause, OpExpr))
		return 1.0;
	arr = DatumGetArrayTypeP(((Const *) lsecond(*args))->constvalue);
	return ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
}

/* the keys of the table the literal list a lookup CTE holds is worth (list_keys_worth), for its column k: 0 for any other lookup */
static double
cte_list_worth(AggCtx *c, const JoinCtx *jc, int k, Var *key)
{
	RangeTblEntry *rte = planner_rt_fetch(jc->rti, c->root);
	int			plan_id = rte->rtekind == RTE_CTE ? cte_plan_id(c->root, rte) : 0;
	int			resno = jc->j.keyatt[k];

	return plan_id > 0 ? list_keys_worth(c->root, cte_inner_body(list_nth(c->root->glob->subroots, plan_id - 1), &resno), key) : 0;
}

/*
 * A shared CTE whose body joins a table with a column index to filtered tables is estimated at a row or two (each join keeps the share of a
 * default guess, and a dimension filter that owns all the rows of the table keeps a hundredth), and everything that reads it is priced from
 * that: the shared plan costs the same at ten times the rows as at one, and the scans that replace it lose the comparison. The rows of the
 * scan are lifted to the geometric mean of the estimate and the largest result the body builds from the table, as derived_rel_rows does for a
 * filter; never fewer rows than PostgreSQL's own. This is a floor, not an estimate. It is made for the shared plan only: the inline plan
 * (agg_variant) has no such CTE to price, and the aggregates the scans leave in it would be priced for more rows than the scans read.
 */
void
derived_cte_rows(PlannerInfo *root, RelOptInfo *rel)
{
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	int			plan_id;
	double		leaf,
				rows;
	bool		reads;

	if (!sift_enable_agg || agg_variant != NULL || rte->self_reference || (plan_id = cte_plan_id(root, rte)) <= 0)
		return;
	leaf = plan_leaf_rows(list_nth(root->glob->subroots, plan_id - 1), list_nth(root->glob->subplans, plan_id - 1), &reads);
	if (!reads)
	{
		key_list_lift(root, rel, list_nth(root->glob->subroots, plan_id - 1));
		return;
	}
	rows = clamp_row_est(sqrt(rel->rows * leaf));
	if (rows <= rel->rows)
		return;
	elog(DEBUG1, "sift: rows of a CTE scan %.0f, not %.0f (its body builds %.0f from a table with a column index)", rows, rel->rows, leaf);
	agg_cte_lift = Max(agg_cte_lift, rows / rel->rows);
	rel->rows = rows;
	foreach_ptr(Path, p, rel->pathlist)
		p->rows = rows;
}

/*
 * How many times more rows the key of a row owns than the mean: nd * sum(freq^2) over the values of the column, the frequencies of its
 * most common values by PostgreSQL's statistics and the rest of the rows spread evenly. 1 for a flat column or one without statistics.
 * The mean is what PostgreSQL prices a probe of the key with, and the keys a query passes are the ones with rows to look at.
 */
static double
column_skew(AggCtx *c, Var *v)
{
	HeapTuple	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid), Int16GetDatum(v->varattno), BoolGetDatum(false));
	AttStatsSlot sslot;
	double		nd,
				left,
				sq = 0;

	if (!HeapTupleIsValid(tup))
		return 1.0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	nd = nd > 0 ? nd : -nd * c->rel->tuples;
	left = 1.0 - ((Form_pg_statistic) GETSTRUCT(tup))->stanullfrac;
	if (nd >= 1 && get_attstatsslot(&sslot, tup, STATISTIC_KIND_MCV, InvalidOid, ATTSTATSSLOT_NUMBERS))
	{
		for (int i = 0; i < sslot.nnumbers; i++)
		{
			sq += (double) sslot.numbers[i] * sslot.numbers[i];
			left -= sslot.numbers[i];
		}
		if (left > 0 && nd > sslot.nnumbers)
			sq += left * left / (nd - sslot.nnumbers);
		free_attstatsslot(&sslot);
	}
	else
		nd = 0;
	ReleaseSysCache(tup);
	return Max(nd * sq, 1.0);
}

/*
 * How many times more keys than PostgreSQL expects the scope lists of the query hold, for the normal plan's price. A lookup join on a
 * column of the scanned table (a list of ids, the rows of a sub-select) is counted by PostgreSQL from its own rows, and those are a
 * default guess (a row or two) when the filters of the lookup read a jsonb path, an expression or the output of a plan: the normal plan
 * is then priced for a handful of keys, though the lists the queries pass hold a good share of the table's distinct values. The keys
 * are taken as the geometric mean of the rows PostgreSQL expects and the distinct values of the scanned column (as child_rows does for
 * the rows of a set-returning function). A lookup PostgreSQL has statistics for keeps its estimate of the keys, and the rows of each are priced
 * by the skew of the column (column_skew, sift.agg_key_skew). With groups set (the rows the path reports, spec_finish) the keys are all the
 * distinct values and the skew is not counted: the groups are the keys' own.
 */
double
lookup_set_ratio(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, bool groups)
{
	double		f = 1.0;

	if (!sift_agg_join_filter || s->topn || sift_agg_param_set_size <= 0)
		return f;
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		const SiftAggCombo *cb = &s->combo[jc->combo];
		const SiftAggJoin *j = &s->join[cb->join];
		double		keys = lookup_rows(c, jc),
					nd = 0;
		Var		   *key = NULL;

		if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || cb->level != 0)
			continue;
		for (int k = 0; k < j->nkey; k++)
		{
			int			leaf = j->keypos[k] < 0 ? -1 : cb->leaf[j->keypos[k]];
			const SiftSrcLeaf *lf = leaf < 0 || leaf >= c->lay->nleaves - c->nvleaf ? NULL : &c->lay->leaf[leaf];
			Node	   *e = lf && lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
			double		d;

			if (lf != NULL && lf->kind == SCL_SCALAR && lf->parent < 0 && e != NULL && IsA(e, Var) && (d = column_distinct(c, (Var *) e)) > nd)
			{
				key = (Var *) e;
				nd = d;
			}
		}
		if (key == NULL)
			continue;
		/* a counted lookup's keys are as many as PostgreSQL says, but each has the rows of the keys a query passes, not the mean's */
		if (lookup_counted(c, jc))
		{
			if (sift_agg_key_skew && !groups)
				f *= sqrt(Max(column_skew(c, key) / AGG_KEY_SKEW_FLOOR, 1.0));
		}
		else if (nd > keys)
			f *= groups ? nd / keys : sqrt(nd / keys);
	}
	/* the normal plan reads no more than the rows the table's own filters keep, whatever the keys */
	return Min(f, Max(c->rel->rows / Max(input_rel->rows, 1.0), 1.0));
}

/*
 * The share of the units the zone tests of p leave (the scan tests each disjunct of an OR on its own, init_filter.c): a comparison keeps the
 * zones its selectivity overlaps, an AND the most selective of its conjuncts' share, an OR the sum of its disjuncts' (1: none refuses a unit).
 */
static double
pred_zone_kept(AggCtx *c, const SiftAggPred *p)
{
	double		kept = 1.0;

	if (p->kind == SAP_AND)
		for (int i = 0; i < p->nchild; i++)
			kept = Min(kept, pred_zone_kept(c, p->child[i]));
	else if (p->kind == SAP_OR)
	{
		kept = 0.0;
		for (int i = 0; i < p->nchild; i++)
			kept += pred_zone_kept(c, p->child[i]);
		kept = Min(kept, 1.0);
	}
	else if (sift_aggspec_zone_cmp(p, c->lay, c->lay->nleaves))
		kept = zone_overlap(c, p->leaf, Min(Max(p->sel, 0.0), 1.0));
	return kept;
}

/*
 * The share of the units the small-set summaries of the leaves the conjuncts test leave (col/setsum.h): an equality or IN on the share s of the leaf's
 * distinct values (p->sel for an integer leaf, nvals / ndistinct for a text one) keeps a unit that holds n of them with the probability
 * 1 - (1 - n / distinct) ^ (s * distinct), about 1 - exp(-s * n); a unit without a summary stays. A summary that keeps nearly every unit is not counted:
 * the scan reads it for nothing then, as for a leaf that has none.
 */
static double
set_kept(AggCtx *c, SiftAggSpec *s)
{
	double		kept = 1.0;

	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];
		SiftSrcStrTest st;
		double		share;

		if (p->kind == SAP_LUT && sift_aggspec_str_test(p, c->lay, c->lay->nleaves - c->nvleaf, &st) && (st.op == SSTR_EQ || st.op == SSTR_IN) &&
			c->stats.ndistinct[p->leaf] > 0)
			share = st.nvals / c->stats.ndistinct[p->leaf];
		else if ((p->kind == SAP_CMP && p->op == SCMP_EQ) || (p->kind == SAP_IN && p->op == 0))
		{
			share = Min(Max(p->sel, 0.0), 1.0);
			if (c->stats.ndistinct[p->leaf] > 0)
				kept = Min(kept, point_kept(c, p->leaf, share * c->stats.ndistinct[p->leaf], c->stats.ndistinct[p->leaf]));
		}
		else
			continue;
		if (c->stats.set_cover[p->leaf] > 0)
		{
			double		cover = c->stats.set_cover[p->leaf],
						held = 1.0 - exp(-share * c->stats.set_keys[p->leaf]);

			if (held < AGG_SET_USEFUL)
				kept = Min(kept, 1.0 - cover + cover * held);
		}
	}
	return kept;
}

/* does the point directory narrow a unit of the scan to the rows of an equality or IN list (set_kept), which are then the only rows its filters test? */
static bool
point_narrows(AggCtx *c, SiftAggSpec *s)
{
	bool		narrows = false;

	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];

		narrows |= ((p->kind == SAP_CMP && p->op == SCMP_EQ) || (p->kind == SAP_IN && p->op == 0)) && c->stats.ndistinct[p->leaf] > 0 &&
			point_kept(c, p->leaf, Min(Max(p->sel, 0.0), 1.0) * c->stats.ndistinct[p->leaf], c->stats.ndistinct[p->leaf]) < 1.0;
	}
	return narrows;
}

/*
 * The share of the units the zone tests leave: the comparisons (their selectivity is also the fraction of the leaf's range they keep) and
 * the ORs of them, the join key filters and a ctid range. The most selective counts, and the scan reads at least one unit (docs/COL-PLAN.md, "Zone pruning").
 */
static double
zone_kept(AggCtx *c, SiftAggSpec *s, int *nkeyfilters, double *key_share)
{
	double		sel_keys,
				share,
				excess,
				kept;
	List	   *rest;

	kept = join_key_zone(c, s, nkeyfilters, &sel_keys, &share, &excess);
	*key_share = Min(share, 1.0);
	kept = Min(kept, zone_sample_kept(c, s, &rest));
	kept = Min(kept, set_kept(c, s));
	foreach_ptr(SiftAggPred, p, rest)
		kept = Min(kept, p->kind == SAP_TID ? p->sel : pred_zone_kept(c, p));
	return kept;
}

/* does a predicate test the leaf? `values`: its values, not only its null flags, and the leaves of a combo slot */
bool
pred_uses_leaf(const SiftAggSpec *s, const SiftAggPred *p, int leaf, bool values)
{
	if (p == NULL)
		return false;
	if (values && p->kind == SAP_COMBO)
	{
		for (int i = 0; i < s->combo[p->leaf].nleaves; i++)
			if (s->combo[p->leaf].leaf[i] == leaf)
				return true;
		return false;
	}
	if (p->kind != SAP_AND && p->kind != SAP_OR && (p->leaf == leaf || p->leaf2 == leaf) && !(values && p->kind == SAP_NULL))
		return true;
	for (int i = 0; i < p->nchild; i++)
		if (pred_uses_leaf(s, p->child[i], leaf, values))
			return true;
	return false;
}

/* is the leaf read by something besides the text tests that run on the encoded strings? */
static bool
leaf_read_whole(AggCtx *c, SiftAggSpec *s, int leaf)
{
	SiftSrcStrTest t;

	/* combos and joins read leaves in ways not followed here */
	if (s->ncombo > 0 || c->joins != NIL)
		return true;
	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].leaf == leaf && s->key[i].kind != SAK_NUM && s->key[i].kind != SAK_SET)
			return true;
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].leaf == leaf || pred_uses_leaf(s, s->agg[i].filter, leaf, false))
			return true;
	for (int i = 0; i < s->npred; i++)
		if (pred_uses_leaf(s, s->pred[i], leaf, false) && !(s->pred[i]->leaf == leaf && sift_aggspec_str_test(s->pred[i], c->lay, c->lay->nleaves - c->nvleaf, &t)))
			return true;
	return false;
}

/* The average width of the heap column a plain text leaf is read from (0: unknown, or the leaf is a jsonb path or an expression) */
static int
leaf_heap_width(AggCtx *c, int leaf)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL && lf->parent < 0 ? list_nth(c->attexpr, lf->attno - 1) : NULL;

	return e && IsA(e, Var) ? get_attavgwidth(planner_rt_fetch(c->relid, c->root)->relid, ((Var *) e)->varattno) : 0;
}

/* the share of the rows of a leaf that PostgreSQL's statistics expect to hold a distinct value; 0 when it has none (a path of a document, an expression) */
static double
leaf_stat_frac(AggCtx *c, int leaf, double rows)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->path == NIL && lf->parent < 0 ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	HeapTuple	tup;
	double		nd = 0;

	if (e == NULL || !IsA(e, Var))
		return 0;
	tup = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid),
						  Int16GetDatum(((Var *) e)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tup))
		return 0;
	nd = ((Form_pg_statistic) GETSTRUCT(tup))->stadistinct;
	ReleaseSysCache(tup);
	return nd > 0 ? Min(nd / Max(rows, 1.0), 1.0) : Min(-nd, 1.0);
}

/* ... 1 when it has none */
static double
leaf_distinct_frac(AggCtx *c, int leaf, double rows)
{
	double		frac = leaf_stat_frac(c, leaf, rows);

	return frac > 0 ? frac : 1.0;
}

/*
 * Text leaves of long strings: a unit stores the strings inline, and reading the leaf as codes builds a dictionary of the unit
 * (measured 0.5 us a row, which the heap scan beats when few rows pass); a test on the encoded strings costs a few ns a row.
 */
static double
inline_text_cost(AggCtx *c, SiftAggSpec *s)
{
	double		cost = 0;

	for (int i = 0; i < s->nleaves; i++)
	{
		const SiftSrcLeaf *lf = &c->lay->leaf[s->leaves[i]];

		if (lf->vtype == SCV_CODE && lf->kind == SCL_SCALAR && lf->parent < 0 && leaf_heap_width(c, s->leaves[i]) >= AGG_INLINE_WIDTH)
			cost += leaf_read_whole(c, s, s->leaves[i]) ? AGG_INLINE_ROW : AGG_INLINE_TEST;
	}
	return cost;
}

/*
 * The operators one unit costs from the pre-aggregates: the groups of a histogram, or the tuples of the rollup of the list (the product of the distinct
 * values of its dims, at most the elements of the unit and the 4096 a unit holds), which the executor leaves for the scan of the elements when they cost less.
 */
static double
preagg_unit_ops(AggCtx *c, SiftAggSpec *s, double units, double rows, double rows_out)
{
	double		tuples = 1.0,
				elems,
				ecost,
				tcost;
	SiftAggPaShape shape;

	if (s->group < 0 && (!sift_aggspec_preagg(s, c->lay, c->lay->nleaves, &c->pa, &shape) || !shape.rows))
	{
		double		sketches = 0;

		for (int i = 0; i < s->naggs; i++)
			sketches += s->agg[i].kind == SAA_GEN && s->agg[i].gen->hll >= 0 ? AGG_PREAGG_HLL : 0.0;
		return Min(rows_out, 256.0) * AGG_PREAGG_GROUP + sketches;
	}
	for (int r = 0; r < c->pa.nrollups; r++)
		if (c->pa.rollup[r].list == s->group)
			for (int d = 0; d < c->pa.rollup[r].ndims; d++)
				tuples *= Max(c->stats.ndistinct[c->pa.rollup[r].dim[d]], 2.0);
	elems = rows / units * (s->group >= 0 ? elems_of(c, s->group) : 1.0);
	sift_aggspec_unit_costs(s, c->lay, &ecost, &tcost);
	return Min(Min(tuples, 4096.0) * tcost * sift_agg_rollup_cost, elems * ecost) / SIFT_AGG_INSTR_PER_OP;
}

/* The share of the units the pre-aggregates answer (a unit is exact when its pages are all-visible, its superblock plain); 0 when the spec cannot use them */
double
preagg_share(AggCtx *c, SiftAggSpec *s)
{
	SiftAggPaShape shape;
	int			dim;
	double		miss = 0.0;
	bool		minmax = false;

	if (!sift_agg_preagg || !sift_aggspec_preagg(s, c->lay, c->lay->nleaves, &c->pa, &shape))
		return 0.0;
	/* a unit written before the options asked for the section it is answered from has none and is scanned: a half refreshed index is priced for the units that have it */
	for (int i = 0; i < s->naggs; i++)
		minmax |= s->agg[i].kind == SAA_MIN || s->agg[i].kind == SAA_MAX;
	dim = shape.dim >= 0 ? shape.dim : shape.hdim;
	if (shape.rows)
		miss = c->stats.rows_miss;
	else if (shape.list >= 0)
		miss = c->stats.roll_miss[shape.list];
	else if (dim >= 0)
		miss = minmax ? c->stats.mm_miss[dim] : c->stats.dim_miss[dim];
	/* a unit whose zone straddles a bound of the query is scanned, not answered: the units are exact only where a range on a leaf holds them whole */
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_CMP && s->pred[i]->leaf >= 0 && s->pred[i]->op != SCMP_NE)
			miss = 1.0 - (1.0 - miss) * (1.0 - Min(c->stats.zone_span[s->pred[i]->leaf], 1.0));
	return Min(Max(c->stats.exact_frac, 0.0), 1.0) * (1.0 - Min(Max(miss, 0.0), 1.0));
}

/* The bytes of a leaf's values in the executor's vectors: the weight of its share of the stored bytes */
static double
leaf_width(const SiftSrcLeaf *lf)
{
	return lf->vtype == SCV_I64 || lf->vtype == SCV_F64 ? 8.0 : 4.0;
}

/*
 * The stored bytes the scan reads: the leaves it reads take their share (by value width) of the compressed bytes of the source; a
 * source that does not know them (a table, a sample) is taken to compress its values by half.
 */
static double
scan_bytes(AggCtx *c, SiftAggSpec *s, double rows)
{
	double		all = 0,
				used = 0;

	for (int i = 0; i < c->lay->nleaves; i++)
		all += leaf_width(&c->lay->leaf[i]);
	for (int i = 0; i < s->nleaves; i++)
		used += leaf_width(leaf_of(c, s->leaves[i]));
	return c->stats.bytes > 0 ? c->stats.bytes * used / Max(all, 1.0) : rows * used * 0.5;
}

bool
extern_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXTERN)
		return true;
	return expression_tree_walker(node, extern_param_walker, context);
}

/* Does a qual of the table compare with a bound parameter? Its selectivity is a default guess of the planner, not the data's. */
static bool
rel_has_param_qual(RelOptInfo *rel)
{
	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		if (extern_param_walker((Node *) ri->clause, NULL))
			return true;
	return false;
}

/*
 * does the lookup that narrows the scan by its key hold a few keys, as PostgreSQL counts them? *rows: the rows of the table those keys own;
 * *excess: how many times the rows of one of them exceed the mean's, which PostgreSQL prices a probe of the key by
 */
bool
few_keys_lookup(AggCtx *c, SiftAggSpec *s, double *rows, double *excess)
{
	double		sel_keys,
				share,
				kept;
	int			n;

	kept = join_key_zone(c, s, &n, &sel_keys, &share, excess);
	*rows = c->rel->rows * Min(share, 1.0);
	return sel_keys <= AGG_PROBE_KEYS && (kept >= AGG_CUT_SHARE || kept * Max(c->stats.nunits, 1.0) > AGG_CUT_UNITS);
}

/*
 * The rows of an unnest PostgreSQL expects of the aggregate fall to a default when a join is on a range or the rows come from a function (a row,
 * or the 100 rows of a jsonb_each), though the table's own filters keep far more: the normal plan is priced for a few rows where the data has
 * thousands, and the scan, which reads the table's rows whatever the join keeps, loses a comparison it wins. The paths of the grouping stage that
 * are not the scan are charged the square root of the excess, as the normal plan of an unnest is on the other side of the same estimate (agg_cost).
 * Rows without an unnest keep PostgreSQL's estimate: the statistics of the joined tables are the ones it counts them by.
 */
double
input_rows_ratio(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, double rows)
{
	if (s->group < 0 || rel_has_param_qual(c->rel))
		return 1.0;
	return Max(sqrt(Max(rows, 1.0) * elems_of(c, s->group) / Max(input_rel->rows, 1.0)), 1.0);
}

/* does a btree index of the joined table start with columns that the table's `column = value` conditions give (map_load picks the same) */
static bool
join_index_usable(RelOptInfo *rel, const SiftAggJoin *j)
{
	foreach_node(IndexOptInfo, ii, rel->indexlist)
	{
		if (ii->relam != BTREE_AM_OID || ii->indpred != NIL || ii->hypothetical || ii->indexkeys[0] <= 0)
			continue;
		for (int k = 0; k < j->nidx; k++)
			if (j->idxatt[k] == ii->indexkeys[0] && op_in_opfamily(j->idxop[k], ii->opfamily[0]) && ii->indexcollations[0] == j->idxcoll[k])
				return true;
	}
	return false;
}

/* the heap pages of the scanned table: those of the partitions of a partitioned table */
double
rel_pages(AggCtx *c)
{
	return c->nparts > 0 ? c->part_pages : c->rel->pages;
}

/* The running terms of agg_cost: the work (in operator costs) is added in this order, which fixes its rounding */
typedef struct CostAcc
{
	double		rows;
	double		elems;
	double		sel_rows;
	double		units;
	double		open_cost;
	double		scan_cost;
	double		load_cost;		/* of scan_cost, the reads of the lookup tables (the leader's alone) and their rows */
	double		load_rows;
	double		cold_ops;		/* operators of the image leaves whose dictionaries a worker inflates for itself (AGG_WIDE_COLD) */
	double		gen_cost;
	double		child_cost;
	double		zone_kept;
	double		sparse;			/* of the rows of a unit that is read, the share whose dictionary entries are mapped: the kept rows when few pass (cost_read), else all */
	double		key_share;		/* of the rows the filters keep, the share whose join key a lookup holds (join_key_zone) */
	double		agg_rows;
	double		scan_rows;
	double		rows_out;
	double		lift;			/* the factor the scan is charged for the rows of an unnest the normal plan expects (agg_cost): the workers it starts cost as much */
} CostAcc;

/*
 * The rows read again from the heap: of the rows of the units a leaf the scan rechecks flags (the share of units the zones say), the share of them that
 * hold an overflow value (a sample of the flagged units says; AGG_RECHECK_FRAC where it could not be taken), one row of each unit at least. A table with
 * half of its documents dirty reads half of them again: past a few percent the normal plan is the cheaper one.
 */
static double
recheck_cost(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	double		rows = 0,
				flagged = 0;

	for (int i = 0; i < s->nleaves + s->nvleaf; i++)
	{
		int			leaf;
		double		frac;

		if (i < s->nleaves)
		{
			if (s->recheck[i] == SRK_NONE)
				continue;
			leaf = s->leaves[i];
		}
		else
		{
			if (s->vleaf[i - s->nleaves].recheck == SRK_NONE)
				continue;
			leaf = vleaf_source(c, i - s->nleaves);
		}
		frac = c->stats.ovf_frac[leaf] < 0 ? AGG_RECHECK_FRAC : c->stats.ovf_frac[leaf];
		rows = Max(rows, k->rows * c->stats.ovf_units[leaf] * frac);
		flagged = Max(flagged, c->stats.ovf_units[leaf]);
	}
	return Max(rows, k->units * flagged) * AGG_RECHECK_ROW;
}

/* is the leaf one the source tests on its stored values (a comparison, IN list or null test of the query's WHERE): decoded for every row of a unit */
static bool
leaf_filtered(const SiftAggSpec *s, int leaf)
{
	for (int i = 0; i < s->npred; i++)
		if ((s->pred[i]->kind == SAP_CMP || s->pred[i]->kind == SAP_IN || s->pred[i]->kind == SAP_NULL || s->pred[i]->kind == SAP_AND) &&
			pred_uses_leaf(s, s->pred[i], leaf, false))
			return true;
	return false;
}

/* The rows read and tested, the zones kept and the units opened */
/* is every reader of the leaf a list (array_agg, string_agg), a count of its values or a group key: its dictionary entries are copied, not computed on? */
static bool
leaf_copy_only(const SiftAggSpec *s, int leaf, bool *sums, bool *keyed)
{
	bool		copied = false;

	*sums = *keyed = false;

	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];

		bool		reads = a->leaf == leaf || bms_is_member(leaf, a->reads);

		if (a->kind == SAA_GEN)
			for (int j = 0; j < a->gen->nargs; j++)
				for (int l = 0; l < a->gen->arg[j].nleaves; l++)
					reads |= a->gen->arg[j].leaf[l] == leaf;
		if (!reads)
			continue;
		if (a->filter != NULL && pred_uses_leaf(s, a->filter, leaf, false))
			return false;
		if (a->wsum == leaf + 1)
			*sums = copied = true;
		else if (a->kind == SAA_COUNT && a->leaf == leaf)
			copied = true;
		else if (a->kind == SAA_GEN && !a->gen->distinct && !a->gen->ordered && !a->gen->sorted && !a->gen->hyp &&
				 (castNode(Aggref, a->gen->aggref)->aggfnoid == F_ARRAY_AGG_ANYNONARRAY || castNode(Aggref, a->gen->aggref)->aggfnoid == F_STRING_AGG_TEXT_TEXT ||
				  castNode(Aggref, a->gen->aggref)->aggfnoid == F_STRING_AGG_BYTEA_BYTEA))
			copied = true;
		else
			return false;
	}
	for (int i = 0; i < s->npred; i++)
		if (pred_uses_leaf(s, s->pred[i], leaf, false))
			return false;
	for (int k = 0; k < s->nkeys; k++)
		if (s->key[k].kind == SAK_COMBO || s->key[k].leaf == leaf)
			copied |= s->key[k].kind != SAK_COMBO;
	for (int ci = 0; ci < s->ncombo; ci++)
		for (int p = 0; p < s->combo[ci].nleaves; p++)
			if (s->combo[ci].leaf[p] == leaf)
			{
				bool		key = false;

				if (s->combo[ci].nleaves > 1 || s->combo[ci].join >= 0)
					return false;
				for (int k = 0; k < s->nkeys; k++)
					key |= s->key[k].kind == SAK_COMBO && s->key[k].leaf == ci;
				for (int ai = 0; ai < s->naggs; ai++)
					if (s->agg[ai].combo == ci && s->agg[ai].kind >= SAA_X_COUNT && s->agg[ai].kind <= SAA_X_AVG)
					{
						if (s->agg[ai].wsum == 0)
							return false;
						*sums = true;
					}
				if (!key)
					return false;
				copied = *keyed = true;
			}
	return copied;
}

/* is every reader of the leaf a sum or average that adds its dictionary entries (SiftAggAgg.wsum)? */
bool
leaf_wsum_only(const SiftAggSpec *s, int leaf)
{
	bool		sums = false,
				other = false;

	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];

		if (a->wsum == leaf + 1)
			sums = true;
		else
			other |= a->leaf == leaf || bms_is_member(leaf, a->reads);
		if (a->kind == SAA_GEN)
			for (int j = 0; j < a->gen->nargs; j++)
				for (int l = 0; l < a->gen->arg[j].nleaves; l++)
					other |= a->gen->arg[j].leaf[l] == leaf;
		other |= a->filter != NULL && pred_uses_leaf(s, a->filter, leaf, false);
	}
	for (int i = 0; i < s->npred; i++)
		other |= pred_uses_leaf(s, s->pred[i], leaf, false);
	for (int k = 0; k < s->nkeys; k++)
		other |= s->key[k].kind != SAK_COMBO && s->key[k].leaf == leaf;
	for (int ci = 0; ci < s->ncombo; ci++)
		for (int p = 0; p < s->combo[ci].nleaves; p++)
			other |= s->combo[ci].leaf[p] == leaf && !combo_wsum_only(s, ci);
	return sums && !other;
}

/* is the numeric leaf stored as images (no precision, or past 38 digits) rather than as 128-bit lanes? */
static bool
numeric_image(AggCtx *c, const SiftSrcLeaf *lf)
{
	Node	   *e = lf->path == NIL && lf->parent < 0 ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	int32		typmod = e && IsA(e, Var) ? ((Var *) e)->vartypmod : -1;
	int			precision = ((typmod - VARHDRSZ) >> 16) & 0xffff,
				scale = (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;

	return lf->wtypid == NUMERICOID && (typmod < 0 || precision > 38 || scale < 0 || scale > 38);
}

/*
 * The dictionary entries a wide leaf builds for each row read, as a share of the rows: a unit has its own dictionary, so a column of 50,000 values
 * read in units of 10,000 rows builds about 9,500 entries a unit, not a fifth of its rows.
 */
static double
wide_entry_frac(AggCtx *c, int leaf, const CostAcc *k)
{
	double		units = c->stats.nunits > 0 ? c->stats.nunits : k->units,
				nd = Max(leaf_distinct_frac(c, leaf, k->rows) * k->rows, 1.0),
				per_unit = k->rows / units;

	return Min(nd * -expm1(-per_unit / nd) * units / Max(k->rows, 1.0), 1.0);
}

/* the operators a row of an image leaf costs a worker for the dictionary it inflates, cold: more for a column of millions of distinct values (AGG_WIDE_BIG) */
static double
wide_cold_ops(AggCtx *c, int leaf, const CostAcc *k)
{
	double		nd = leaf_distinct_frac(c, leaf, k->rows) * k->rows;

	return AGG_WIDE_COLD * pow(Min(nd / 2000000.0, 1.0), 0.13) + Min(AGG_WIDE_BIG_MAX, AGG_WIDE_BIG * Max(nd - 2000000.0, 0.0) / 1000000.0);
}

/* the bytes of an array of the column an image leaf holds, 0 when the leaf is not the image of an array */
static double
array_image_width(AggCtx *c, int leaf)
{
	const SiftSrcLeaf *lf = &c->lay->leaf[leaf];
	Node	   *e = lf->wide && lf->parent < 0 && lf->path == NIL ? list_nth(c->attexpr, lf->attno - 1) : NULL;
	int			w;

	if (e == NULL || !OidIsValid(get_element_type(exprType(e))))
		return 0;
	w = leaf_heap_width(c, leaf);
	return w > 0 ? w : AGG_ARR_WIDTH;
}

/* is a value of the leaf read by a key, an aggregate or a combo (an image decoded), not only its null flags or a test? */
static bool
leaf_values_used(const SiftAggSpec *s, int leaf)
{
	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].leaf == leaf && s->key[i].kind != SAK_COMBO)
			return true;
	for (int i = 0; i < s->naggs; i++)
	{
		const SiftAggAgg *a = &s->agg[i];

		if (a->leaf == leaf || bms_is_member(leaf, a->reads) || pred_uses_leaf(s, a->filter, leaf, true))
			return true;
		if (a->kind == SAA_GEN)
			for (int j = 0; j < a->gen->nargs; j++)
				for (int l = 0; l < a->gen->arg[j].nleaves; l++)
					if (a->gen->arg[j].leaf[l] == leaf)
						return true;
	}
	for (int i = 0; i < s->ncombo; i++)
		for (int p = 0; p < s->combo[i].nleaves; p++)
			if (s->combo[i].leaf[p] == leaf)
				return true;
	return false;
}

/* does a conjunct of the query test a value of the leaf (every row's value is read, whatever few pass)? */
static bool
leaf_tested(const SiftAggSpec *s, int leaf)
{
	for (int i = 0; i < s->npred; i++)
		if (pred_uses_leaf(s, s->pred[i], leaf, true))
			return true;
	return false;
}

/* is a value of the leaf read by a key, an aggregate, a combo or a test (an image decoded), not only its null flags? */
static bool
leaf_values_read(const SiftAggSpec *s, int leaf)
{
	return leaf_values_used(s, leaf) || leaf_tested(s, leaf);
}

/* do only comparisons with constants read the leaf: the kernels test its lanes where they are, no value is decoded (0.3 ns a row measured for 3,000,000 rows of a date column) */
static bool
leaf_compared_only(const SiftAggSpec *s, int leaf)
{
	bool		tested = false;

	/* a top-N orders by columns that no key or aggregate names */
	if (s->topn || leaf_values_used(s, leaf))
		return false;
	for (int i = 0; i < s->npred; i++)
		if (pred_uses_leaf(s, s->pred[i], leaf, true))
		{
			if (s->pred[i]->kind != SAP_CMP)
				return false;
			tested = true;
		}
	return tested;
}

/*
 * The bytes of the arrays whose entries a scan decodes and tests: a leaf a filter tests is read whole, one the query only groups or aggregates at the
 * rows that pass (sparse), and none when all it reads of the leaf are its null flags.
 */
static double
array_bytes_read(AggCtx *c, const SiftAggSpec *s, int leaf, double sparse)
{
	double		bytes = array_image_width(c, leaf);

	if (bytes == 0 || !leaf_values_read(s, leaf))
		return 0;
	return bytes * (leaf_tested(s, leaf) ? 1.0 : sparse);
}

/* the entries of it, in operators */
static double
wide_entry_cost(AggCtx *c, const SiftAggSpec *s, int leaf, const CostAcc *k, double sparse)
{
	bool		sums,
				keyed;
	double		array = AGG_ARR_BIG * Max(array_bytes_read(c, s, leaf, sparse) - AGG_ARR_KNEE, 0.0),
				frac = wide_entry_frac(c, leaf, k) * (leaf_tested(s, leaf) ? 1.0 : sparse);	/* a leaf no conjunct tests: the entries of the rows that pass, a unit that keeps few is read at those rows only */

	if (leaf_copy_only(s, leaf, &sums, &keyed))
		return frac * ((sums ? (keyed ? AGG_WIDE_KEYSUM : AGG_WIDE_NUM) : AGG_WIDE_COPY) + array);
	return frac * ((numeric_image(c, &c->lay->leaf[leaf]) ? (leaf_wsum_only(s, leaf) ? AGG_WIDE_NUM : AGG_WIDE_IMAGE) : AGG_WIDE_ENTRY) + array);
}

/* The nanoseconds a row costs the tests of the IN lists of the predicate p beyond a small table in the cache: a list of spread values is a hash or bit table */
static double
in_tests_ns(AggCtx *c, const SiftAggPred *p)
{
	double		ns = 0;

	if (p->kind == SAP_AND || p->kind == SAP_OR)
		for (int i = 0; i < p->nchild; i++)
			ns += in_tests_ns(c, p->child[i]);
	else
		ns = Max(in_list_test_ns(c, p) - AGG_IN_TEST_BASE, 0);
	return ns;
}

/*
 * A key read as dictionary codes gives an id to every entry of the dictionary of each unit read, whether a row that passes the filters holds it
 * or not: a near-unique text column mapped over the units the zones leave costs its rows in entries, and the table of ids outgrows the groups
 * the query has. Returns the entries mapped; *ids: the ids they make (the most of one key); *ops: what mapping them costs.
 */
static double
key_entries(AggCtx *c, const SiftAggSpec *s, const CostAcc *k, double *ids, double *ops)
{
	double		units = Max(k->units, 1.0),
				read = Max(k->zone_kept, 1.0 / units) * units,
				total = 0;

	*ids = 0;
	*ops = 0;
	for (int i = 0; i < s->nkeys; i++)
	{
		const SiftSrcLeaf *lf;
		int			leaf = -1;
		double		frac,
					nd,
					entries;

		/* a wide column is a key through the combo of its leaf */
		if (s->key[i].kind == SAK_COMBO && s->combo[s->key[i].leaf].nleaves == 1 && s->combo[s->key[i].leaf].join < 0)
			leaf = s->combo[s->key[i].leaf].leaf[0];
		else if (s->key[i].kind == SAK_LEAF || s->key[i].kind == SAK_LUT)
			leaf = s->key[i].leaf;
		if (leaf < 0)
			continue;
		lf = &c->lay->leaf[leaf];
		/* without statistics the values are not known to be many; the combo of an integer column gives each value an id of the output table, as an entry of a dictionary does */
		if (s->key[i].kind == SAK_COMBO ? !(lf->wide ? lf->vtype == SCV_CODE : lf->vtype == SCV_I32) : (lf->vtype != SCV_CODE || lf->wide))
			continue;
		frac = leaf_stat_frac(c, leaf, k->rows);
		if (frac <= 0)
			continue;
		nd = Max(frac * k->rows, 1.0);
		if (lf->wide)
		{
			/* the units the column has, not a guess of 2,000 rows each */
			double		wunits = c->stats.nunits > 0 ? c->stats.nunits : units;

			entries = nd * -expm1(-k->rows / wunits * k->sparse / nd) * Max(k->zone_kept, 1.0 / wunits) * wunits;
		}
		else
			entries = nd * -expm1(-k->rows / units * k->sparse / nd) * read;
		total += entries;
		if (lf->wide)
			*ops += entries * AGG_KEY_WIDE * pow(Min(entries, nd) / 30000.0, 0.42);
		else
			*ops += entries * AGG_KEY_ENTRY * (pow(Max(Min(entries, nd) / AGG_KEY_IDS, 1.0), 0.35) - 1.0);
		*ids = Max(*ids, Min(entries, nd));
	}
	return total;
}

/* the tests a row goes through: the bounds of a range on one leaf are one pass over its lanes (0.65 ms for 1,000,000 rows with one bound or two, 1.04 with a second column) */
static int
filter_tests(const SiftAggSpec *s)
{
	int			tests = 0;

	for (int i = 0; i < s->npred; i++)
	{
		const SiftAggPred *p = s->pred[i];
		bool		bound = p->kind == SAP_CMP && p->op != SCMP_EQ && p->op != SCMP_NE,
					seen = false;

		for (int j = 0; bound && j < i; j++)
			seen |= s->pred[j]->kind == SAP_CMP && s->pred[j]->leaf == p->leaf && s->pred[j]->op != SCMP_EQ && s->pred[j]->op != SCMP_NE;
		tests += !seen;
	}
	return tests;
}

static void
cost_read(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	int			nkeyfilters;
	double		sparse,
				tested;

	k->zone_kept = zone_kept(c, s, &nkeyfilters, &k->key_share);
	/*
	 * A unit that few rows pass the filters is read at those rows only (docs/COL-SEARCH.md: a unit with a kept row in eight or fewer is
	 * compacted, its leaves decoded for the kept rows): the leaves the filters test are read whole, the others for the share that passes.
	 */
	sparse = Min(k->sel_rows / (k->rows * Max(k->zone_kept, 1.0 / k->units)), 1.0);
	sparse = sparse <= AGG_SPARSE_MAX ? Min(sparse * AGG_SPARSE_LEAF, 1.0) : 1.0;
	/* the rows the point directory lists are the only ones the filters read and test */
	tested = point_narrows(c, s) ? sparse : 1.0;
	k->sparse = sparse;
	for (int i = 0; i < s->nleaves; i++)
		if (!leaf_compared_only(s, s->leaves[i]))
			k->scan_cost += k->rows * (leaf_level(c, s->leaves[i]) ? elems_of(c, leaf_of(c, s->leaves[i])->parent) : 1.0) * AGG_SCAN_LEAF *
				(leaf_filtered(s, s->leaves[i]) ? tested : sparse);
	k->scan_cost += k->rows * (1 + (s->group >= 0 ? k->elems : 0)) * AGG_FILTER_VALUE * Max(filter_tests(s) + nkeyfilters, 1) * tested;
	k->scan_cost += k->rows * c->elem_work * AGG_FILTER_VALUE;
	for (int i = 0; i < s->npred; i++)
		k->scan_cost += k->rows * in_tests_ns(c, s->pred[i]) / (AGG_OP_NS * AGG_SCAN_SPEED) * tested;
	for (int i = 0; i < s->nvleaf; i++)
		k->scan_cost += sift_aggvleaf_member(&s->vleaf[i]) ? k->rows * elems_of(c, s->vleaf[i].obj) * AGG_VLEAF_ELEM : 0;
	k->scan_cost += k->rows * inline_text_cost(c, s);
	for (int i = 0; i < s->nleaves; i++)
		if (leaf_of(c, s->leaves[i])->wide)
		{
			k->scan_cost += k->rows * (AGG_WIDE_ROW + wide_entry_cost(c, s, s->leaves[i], k, sparse));
			if (numeric_image(c, leaf_of(c, s->leaves[i])))
				k->cold_ops += k->rows * wide_entry_frac(c, s->leaves[i], k) * wide_cold_ops(c, s->leaves[i], k);
		}
	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_WIDE && leaf_of(c, s->pred[i]->leaf)->vtype == SCV_CODE)
			k->scan_cost += k->rows * leaf_distinct_frac(c, s->pred[i]->leaf, k->rows) * AGG_WIDE_TEST;
	k->scan_cost = (k->scan_cost + k->units * AGG_UNIT_COST) * Max(k->zone_kept, 1.0 / k->units);
	k->cold_ops *= Max(k->zone_kept, 1.0 / k->units);
}

/* are the rows PostgreSQL expects of the joins counted: no correlated child and no function (its default is a thousand rows a call) */
static bool
joins_counted(AggCtx *c)
{
	bool		counted = true;

	foreach_ptr(JoinCtx, jc, c->joins)
		counted &= jc->j.params == NIL && planner_rt_fetch(jc->rti, c->root)->rtekind != RTE_FUNCTION;
	return counted;
}

/* The tuples that reach the groups and what each takes */
static void
cost_tuples(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, CostAcc *k)
{
	k->agg_rows = k->sel_rows * (s->group >= 0 ? k->elems : 1.0);
	/* the lookup joins reach the groups with the rows PostgreSQL expects of the join: fewer where they filter, more where a key of the table repeats (an unnest has its own count, tuple_group_count) */
	if (c->joins != NIL && !pricing_wide)
		k->agg_rows = s->group < 0 && joins_counted(c) ? Max(input_rel->rows, 1.0) : Min(k->agg_rows, Max(input_rel->rows, 1.0));
	else if (pricing_wide)
		k->agg_rows *= sqrt(Max(Min(k->zone_kept, 1.0), 1.0 / k->units));
	k->scan_rows = k->agg_rows;
	/* the rows of a preserved side no tuple matches are tuples too */
	foreach_ptr(JoinCtx, jc, c->joins)
		if (sift_aggjoin_preserves(jc->kind))
			k->agg_rows += Max(c->root->simple_rel_array[jc->rti]->tuples, 1.0);
	/* a group takes a tuple: the groups PostgreSQL expects of the unnested rows it assumes (a cap of 100 a call) are more than the elements hold */
	k->rows_out = Min(k->rows_out, Max(Max(s->nsets, 1) * k->agg_rows, 1.0));
	k->scan_cost += k->agg_rows * (Max(s->nsets, 1) * (AGG_TUPLE + s->nkeys * AGG_KEY_ROW + s->naggs * AGG_AGG_ROW) + s->ncombo * AGG_COMBO_ROW);
}

/*
 * A lookup table that a dependent lookup keys is read by a btree probe for each of its `keys`: a descent each and a heap page for each of the `rows` they find (at most
 * the table's pages), all random pages. It pays while that costs less than the pages of the table at the page cost of PostgreSQL's own scan of them (as join_load_rows
 * prices a table read whole).
 */
bool
sift_agg_probe_pays(double keys, double rows, double pages)
{
	return (keys + Min(rows, pages)) * random_page_cost < pages * Min(seq_page_cost, random_page_cost);
}

/* the rows a value of column att has: the rows of the table over the distinct values its statistics say; 0 when they say nothing */
double
sift_agg_rows_per_key(unsigned int relid, int att, double tuples)
{
	HeapTuple	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(relid), Int16GetDatum(att), BoolGetDatum(false));
	double		nd;

	if (!HeapTupleIsValid(tp))
		return 0;
	nd = ((Form_pg_statistic) GETSTRUCT(tp))->stadistinct;
	ReleaseSysCache(tp);
	if (nd < 0)
		nd = -nd * tuples;
	return nd > 0 && tuples > 0 ? Max(tuples / nd, 1.0) : 0;
}

/*
 * The keys of a dependent lookup that the loader probes in a btree of the table (map_keyed_dependent and map_probe in agg/lookup.c): an inner or semi join keyed by a
 * column of the table alone, or a child plan inner or semi joined on the same leaf of the scan, when a btree starts with the column and the probes cost less than its pages
 * (sift_agg_probe_pays). 0: the table is read whole. The keys are the rows PostgreSQL expects of the dependent lookup, taken as the geometric mean with the table's rows
 * when it has no statistics for them (join_key_zone).
 */
static double
join_probe_keys(AggCtx *c, SiftAggSpec *s, JoinCtx *jc, double *rows)
{
	RelOptInfo *jrel = c->root->simple_rel_array[jc->rti];
	const SiftAggCombo *cb = &s->combo[jc->combo];
	const SiftAggJoin *j = &jc->j;
	double		tuples = Max(jrel->tuples, 1.0),
				best = 0;

	*rows = 0;
	if (!sift_agg_lookup_pushdown || (j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || j->nidx > 0 || j->first.qual != NIL || j->first.nord > 0 || cb->level != 0)
		return 0;
	foreach_ptr(JoinCtx, dj, c->joins)
	{
		const SiftAggCombo *cc = &s->combo[dj->combo];
		const SiftAggJoin *d = &dj->j;
		int			att = 0,
					dkey;
		Oid			typ = InvalidOid;
		double		keys,
					rpk;
		bool		usable = false,
					unique = false;

		if (dj == jc || cc->level != 0 || (d->kind != SJ_INNER && d->kind != SJ_SEMI) || d->params != NIL || d->nkey != 1 || d->keynull[0])
			continue;
		if (dependent_join(c, s, jc, &dkey) == dj && d->keyexpr[0] != NULL && IsA(d->keyexpr[0], Param))
		{
			int			col = cc->refcol[((Param *) d->keyexpr[0])->paramid];

			if (col >= 0 && col < j->ncol)
			{
				att = j->colatt[col];
				typ = j->coltype[col];
			}
		}
		else if (dj->cpath != NULL && j->nkey == 1 && !j->keynull[0] && j->tkey[0] == NULL && j->keypos[0] >= 0 && d->keypos[0] >= 0 &&
				 cc->leaf[d->keypos[0]] == cb->leaf[j->keypos[0]] && cb->leaf[j->keypos[0]] >= 0)
		{
			att = j->keyatt[0];
			typ = j->keytype[0];
		}
		if (att <= 0 || (typ != INT4OID && typ != INT8OID))
			continue;
		foreach_node(IndexOptInfo, ii, jrel->indexlist)
			if (ii->relam == BTREE_AM_OID && ii->indpred == NIL && !ii->hypothetical && ii->indexkeys[0] == att)
			{
				usable = true;
				unique |= ii->unique && ii->nkeycolumns == 1;
			}
		if (!usable)
			continue;
		rpk = sift_agg_rows_per_key(planner_rt_fetch(jc->rti, c->root)->relid, att, tuples);
		if (rpk <= 0 && !unique)
			continue;
		rpk = Max(rpk, 1.0);
		keys = lookup_rows(c, dj);
		keys = Min(!lookup_counted(c, dj) ? sqrt(keys * tuples) : keys, tuples);
		if (sift_agg_probe_pays(keys, keys * rpk, jrel->pages) && (best == 0 || keys < best))
		{
			best = keys;
			*rows = keys * rpk;
		}
	}
	return best;
}

/* the rows the scan loads of a lookup table, and what each costs: a plan is run whole; an index scan reads the rows that pass the equalities or the keys of a dependent lookup, the rest of a table is read whole */
static double
join_load_rows(AggCtx *c, SiftAggSpec *s, JoinCtx *jc, double *row_cost)
{
	PlannerInfo *root = c->root;
	RelOptInfo *jrel = root->simple_rel_array[jc->rti];
	double		tuples = Max(jrel->tuples, 1.0),
				probes,
				prows;

	if (jc->cpath != NULL)
	{
		int			nnum = 0;

		foreach_ptr(Var, v, jc->cvars)
			nnum += v->vartype == NUMERICOID;
		*row_cost = jc->cvars == NIL ? AGG_JOIN_ROW : AGG_PLAN_ROW + nnum * AGG_LOAD_NUMERIC;
		return Max(jc->cpath->rows, 1.0);
	}
	if (jc->j.nidx > 0 && join_index_usable(jrel, &jc->j))
	{
		*row_cost = AGG_IDX_ROW;
		return Min(Max(jrel->rows, 1.0), tuples);
	}
	if ((probes = join_probe_keys(c, s, jc, &prows)) > 0)
	{
		/* the descents and the pages of the rows found, as sift_agg_probe_pays counts them: the price meets that of the whole table where the rule turns */
		*row_cost = AGG_IDX_ROW + (probes + Min(prows, jrel->pages)) * random_page_cost / (Max(prows, 1.0) * cpu_operator_cost);
		return Max(prows, 1.0);
	}
	/* the pages of a table read whole cost what PostgreSQL's own sequential scan of them costs (a 1.5-million-row table: 11,000 of 15,000 more), at most a random fetch each */
	{
		Relation	rel = relation_open(root->simple_rte_array[jc->rti]->relid, NoLock);
		int			nnum = 0;
		bool		cols = map_load_cols_possible(rel, &jc->j, &nnum);

		relation_close(rel, NoLock);
		/* the pages of the heap are not read by a load from the leaves, whose bytes are in the price of the row */
		*row_cost = cols ? AGG_LOAD_ROW_COLS * pow(Max(tuples / AGG_LOAD_BIG, 1.0), 0.4) + nnum * AGG_LOAD_NUMERIC : AGG_LOAD_ROW + jrel->pages * Min(seq_page_cost, random_page_cost) / (tuples * cpu_operator_cost);
	}
	return tuples;
}

/* the rows of the lookup tables a parallel scan has the leader write to the shared memory (agg/lookshare.c) */
double
agg_share_rows(AggCtx *c, SiftAggSpec *s)
{
	double		rows = 0;

	foreach_ptr(JoinCtx, jc, c->joins)
	{
		double		row_cost;

		if (jc->j.params == NIL)
			rows += join_load_rows(c, s, jc, &row_cost);
	}
	return rows;
}

/* The lookup joins: the children's plans and the lookup tables' reads */
static void
cost_joins(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	foreach_ptr(JoinCtx, jc, c->joins)
	{
		double		rows,
					row_cost;

		/* Correlated children run once per distinct input, with their native path's cost. */
		if (jc->j.params != NIL)
		{
			const SiftAggCombo *cb = &s->combo[jc->combo];
			SiftAggFirst *f = &s->join[foreach_current_index(jc)].first;
			double		run = AGG_CHILD_RUN * (jc->cpath->total_cost + cpu_operator_cost * AGG_EVAL_SLOT);
			double		native = combo_entries(c, s, cb, k->agg_rows, false) * run;

			/*
			 * the child that is a lookup of one row by unique key: the table's rows read once, if that costs less than the child for each input; the
			 * inputs are counted by what the index holds of the elements and of the tuples the scan reads, as the rows PostgreSQL expects of the join
			 * (a few for a filter it knows nothing of) are not what the child runs for
			 */
			f->use = f->ok && jc->frows * AGG_FIRST_ROW < combo_entries(c, s, cb, k->sel_rows * (s->group >= 0 ? k->elems : 1.0), true) * run;
			if (f->use)
			{
				k->load_rows += jc->frows;
				k->load_cost += jc->frows * AGG_FIRST_ROW;
			}
			else
				k->child_cost += native;
			continue;
		}
		rows = join_load_rows(c, s, jc, &row_cost);
		k->load_rows += rows;
		k->load_cost += rows * row_cost;
		if (jc->cpath != NULL)
			k->child_cost += jc->cpath->total_cost;
		if (jc->kind == SJ_SEMI || jc->kind == SJ_ANTI)
			k->scan_cost += k->sel_rows * AGG_SEMI_ROW;
	}
	k->scan_cost += k->load_cost;
}

/*
 * Does the set of a count(DISTINCT) of a leaf stay in the words of a group (agg/dist.c DD_WORDS)? The values of the leaf fall in a window of
 * dist_narrow_words words aligned to 64: from the range of its zones, or, for a text leaf, the codes of its dictionary (0 to nd - 1). A
 * leaf with no range known is not: the executor decides at its first batch, and the plan keeps the bitmap price.
 */
static bool
distinct_narrow(AggCtx *c, const SiftAggAgg *a)
{
	double		nd = c->stats.ndistinct[a->leaf],
				lo = c->stats.min[a->leaf],
				hi = c->stats.max[a->leaf],
				room = 64.0 * sift_limit(dist_narrow_words, DIST_NARROW_WORDS);

	if (lo != 0 || hi != 0)
		return floor(hi / 64.0) * 64.0 + 64.0 - floor(lo / 64.0) * 64.0 <= room;
	return nd > 0 && nd <= room && leaf_of(c, a->leaf)->vtype == SCV_CODE;
}

/*
 * The chunks of the bitmap sets of a count(DISTINCT) of a leaf: a group's values fall in chunks of AGG_DIST_CHUNK_BITS bits of the value
 * range (nd values wide), and a chunk is allocated and zeroed when a value first touches it. Only while the chunks fit the set's memory:
 * past it the sets move to a hash set, whose pair costs between 0.1 and 2 us with the column and the size of the set (not priced: 0).
 * A set of a narrow leaf has none.
 */
static double
distinct_chunks(AggCtx *c, SiftAggAgg *a, CostAcc *k)
{
	double		groups = Max(Min(k->rows_out, k->agg_rows), 1.0),
				nd = a->leaf >= 0 ? c->stats.ndistinct[a->leaf] : 0,
				chunks,
				touched;

	if (nd <= 0 || distinct_narrow(c, a))
		return 0;
	chunks = Max(ceil(nd / AGG_DIST_CHUNK_BITS), 1.0);
	/* the integer set of a group that holds few rows starts as a hash set (dist_wide_mode in agg/dist.c): no chunk is allocated */
	if (leaf_of(c, a->leaf)->vtype != SCV_CODE && k->agg_rows / groups < 16.0 * (2.0 + floor((c->stats.max[a->leaf] - c->stats.min[a->leaf]) / AGG_DIST_CHUNK_BITS)))
		return 0;
	touched = Min(groups * chunks * -expm1(-k->agg_rows / groups / chunks), k->agg_rows);
	return touched * AGG_DIST_CHUNK_BYTES > (double) get_hash_memory_limit() ? 0 : touched;
}

/* the distinct values of the text column a count(DISTINCT) reads by the statistics; 0 for any other argument or without them */
static double
distinct_text_values(AggCtx *c, const SiftAggAgg *a, const CostAcc *k)
{
	double		frac;

	if (a->kind != SAA_COUNT_DISTINCT || a->leaf < 0 || c->lay->leaf[a->leaf].vtype != SCV_CODE || c->lay->leaf[a->leaf].wide ||
		(frac = leaf_stat_frac(c, a->leaf, k->rows)) <= 0)
		return 0;
	return Max(frac * k->rows, 1.0);
}

/*
 * The ids the dictionary entries of a text column get for its count(DISTINCT): each unit hashes its entries into the table of the ids of the
 * query, which grows with the values the column has (a column of a few thousand values costs nothing; one of 700,000 costs 140 ns a row).
 */
static double
distinct_id_ops(AggCtx *c, const SiftAggSpec *s, const CostAcc *k)
{
	double		units = Max(k->units, 1.0),
				read = Max(k->zone_kept, 1.0 / units) * units,
				ops = 0;

	for (int ai = 0; ai < s->naggs; ai++)
	{
		double		nd = distinct_text_values(c, &s->agg[ai], k),
					entries,
					x;
		bool		counted = false;

		/* a leaf the keys or an earlier aggregate gave ids to has them */
		for (int j = 0; j < ai; j++)
			counted |= s->agg[j].kind == SAA_COUNT_DISTINCT && s->agg[j].leaf == s->agg[ai].leaf;
		for (int j = 0; j < s->nkeys; j++)
			counted |= (s->key[j].kind == SAK_LEAF || s->key[j].kind == SAK_LUT) && s->key[j].leaf == s->agg[ai].leaf;
		if (nd <= 0 || counted)
			continue;
		entries = nd * -expm1(-k->rows / units / nd) * read;
		x = Min(entries, nd) / 100000.0;
		ops += entries * AGG_DIST_ID * pow(x, x <= 1.0 ? 0.9 : 0.6);
	}
	return ops;
}

/* The aggregates, combos, keys and predicates of every tuple, less what the plain path saves, plus the bytes read */
static void
cost_work(AggCtx *c, SiftAggSpec *s, CostAcc *k)
{
	double		chunks = 0,
				entry_cost;

	for (int i = 0; i < s->naggs; i++)
	{
		if (s->agg[i].kind == SAA_COUNT_DISTINCT)
			chunks += distinct_chunks(c, &s->agg[i], k);
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			k->scan_cost += k->agg_rows * AGG_ORDERED_ROW;
		k->scan_cost += k->agg_rows * (num_cost(&s->agg[i].num) + direct_agg_cost(&s->agg[i])) + k->rows * pred_num_cost(s->agg[i].filter);
		if (s->agg[i].kind == SAA_GEN)
			k->gen_cost += k->agg_rows * s->agg[i].gen->cost + k->rows_out * (s->agg[i].gen->ordered || s->agg[i].gen->sorted || s->agg[i].gen->hyp ? AGG_GEN_GROUP : AGG_GEN_STATE);
	}
	for (int a = 0; a < s->narms; a++)
		k->scan_cost += k->agg_rows * (s->arm[a].hmask ? AGG_ARM_HIDDEN : AGG_ARM_TUPLE);
	for (int ci = 0; ci < s->ncombo; ci++)
		if (s->combo[ci].join >= 0 && ((JoinCtx *) list_nth(c->joins, s->combo[ci].join))->j.params == NIL)
		{
			JoinCtx    *jc = list_nth(c->joins, s->combo[ci].join);
			double		past = c->root->simple_rte_array[jc->rti]->rtekind == RTE_RELATION ? c->root->simple_rel_array[jc->rti]->rows - AGG_FAN_CACHE : 0.0;

			k->scan_cost += Max(k->agg_rows - k->sel_rows * k->elems, 0.0) * (AGG_FAN_ROW + s->combo[ci].nslots * AGG_FAN_SLOT) * Min(sqrt(Max(past, 0.0) / AGG_FAN_CACHE), AGG_FAN_CAP);
		}
	k->scan_cost += combo_eval_cost(c, s, k->rows, k->agg_rows, k->scan_rows, k->rows * Max(k->zone_kept, 1.0 / k->units), k->sel_rows * k->elems * k->key_share, &entry_cost) + bucket_eval_cost(c, s, k->agg_rows) + distinct_id_ops(c, s, k);
	/* each process evaluates the entries it meets: the workers divide the probes, not those (cost_parallel divides the cold operators with the rest) */
	k->cold_ops += entry_cost * (parallel_divisor(s->nworkers) - 1.0);
	for (int i = 0; i < s->nkeys; i++)
		k->scan_cost += k->agg_rows * num_cost(&s->key[i].num);
	for (int i = 0; i < s->npred; i++)
		k->scan_cost += k->rows * pred_num_cost(s->pred[i]);
	k->scan_cost = Max(k->scan_cost - plain_credit(c, s, k->agg_rows), k->scan_cost * 0.1);
	/* after the credit, which prices what the normal plan under-estimates: the rows read again from the heap are work it does not do */
	k->scan_cost += recheck_cost(c, s, k) * Max(k->zone_kept, 1.0 / k->units);
	k->scan_cost += chunks * AGG_DIST_CHUNK;
	k->scan_cost += scan_bytes(c, s, k->rows) * AGG_BYTE * Max(k->zone_kept, 1.0 / k->units);
}

/*
 * Each process aggregates its share into a table of its own; the leader merges them. The lookup tables are
 * read once, by the leader, who writes them to the shared memory (agg/lookshare.c): the divisor is not theirs.
 */
static void
cost_parallel(AggCtx *c, SiftAggSpec *s, CostAcc *k, Cost *startup)
{
	double		tables = s->nworkers + 1;
	double		load = Min(k->load_cost * cpu_operator_cost, k->scan_cost);
	double		setup = parallel_setup_cost * AGG_SETUP_FACTOR;
	bool		priced = parallel_setup_cost > 0;	/* 0 asks for the workers, whatever they save */

	/*
	 * Starting the workers takes 3 ms and 0.35 ms for each, which a low parallel_setup_cost prices below the plans of a few milliseconds; the
	 * leader merges the pairs of a count(DISTINCT) into its own sets one by one, so they do not scale with the workers (below)
	 */
	if (priced)
		setup = Max(setup, (3.0 + 0.35 * s->nworkers) * AGG_START_MS);
	k->scan_cost = (k->scan_cost - load + (priced ? k->cold_ops * AGG_SCAN_SPEED * cpu_operator_cost : 0)) / parallel_divisor(s->nworkers) + load + k->load_rows * AGG_SHARE_ROW * AGG_SCAN_SPEED * cpu_operator_cost +
		tables * Min(k->rows_out, Max(k->agg_rows / tables, 1.0)) * (AGG_MERGE_GROUP + (priced ? s->nkeys * AGG_MERGE_KEY : 0.0)) * AGG_SCAN_SPEED * cpu_operator_cost;
	for (int i = 0; i < s->naggs; i++)
	{
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			k->scan_cost += k->agg_rows * AGG_ORDERED_MERGE * AGG_SCAN_SPEED * cpu_operator_cost;
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
}

/* the per-group arrays of one aggregate, as acc_alloc (agg/group.c) allocates them */
static double
acc_bytes(const SiftAggAgg *a)
{
	double		b = sizeof(int64);	/* n */

	switch (a->kind)
	{
		case SAA_SUM_INT:
		case SAA_AVG_INT:
		case SAA_PCT_CONT:
		case SAA_PCT_DISC:
		case SAA_O_SUM_I64:
			return b + sizeof(int64);
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
			return b + sizeof(sift_i128) + 1;
		case SAA_MIN:
		case SAA_MAX:
			return b + sizeof(int64) + sizeof(double) + (a->num.n ? sizeof(sift_i128) : 0);
		case SAA_SUM_F64:
		case SAA_AVG_F64:
		case SAA_O_SUM_F64:
		case SAA_O_AVG_F64:
			return b + sizeof(double) + 1;
		case SAA_M_PG:
			return b + 1 + sizeof(Datum);
		case SAA_X_SUM:
		case SAA_X_AVG:
			return b + (a->restype == NUMERICOID ? sizeof(int64) + sizeof(sift_i128) : 0) + sizeof(double) + 1 + sizeof(Datum);
		case SAA_M_SUM:
		case SAA_M_AVG:
			return b + sizeof(double) + 1 + sizeof(Datum);
		case SAA_M_MIN:
		case SAA_M_MAX:
		case SAA_X_MIN:
		case SAA_X_MAX:
			return b + sizeof(Datum);
		case SAA_O_COUNT_ROWS:
		case SAA_COUNT_ROWS:
			return b + sizeof(uint64);
		case SAA_GEN:
			return b + 2 * sizeof(Datum) + 1 + (a->gen->ordered || a->gen->sorted || a->gen->distinct ? sizeof(int64) : 0);
		default:
			return b;
	}
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
		b += acc_bytes(&s->agg[i]);
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
static double
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

/*
 * The heap check of the rows on pages that are not all-visible (vis is their share), a page at a time: the pages of the units the zones keep
 * (zone_kept) that hold a selected row, which the rows reach by chance (Poisson). A page is in shared buffers, in the cache the operating system keeps
 * (PostgreSQL's own assumption for it is effective_cache_size), or read from the disk: a sequential read when the scan reaches most of the pages it
 * passes and a random one when it skips them.
 */
static double
vet_cost(AggCtx *c, double vis, double sel_rows, double zone_kept)
{
	double		pages = Max(rel_pages(c), 1.0),
				vet_pages = vis * Min(Max(pages * zone_kept, 1.0), pages),
				rows = vis * sel_rows,
				buffered = Min(NBuffers / pages, 1.0),
				cached = Max(Min(effective_cache_size / pages, 1.0), buffered),
				touched,
				seq,
				disk,
				op = AGG_SCAN_SPEED * cpu_operator_cost;

	if (vet_pages <= 0)
		return 0;
	touched = vet_pages * (1.0 - exp(-rows / vet_pages));
	/* the pages are read in order and skip the all-visible ones: never dearer than the random fetch of the same pages, where a configuration prices a sequential page above a random one */
	seq = Min(seq_page_cost, random_page_cost);
	disk = seq + (random_page_cost - seq) * (1.0 - touched / vet_pages);
	return touched * (buffered * AGG_VET_PAGE * op + (cached - buffered) * AGG_VET_OS_PAGE * op + (1.0 - cached) * disk) + rows * AGG_VET_ROW * op;
}

/* The tables of the IN lists the scan builds when it starts: they depend on the range of the values, not on the rows read or the units kept */
static double
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

/* What every path of the scan pays to start, whatever it reads: its source and the partitions it opens. No path costs less in total. */
Cost
agg_open_cost(int nparts)
{
	return (AGG_START_COST + AGG_PART_PAIR * nparts * nparts) * cpu_operator_cost;
}

/*
 * the most rows of elements a scan of the plan being made reads: what a plan that keeps the shared CTE has to produce (cte.c). A filter without
 * statistics keeps a default guess of the table's rows: the rows are the geometric mean of the guess and the table's, as filter_rows_ratio has them.
 */
/* the tuples the cap of cost_tuples removes, as a share of those the scan reads, above which workers are judged on the tuples that are read */
#define AGG_WIDE_CAP		0.5

/*
 * PostgreSQL's count of a join of lookups that it has no statistics for (a CTE or sub-select of ids is estimated through its own defaults: 100 rows where
 * 1,000,000 pass) caps the tuples that reach the groups (cost_tuples), and the scan is priced at milliseconds when it takes hundreds, so the workers it
 * would need (3 to 4 ms to start) never pay. The price of the scan stays the capped one, which is what the plan above and the normal plan are priced
 * with; the path with workers is priced at the share of it that the workers take of the scan when the tuples are the rows the scan reads: the
 * same scan, serial and with workers, priced without the cap, and the capped serial price times their ratio.
 */
static void
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
	total = sc.total_cost * pw.total_cost / sw.total_cost;
	if (total < p->total_cost)
	{
		p->total_cost = total;
		p->startup_cost = Max(total - out_cost, 0.0);
		s->wide_unit_rows = Max(c->stats.nrows / Max(c->stats.nunits, 1.0), 1.0);
	}
}

double		agg_read_rows;

void
agg_cost(AggCtx *c, SiftAggSpec *s, RelOptInfo *input_rel, Path *p, double rows_out)
{
	CostAcc		k = {0};
	double		vis = 1.0 - Min(Max(c->stats.allvisible_frac, 0.0), 1.0),
				exact,
				vet,
				spill,
				table,
				groups,
				share,
				entries,
				key_ops,
				ids;
	Cost		startup;

	k.rows =Max(c->stats.nrows, Max(c->rel->tuples, 1.0));
	k.elems = s->group >= 0 ? elems_of(c, s->group) : 1.0;
	k.sel_rows = Min(Max(c->rel->rows, 1.0), k.rows);
	if (!pricing_scratch)
		agg_read_rows = Max(agg_read_rows, k.sel_rows * filter_rows_ratio(c) * k.elems);
	k.units = Max(c->stats.nunits, ceil(k.rows / 2000.0));
	k.rows_out = rows_out;
	k.open_cost = c->search ? sift_search_open_cost(c->search, k.rows) : 0;
	k.scan_cost = c->search ? sift_search_cost(c->search, k.rows, k.units, &k.rows, &k.units) : 0;
	cost_read(c, s, &k);
	cost_tuples(c, s, input_rel, &k);
	cost_joins(c, s, &k);
	cost_work(c, s, &k);
	entries = key_entries(c, s, &k, &ids, &key_ops);
	k.scan_cost += key_ops;
	/* the reads of the lookup tables are PostgreSQL's own, priced as its plans are */
	k.scan_cost = (k.scan_cost - k.load_cost) * AGG_SCAN_SPEED + k.load_cost;
	/* a top-N reads a fraction of the units, but opens the match set whole */
	if (s->topn)
	{
		double		scan = (k.scan_cost - k.open_cost) * c->topn_frac + k.open_cost;

		/* through the sorted runs, a unit costs a seek and the rows wanted, never more than its rows scanned */
		k.scan_cost = c->topn_runs ? Min(scan, k.open_cost + Max(c->stats.nunits, 1.0) * c->topn_frac * (TOPN_RUN_UNIT + rows_out * TOPN_RUN_ROW)) : scan;
	}
	k.scan_cost *= cpu_operator_cost;
	/*
	 * The normal plan of an unnest expects the rows of the function (100 per call for jsonb_to_recordset, jsonb_array_elements,
	 * jsonb_each) minus its filters, and prices its sort, join and aggregate with them, though the data has a handful per row: when it
	 * expects more unnested rows than the selected orders hold elements, this path is charged the square root of the excess. The
	 * estimates of the two plans then cross where the times do: a selective store set keeps the btree plan, a whole table or a
	 * filter on the elements (which cuts the expected rows) keeps its price. A qual with a bound parameter has no such estimate (a generic
	 * plan guesses its selectivity; the values are known at execution): no charge.
	 */
	k.lift = s->group >= 0 && !rel_has_param_qual(c->rel) ? Max(sqrt(input_rel->rows / (k.sel_rows * k.elems)), 1.0) : 1.0;
	k.scan_cost *= k.lift;
	/* the calls of the generic aggregates are the elements the data has, whatever the normal plan expects of the unnest */
	k.scan_cost += k.gen_cost * cpu_operator_cost;
	/* rows on pages that are not all-visible are checked in the heap: the rows that reach the groups, not the rows the table's own filters keep (the lookup joins filter them first) */
	vet = vet_cost(c, vis, Min(k.sel_rows, k.scan_rows / k.elems), Min(Max(k.zone_kept, 1.0 / k.units), 1.0)) * (s->topn ? c->topn_frac : 1.0);
	k.scan_cost += vet;
	/* the exact units cost their groups, not their rows */
	exact = preagg_share(c, s);
	if (exact > 0)
	{
		double		pre_cost = k.units * Max(k.zone_kept, 1.0 / k.units) * (AGG_UNIT_COST + preagg_unit_ops(c, s, k.units, k.rows, k.rows_out)) * AGG_SCAN_SPEED * cpu_operator_cost;

		k.scan_cost += exact * (Min(pre_cost, k.scan_cost) - k.scan_cost);
	}
	startup = (c->level_params != NIL ? AGG_RESTART_COST * cpu_operator_cost : agg_open_cost(c->nparts)) + k.child_cost + c->subplan_cost;
	/* a search is opened first (k.open_cost) and its native plan is the index scan of the search, which the scan starts no cheaper than */
	if (s->nkeys == 0 && s->naggs > 0 && !s->topn && c->nparts == 0 && c->level_params == NIL && !c->search)
		startup -= (AGG_START_COST - AGG_START_PLAIN) * cpu_operator_cost;
	for (int i = 0; i < s->npred; i++)
		startup += in_tables_cost(c, s->pred[i]);
	if (s->nworkers > 0)
		cost_parallel(c, s, &k, &startup);
	/* each process holds a table of its own (and spills it) */
	groups = group_count(c, s);
	/* a top-N reads a fraction of the units, and makes the ids of those */
	/* a serial scan that finishes the groups of a partition when its units end (part_flush_on) holds the table of one partition at a time */
	share = s->part_final && s->nparts > 0 && s->nworkers == 0 ? s->nparts : 1;
	spill = group_spill_cost(s, groups / share, Min(ids, entries / (s->nworkers + 1)) * (s->topn ? c->topn_frac : 1.0) / share, k.agg_rows / (s->nworkers + 1), &table);
	k.scan_cost += spill;
	if (!pricing_scratch)
	{
		agg_arm_spill(spill, groups, table);
		agg_arm_vet(vet, vis * 100.0);
	}
	/* no unit is read where the order array gives the rows (sift_topn_gorder): the heap confirms the n of them */
	if (s->topn && c->topn_array && s->nworkers == 0)
	{
		startup = (AGG_GORD_START + AGG_GORD_SEG * ceil(Max(c->rel->tuples, 1.0) / AGG_GORD_SEG_ROWS)) * cpu_operator_cost;
		k.scan_cost = 0;
	}
	p->startup_cost = startup + k.scan_cost;
	p->total_cost = p->startup_cost + k.rows_out * cpu_tuple_cost;
	/* the rows of a top-N are fetched from the heap, one page each */
	if (s->topn)
		p->total_cost += c->topn_fetch * random_page_cost * 0.5;
	/* the groups of the rows the lists pass (spec_finish) tell the plan above how many rows to expect; they are not part of the path's own price */
	p->rows = s->topn ? k.rows_out : Max(k.rows_out, c->path_rows);
	/*
	 * The rows of a fused scan add the groups of every arm, though no arm has more groups than rows it reads: the node reports at most the rows
	 * its arms aggregate. The plan above is priced for them (agg/cte.c weighs the inline plan of a shared CTE against the shared one), and the
	 * sum of the arms' groups made the inline plan look 6 times dearer than it is.
	 */
	if (s->narms > 0 && !s->topn)
		p->rows = Min(p->rows, Max(s->est_rows, 1.0));
	if (s->nworkers > 0 && !pricing_scratch && parallel_setup_cost > 0 && c->joins != NIL && k.agg_rows * AGG_WIDE_CAP < k.sel_rows * k.elems)
		workers_ratio(c, s, input_rel, p, rows_out, k.rows_out * cpu_tuple_cost);
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
