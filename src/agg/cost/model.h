/*
 * agg/cost/model.h - the constants of the cost model of the aggregate scan, in one place (agg_cost in agg/cost/model.c prices a scan with them).
 *
 * Unless the comment of a constant says otherwise, a value is in operators: units of cpu_operator_cost, about AGG_OP_NS nanoseconds each, which
 * AGG_SCAN_SPEED scales to what the scan really spends. A comment that gives measured figures keeps them with the constant they calibrate.
 */
#ifndef SIFT_AGG_COST_MODEL_H
#define SIFT_AGG_COST_MODEL_H

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
#define AGG_JOIN_LUT		5.0		/* ... of a table answered from a byte table of its keys (semi_lut_build, in the cache of a core): 40 instructions a tuple measured on 985,000 tuples of a 550,000-key table */
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
#define AGG_ARM_TUPLE		36.0	/* a tuple through an arm of a two level query: its cell started or added to, and flushed into the outer groups when the row ends (35 ns measured; 150 priced the scan at 334 units a million instructions against the 90 of the others and the planner kept a plan 10 times slower, PLANNING.md B2) */
#define AGG_ARM_HIDDEN		80.0	/* ... in an arm with hidden keys: the cells of the row are numbered in a table of their own (70 ns) */
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

#define AGG_LIFT_GROUPS		1024.0	/* the groups the lift of a lookup's rows (spec_finish) adds to the plan's own: a plan above hashes or materializes a node of a thousand rows more than it expects, and a join of two lifted nodes (their rows multiplied by a default join selectivity) priced the scans above the plan that keeps one of them normal */
#define AGG_GEN_ROW			1.0	/* a transition function call through fmgr (the function's own procost and the leaf's Datum, 2 operators, come on top): 32 to 88 ns a row measured for bool_and, min(text), variance, array_agg, string_agg and corr over 1,000,000 rows, about 3 to 7 operators by the cost of the normal plan's own rows (it was 4: a scan 10 to 50% faster than PostgreSQL's plan lost the choice) */
#define AGG_GEN_DISTINCT	10.0	/* the (group, value) pair of a DISTINCT aggregate: a hash probe */
#define AGG_GEN_SORTED		30.0	/* a row of an aggregate with ORDER BY: the tuplesort put, and its share of the sort and the read back */
#define AGG_GEN_FIRST		1.0		/* a row of a DISTINCT ON column: the compare of its sort keys with the best row of the group, the value read when it wins */
#define AGG_GEN_FIRST_KEY	1.2		/* ... and each sort key's compare */
#define AGG_GEN_FOLLOW		0.3		/* a row of the other DISTINCT ON columns: the answer of the first one */
#define AGG_GEN_HYP			200.0	/* a row of a hypothetical-set aggregate: sorted by the scan for its groups, then again by the aggregate (no gain over the normal plan) */
#define AGG_GEN_ORDERED		24.0	/* a value of an ordered-set aggregate: the tuplesort put */
#define AGG_GEN_GROUP		150.0	/* the sort of an ordered-set or sorted group */
#define AGG_GEN_STATE		20.0	/* a group's state and final function of any other generic aggregate: 40 to 200 ns a group measured for min(text), bool_and, max(timestamptz), variance and array_agg over 100,000 to 620,000 groups (it was priced with the sort, 1.8 us) */
#define AGG_GROUP_KEY_BYTES	40.0	/* a group key's share of the table: its id maps and the ids of the group (agg/group.c), without the value's own bytes */
#define AGG_GROUP_SLACK		1.3		/* the group arrays grow by doubling: on average a third is room, measured 185 bytes a group for 5 aggregates and one key */
#define AGG_PREAGG_EXACT	0.98	/* the share of exact units from which the metadata answers the query: no workers, they cost more than the answer */
#define AGG_JOIN_ROW		4.0		/* a row of a joined table, read when the scan starts */
#define AGG_PLAN_ROW		11.0	/* a row of a plan (a UNION ALL of two tables of 500,000 rows) made the lookup table: the tuple copied into the table and its key hashed, 380 instructions a row measured with a count(*) over it; each numeric column of it adds AGG_LOAD_NUMERIC */
#define TOPN_HEADS_UNITS	16.0	/* the units a top-N walk may read for the heads of their runs to beat the order array (topn_heads_win) */
#define AGG_ORDER_MIN_COST	1000.0	/* the cost of the normal join under which the other tables of an inner join are not tried as the scanned one: the order saves less than the 60 to 90 us the extra paths take to plan (about 2 ms of the normal plan) */
#define AGG_LOAD_ROW		20.0	/* ... of a table read whole from its heap, not kept from an earlier query: the visibility test, the key hashed and the row stored (66 ns a row measured on a 500,000-row read, 100 ns with the first touches), beyond the pages */
#define AGG_LOAD_KEY		10.0	/* ... of a table kept as the keys of its rows (lookup_keys_only: no dictionary of the keys, no chains): 453 instructions and 29 ns a row measured on a 550,000-row read, the pages apart */
#define AGG_LOAD_ROW_COLS	8.0	/* ... of a table read whole from the leaves of a column index that holds its columns (map_load_cols): the units are decoded a unit at a time and a tuple of the columns is made for the rows that are visible (65 ns a row against 160 ns from the heap, measured); AGG_LOAD_BIG scales it past the rows the cache holds */
#define AGG_LOAD_BIG		250000.0	/* the rows of a table whose map stays in the cache: past them a load from the leaves costs the 0.4 power of the rows more (61 ns a row for 1,500,000 rows of 100,000 keys, 74 to 88 ns for 6,000,000 rows of 10,000 to 200,000 keys, 127 ns for 1,500,000 keys: the key dictionary outgrows the cache) */
#define AGG_LOAD_NUMERIC	32.0	/* ... more for each numeric column of such a load: the value is built and allocated for each row (260 ns a row and column measured on 2,750,000 rows of a fact table, against 15 ns for an integer one) */
#define AGG_FIRST_ROW		0.1		/* a row of a table read as the first-row lookup of a child (agg/first.c): 0.3 us to load and hash (33 ms for 108,000 rows), a model unit being 2.9 us */
#define AGG_FUSED_ARM		0.1		/* the share of its price an arm pays that runs in the pass of another one (agg/fuse.c) */
#define AGG_SHARE_RATIO		4.0		/* rows of the scan for each row of the lookup tables from which workers pay: the leader writes the tables to the shared memory and each worker maps them (4 workers, measured at 1, 2, 5 and 12) */
#define AGG_IDX_ROW			12.0	/* a row of the joined table read through an index: the btree descent and the heap fetch */

/* a tuple of a bucket key: the number of its step, the lookup of the id */
#define AGG_BUCKET_ROW		3.0

#endif							/* SIFT_AGG_COST_MODEL_H */
