# Aggregate pushdown

## Joins to plans

The right side of a lookup join of `Custom Scan (SiftAgg)` is a table, or a plan: a materialized CTE, a `VALUES` list, a function (`generate_series`) or a sub-select that
PostgreSQL keeps (`GROUP BY`, `LIMIT`, `DISTINCT`, a set operation, a ranked scan with `LIMIT`) in an inner, `LEFT`, `IN` or `NOT EXISTS` join.
PostgreSQL's plan of the right side is the child of the node (`EXPLAIN` shows it under `Lookup Joins: s (subquery)`); the scan runs it once when it
starts, with the query's snapshot, and its rows are the lookup table, so duplicate keys multiply the rows, a NULL key matches nothing, and the result is the
one of the normal plan (`EXPLAIN ANALYZE` has `Lookup Rows Read`). A rescan replays the groups; a prepared statement's parameters in the right side are read
by the child plan as in any plan.

A `UNION ALL` or `UNION` of sub-selects, a sub-select of a statement-level CTE and a nested `WITH` inside the sub-select are plans in the same way.
The same holds for a `WITH` of a derived table (a ranked top-N over an aggregate): its CTEs are lookup plans while the derived table reads nothing of the queries around it, and a CTE of it that several aggregates read is planned inline like the statement's own (the cheaper plan wins). A lateral sub-select that reads the row around it keeps the normal plan. Test `agg_nested_ctes`, fixtures `nf_nested_cte_*`, row J32.

The normal plan stays when the right side reads a volatile function, the join has no condition (`ON true`) and is not lateral, the right side is
estimated above `sift.agg_lookup_max_kb` of memory (rows times their width; a lateral one: its rows for each distinct input of the scanned table, together), or its plan (an `Append`) outputs only the relation's columns and a
condition reads another. A scan with such a join runs without workers.

A lateral right side that reads the joined row (`LEFT JOIN LATERAL (SELECT .. LIMIT 1)`, a `UNION` or `UNION ALL` of arms, `generate_series`, `unnest` of
an array column, a set-returning function) keeps PostgreSQL's parameterized plan of it as the child: the scan runs the child for each distinct value of the
columns it reads (the lookup keys of the table, or another joined table's columns) and multiplies the rows by what it returns, duplicates and
NULL-extended rows included. A call of a document column that cannot raise (`jsonb_exists(doc, 'k')` or `doc ? 'k'`, `doc ->> 'k'`, `jsonb_typeof(doc)`) takes the
call's value as the parameter, when the index holds it as an expression column spelled like the query's: `jsonb_exists(doc, 'k')` gives the key presence of any JSON
root (a path leaf cannot tell a missing key from a JSON null). The scan runs the child for the values of the tuples that reach its join, in its own order and
PostgreSQL's plan in another, so a child that can raise (a division, a cast of text, a sub-select) is joined only when the query joins nothing else (the
conditions of the scanned table come before it in both plans), else the normal plan stays; so it does for a child that reads the whole document or a column of a
query above the aggregate's own (a sub-select of an outer query: its value changes per row of that query).
The value of a `jsonb_each` member (`r.value`) is given to the child as the jsonb its stored float stands for (the member's number, or a JSON null when
the member holds none), so the child may cast it to an integer or float (`CAST(r.value AS integer)`): the cast runs only for the members of the rows that reach the child, and
raises PostgreSQL's own error (`integer out of range`). A document the index cannot hold as numbers (a string or object member) keeps the normal plan.
A path declared `float8` holds floats, which carry no scale: a child that reads the member any other way (a cast to `numeric` or `boolean`, `r.value` itself, as text) would see `12.5` where the document has `12.50`, so it keeps the normal plan (test `agg_child_member_value`).
A lateral sub-select that PostgreSQL does not pull up because its query is read on the nullable side of an outer join (`dates LEFT JOIN cte`; below a `RIGHT` or `FULL` join the normal plan stays) is pulled up
inside its own query level by the planner hook (`agg/flat.c`), which plans the query both ways and keeps the flat plan when it runs in the scan and costs
less; a sub-select with a join of its own is attached through its leftmost table, as `(a JOIN jsonb_each(a.doc) r) LEFT JOIN m ON ..`.
`LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'` with a constant key is the member `k` of each row (an object has one member per key; a row without it stays, with NULL columns): the scan reads it at the
row level, as `(doc ->> 'k')::float8`, and no element is unnested. A correlated child reads `r.value` only through casts of jsonb to an integer or float (a missing member and a JSON null are the same NULL, and a
JSON null casts to NULL in PostgreSQL 18); before PostgreSQL 18, with another condition, or with `r.key` read, the normal plan stays. Other outer joins of an unnested JSON function keep the normal plan too.
Tests `agg_correlated_children`, `agg_correlated_fanout`, `agg_derived_correlation`, `agg_append_children`, `agg_raw_value_children`, `agg_lateral_flatten`, `agg_left_each` and `agg_left_unnest`.
Predicate-only joins, including inequalities and range containment, test every candidate row; overlapping and duplicate matches multiply the inputs.
Lookup keys, grouping expressions and predicates may also read earlier lookup outputs; duplicates and NULL-extended rows remain distinct inputs.
Aggregate `FILTER` predicates can read lookup columns and source values together. They run for each surviving joined tuple, after all joins;
an excluded tuple does not evaluate a filter expression that could raise an error.
Inner lookups are ordered by their dependencies within each join boundary. Statement-level materialized CTEs can be referenced from nested queries.
A condition over the columns of such a sub-select that PostgreSQL pushed down into it (the run condition of a ranking window, `rn <= 3`) is enforced by its plan, which then does not output the column the condition read: the scan does not test it again (a column the query reads keeps its place, and the condition is tested as usual). Test `agg_pushed_conditions`, fixtures `nf_pushed_*`, row J33.
Fixtures `pt_*`, tests `agg_plan_tables`, `agg_child_predicates`, `agg_lookup_dependencies` and `agg_statement_cte_children`.

## Join key filters and shared lookups

An inner or semi lookup join whose key is one integer (`int8` too) or date leaf of the index (`JOIN stores s ON s.id = o.store_id`, `IN (SELECT ..)`, `EXISTS`) needs a row of the table with that
key, so the keys of the table, loaded when the scan starts with its own conditions already applied, are a conjunct of the scan: `leaf IN (keys)`. The conjunct is a byte table over the
range of the keys (a hash table of the keys when they span more than 4 million values), the same test, zone pruning and source filters as an `IN` list of constants: a unit whose zone misses the
range is not read (`Units Pruned`) and the other rows are tested as a set before a tuple is made, where the join would resolve every tuple through the combo cache. A join on several
key columns adds one conjunct per integer key. A join keyed by a column of that table (`JOIN ids ON ids.id = d.id` after `JOIN d ON d.id = f.d_id`, an inner or semi join with no condition on the scan's own columns) drops the rows of `d` it has no match for, so only the keys of the others are in the conjunct; no key at all (an empty selection) reads no unit. `EXPLAIN ANALYZE` shows `Join Key Filters`; `sift.agg_join_filter = off` gives the plain join back. When the key is a column the partitions of a partitioned table are split by, the keys also drop the partitions none of them can be in (`Partitions Removed`): a range or list bound by the range of the keys and, with few keys, by each of them; a hash level by the remainders of the keys; no key reads no partition. The rows are those of the normal plan, and a tuple the join drops never reaches an expression (as when the normal plan joins first; a conjunct on the fact rows that may raise
is evaluated on the rows whose key the table holds, which is what a plan that joins before it filters does). `LEFT`, `RIGHT`, `FULL` and anti joins, text keys,
element-level leaves and a top-N scan have no such conjunct; a plan with a preserved side has the conjuncts of its inner and semi joins (a tuple that fails one never matches a preserved row, and the preserved rows nothing matched are still the NULL rows of the groups). When the join returns no column of the table and the key filter holds the keys (and an inner join's keys are unique), the tuples the filter let through are not looked up again: `Join Key Passes`. Keys spread over more than 4 million values also skip the units whose zone holds none of them. Two sets on one leaf (an `IN` list or a dimension's keys, and the keys of a join) are met before the zones are tested: a unit is read when its zone holds a value of both (test `agg_zone_sets`). The cost of the node keeps the units the zones of the keys leave (the mean zone width of the leaf is stored with the index; keys count as points). Tests `agg_join_filter` and `agg_join_key_types`, isolation spec `agg_join_filter`, fixtures `j_join_filter_*`, row J28.

A combo of one input (a join key, a grouping expression of one leaf) finds the entry of an id below 65,536 by array index; a larger or negative id, and combos of several inputs, use the hash table.

`UNION ALL` arms (or a CTE read several times) of one statement that join the same table with the same key and output columns and the same conditions on it read the same rows in the
same snapshot: the first node loads them into a context of the statement and the others use that table (`sift.agg_share_lookups`, `Shared Lookup Tables`). Arms that read other columns
or conditions of the table, a table that is a plan, a preserved side and a parallel worker load their own. Test `agg_share_lookups`, isolation spec
`agg_share_lookups`, fixture `j_share_arms`, row J29.

A backend also keeps the rows of such a table for its next statement (`sift.agg_lookup_cache_kb`, `Cached Lookup Tables`), the load of a mapping table of half a million rows being the fixed
cost of every query that joins it. The rows are kept under the table's conditions while every page of the heap is visible to all transactions (vacuumed) and has the same LSN as when the
rows were loaded, so a write that is in flight, committed or rolled back, a vacuum that rewrites a page and a rewrite of the table all load the rows again; a table with a page that is not
yet visible to all (rows not vacuumed since a write), a condition that is not constant, a preserved side, a table of a plan or another access method and a parallel worker are never kept.
Test `agg_lookup_cache`, isolation spec `agg_lookup_cache`.

The rows can also be kept for the other backends (`sift.agg_lookup_shared_kb`, on by default: -1 is the smaller of `sift.agg_lookup_cache_kb` and 5% of `shared_buffers`, 0 turns it off; superuser): a backend that loads a table writes its rows in the flat form of a parallel scan into a
dynamic shared memory segment and lists it in a directory of 64 entries that every backend reaches by name (the DSM registry: no restart, no preload). A backend that finds the entry of the
key with the pages it sees now (the same LSNs, all visible, as above) attaches the segment and reads the rows in place, so a connection that lives for one query pays the attach, not the load.
The segment holds the key and the pages and they are compared in full, the directory only has their hashes; the rows are per role, as in the cache of a backend. A write moves the LSNs, and the
entry of the old state goes when the key is kept again. The entries together take up to the setting (the least recently used go first; a table of more than half of it is not kept) and a segment
dropped from the directory stays until the backends that read it are done. A crash restart discards the registry and the segments. A backend that loads while others load the same table
(a cold start) loads for itself, each one. Test `agg_lookup_shared`, isolation spec `agg_lookup_shared`, TAP test `018_lookup_shared`.

The right side of a semi or anti join can be a tree of joined tables (`IN (SELECT .. FROM a JOIN b ..)`, `EXISTS (SELECT 1 FROM a JOIN b ON .. WHERE a.k = o.k)`, or the `coalesce` of a LEFT JOIN
that PostgreSQL makes of `p.id IN (SELECT k FROM (SELECT coalesce(a.k, b.k) AS k FROM a LEFT JOIN b ..) t)`). The tree is the join PostgreSQL planned for those tables; the scan runs its cheapest
serial plan once when it starts, as a sub-select's, and looks the keys up. The condition on the scan's side reads one table of the tree (the key); the conditions of the ON clause on the tree's own
columns are already in its plan. A tree that reads the query's other tables, has a parameterized or parallel plan only, or has more keys than `sift.agg_lookup_max_kb` allows, and a key that is an
expression of two tables of the tree straight under the scan (`o.k IN (SELECT coalesce(a.k, b.k) FROM a LEFT JOIN b ..)`; `from_shape`), keep the normal plan. Test `agg_semi_tree`, fixtures `j_semi_tree*`.

## Outer joins that keep the other table

A `RIGHT JOIN` or `FULL JOIN` to a table, and a `LEFT JOIN` whose right side is the indexed table, aggregate over the table that keeps its rows (`EXPLAIN`
shows it as `Lookup Joins: stores (preserved)` or `(full)`). The scan reads the table when it starts and marks the rows the tuples of the index match;
when the index is read, the rows nothing matched are tuples of NULL leaves that go through the same keys, combos and aggregates (`count(o.id)` adds 0,
`count(*)` 1, a `FILTER (WHERE o.id IS NULL)` is true). A `FULL JOIN` also keeps the tuples with no match, with NULL columns. The `ON` clause only decides
the matches: a condition on the table's own columns, a NULL key and a duplicate key leave its rows unmatched or multiply the tuples, as in the normal plan.

The normal plan stays when a `WHERE` conjunct over only the indexed table must be evaluated after preservation. The indexed table may be partitioned: the preserved table is read whole when the scan starts, whichever partitions are read. The preserved or looked-up table may be partitioned too (its plan is run once when the scan starts), and a `FULL JOIN` to a view or a sub-select with conditions keeps the rows they leave out out of the join; a `FULL JOIN` to a view on the left of the scanned table stays plain.
A nullable indexed input may contain ordinary lookup joins and expand one JSON object or array before joining to the preserved side.
Empty preserved buckets contribute one NULL fact input, including NULL lookup columns and expressions computed inside that input.
The `ON` conjuncts of such a `LEFT JOIN` that read only the indexed table (and the rows it unnests) decide which of its rows can match, so they are conditions of the scan: an `EXISTS` over the elements of an array, written with a sub-select that PostgreSQL pulls up, or a `jsonb_path_exists`, runs there, and so does an aggregate filter on a test computed inside the nullable input. The test of an absent input (`CASE WHEN absent THEN NULL ELSE x END`) is read as `x` only when `x` is not true for the NULL leaves of the row an empty bucket contributes (comparisons, `IS NOT NULL`, `EXISTS`); `x IS NULL` keeps the normal plan.
A scan with such a join runs without workers: one scan sees every row.
Fixtures `ps_*` and `j_dim_partitioned*`, `j_full_*`, rows J06 and J07 of the coverage table.
Fixtures `nf_*`, test `agg_nullable_filters`, rows J30 and J31.

An `EXISTS` over the elements of an array may read a column of a joined table, or look each element up in a mapping table (`LEFT JOIN LATERAL (SELECT .. LIMIT 1) m ON true`, keyed by the element and a joined column) before it tests the result: the lookup is PostgreSQL's plan, run once for each distinct combination of the values it reads, and a row's answer is kept while its elements and the joined values repeat. `NOT EXISTS`, `EXISTS` inside an aggregate `FILTER` and an expression that can raise keep the normal plan. Fixtures `ee_*`, row J34 of the coverage table.

## Partitioned tables

A partitioned table is one source of `Custom Scan (SiftAgg)` when every partition left after PostgreSQL's pruning (plan-time, at every level) has a
`sift_col` index of the same layout: the same columns and expressions in the same order, the same `paths`, and the same pre-aggregate options. The
attribute numbers may differ (a dropped column), and a sub-partitioned branch, a default partition and an unlogged partition are partitions like the rest.
The query is compiled once against the partitioned table's columns; the scan reads the units (segment by superblock) of each partition's index one
after the other, each with its own dictionaries, so the groups, `DISTINCT` sets and the parallel workers (which take units across all the partitions)
work as on one table, and the result is the normal plan's. `EXPLAIN` shows `Index: (partitions)` and `Partitions: a, b, ..`; the cost is the sum of the
partitions' pages and rows. Pre-aggregates answer per unit as on a plain table. Privileges and row security are checked on the partitioned table, as in
PostgreSQL (a role without access to the partitions reads through the parent).

The normal plan stays when a partition has no `sift_col` index, has an index with other columns or options, is not a heap table (`sift_table`, a foreign
table), is read with `ONLY`, or the table is an inheritance parent (a mixed tree too); when a parameter is compared with a column a hash level splits by other
than by an equality of the column's type (`k > $1`, a value of another type, an expression key: the normal plan prunes the remainders); and for the shapes the scan reads one index for (a ranked search with
`sift.score`, a text search, a time series of unions). A condition on a partition key column whose operand is a parameter of a generic plan or a stable function
(`d >= $1`, `k = ANY ($1)`, `ts >= now() - interval '30 days'`) prunes partitions when the plan starts, as in PostgreSQL: the scan evaluates the operand when
it starts and does not read the partitions whose bounds it refutes (`EXPLAIN ANALYZE` shows `Partitions Removed`); a default partition stays unless the bounds
of the others refute it. A hash level is pruned by the remainders: an equality or `= ANY` on every column of its key (a parameter, a stable function or a
constant) gives the values, the key's hash function gives each its remainder, and a partition whose remainder none of them has is not read (`k = $1`,
`k = ANY ($1)`, `a = $1 AND b = $2`; a NULL value or an empty list reads no partition). A plan reads up to 32,768 partitions (the planner reads the layout, statistics and pre-aggregates of each index once and keeps them for the backend, a few kB each, until the index's directory, the pages of its table or their visibility map, or the catalog entry of the index or of its table change; a table of more partitions, before run-time pruning, keeps the normal plan and `sift.explain_why` names `partition_count` with the number); the scan opens only the partitions that pruning leaves (the whole query over 5,000 partitions of 6 rows each runs 312 million instructions, the normal plan 300 million) and releases the pages of one before it reads the next, so a statement that prunes a table of thousands of partitions to a few opens those few (the bounds of a range key are bisected, not proved for each partition). PostgreSQL itself takes two relation locks for each partition its plan keeps (the heap and its index), and a plan of the scan takes no more: `max_locks_per_transaction` needs about 2 x the partitions of the plan whichever plan runs (the default 64 x `max_connections` covers about 3,200 partitions). The remainders of a hash level are computed once per level for the values of the keys (up to 1,048,576 combinations), not once per partition. A plan is invalidated when a partition or its index is attached, detached or dropped. `CREATE INDEX` on the parent makes one per partition (also for partitions attached later).
A top-N of a partitioned table (`ORDER BY .. LIMIT`, docs/COL-SEARCH.md) keeps its best rows with the partition each came from (its number in the high bits of the tie-break key, the TID in the low ones), reads the zones of the partitions' units as one range of superblocks, and fetches the rows it keeps from the heap of their partition, by the column names of the partitioned table. A table written first in the `FROM` list that has no index is joined to the partitioned table, which the scan reads (inner joins).
Fixtures `pa_*`, `topn_part_*` and `t_partitioned`, test `agg_partitions`.

## Partial indexes

A `sift_col` index with a `WHERE` predicate holds only the rows that pass it, so the scan reads it only when the query proves the predicate: the planner
takes the index when PostgreSQL's `predOK` is true for it (the conditions of the table imply the predicate, as for a partial btree or a BM25 index).
A query that may need a row outside the predicate keeps the normal plan, and so does a generic plan of a prepared statement whose parameter the proof
needs. Rows that an `UPDATE` moves in or out of the predicate are inserted into or deleted from the index like any other change. Of several indexes
that the query can use, a full one and a partial one, the cheaper (the partial one is smaller) wins. Fixtures `pi_*` and `t_partial`, test `agg_partial_index`.

## Uncorrelated sub-selects

`col NOT IN (SELECT ...)`, and `col IN (SELECT ...)` under `OR` or `NOT`, are hashed `SubPlan`s in PostgreSQL (a semi join is not possible for them), and
`Custom Scan (SiftAgg)` takes them as a lookup table over the leaf: PostgreSQL's own `SubPlan` answers once for every distinct value of the column, so the NULL rule
of `IN` and `NOT IN` is PostgreSQL's (one NULL in the sub-select makes every `NOT IN` NULL, no row; an empty sub-select makes it true for a NULL value too) and the
test composes with `AND`, `OR`, `NOT`, `CASE` and `FILTER`. The sub-select runs once, when the scan starts, with the query's snapshot, and `EXPLAIN` shows it as
`SubPlan` under the node; an integer column whose values span more than 4 million (the statistics may not show it) is tested row by row instead of through a table; a rescan keeps the answers, and a prepared statement's parameters are read by the sub-select as in any plan.

A test of two columns (`(a, b) NOT IN (SELECT x, y ...)`), of a column the lookup table does not hold (an `int8` column), and a test inside an expression that reads
the rows of an outer join's nullable input (a group key `CASE WHEN country IN (SELECT ..) THEN jsonb_build_object(..) END`) is not a lookup table: the scan asks
the same `SubPlan` for each tuple that reaches it.

The normal plan stays when the sub-select is correlated or reads a parameter of the outer query, is not
hashed (too large for `work_mem` and `hash_mem_multiplier`), reads a volatile function, locks rows (`FOR UPDATE`), or holds a sub-select of its own or an expression
that may raise (a division, a cast through text). A scan with such a test runs without
parallel workers: the leader runs the sub-plans. Fixtures `t_not_in_subquery*`, `t_in_subquery_or`, tests `agg_subplan_lut`, `agg_semi_lut` and `agg_nullable_input_keys`.

## ctid ranges

`ctid` compared with a constant TID (`=`, `<`, `<=`, `>`, `>=`, either side, and `BETWEEN`) is a predicate of the scan when it is a conjunct of the top level of
the `WHERE`, on a `sift_col` or `sift_table`. The conjuncts make one half-open range of TIDs (keys `block << 16 | offset`, ordered as PostgreSQL orders TIDs). The
block part is a range of superblocks (256 pages, or about 12000 rows of a `sift_table`), so a unit outside it is not read (`Units:` of `EXPLAIN ANALYZE`) and a
unit inside it is read whole; the rows of the first and last block are tested one by one against the range. A heap index stores the TID of the root of a HOT
chain while `ctid` is the visible version's, so those rows are looked up in the heap through the snapshot (HOT keeps the versions in their page, so the
other blocks need no lookup). An empty or crossed range reads no unit, a range beyond the end of the table reads none, and an invalid TID constant is the
PostgreSQL error. The cost keeps the fraction of the pages of the range.

The normal plan stays for `<>`, `ctid` in an expression, in `GROUP BY` or compared with a column, an `OR`, `IN`/`= ANY`, a parameter of a generic plan
(a custom plan folds it), a partitioned table (TIDs are per partition) and the top-N shapes (their probes read zones of units outside the range). PostgreSQL
plans a tid range scan for a narrow range (`enable_tidscan = off` shows the scan). Fixtures `tr_*`, test `agg_tid_range`.

## Memory of the group table

The planner prices the table of a scan (the groups it expects times the bytes of a group, about 185 for one key and five aggregates) against `hash_mem`; past it the scan is charged the spill, and the arm keeps the normal plan with the slug `group_memory` when that makes it dearer. The executor holds the same budget: when the table of an aggregate made of `count`, `sum`, `avg`, `min` and `max` passes it at the end of a unit, the groups go to hash partitions on disk as partial blobs (the form the workers send the leader), and the partitions are merged one by one at the end, each split again by another hash while it does not fit, up to `group_spill_depth` levels (`group_spill_parts` partitions at most). A scan that must spill runs without workers. The sums of floats are added in the order the partitions are merged, so the last digits can differ from an unbounded table's. `count(DISTINCT)` and the distinct `sum` and `avg` of numbers spill with their sets, which are united when the partitions are merged. The shapes that cannot spill (the arms of a two-level aggregate, grouping sets, set keys, a top-N, ordered-set and generic aggregates, and `sum`, `min`, `max` over combos such as numeric and text columns) keep the scan until the table is `group_nospill_factor` times hash memory (8 MB at least), and then the normal plan. Tests `agg_group_memory`, `agg_group_spill`, `agg_group_spill_limits`.

## DISTINCT ON

`SELECT DISTINCT ON (keys) ... ORDER BY keys, sort` is a group by the ON columns that keeps the columns of the first row of each group by the `ORDER BY`. The
scan returns each such column as a "first row" aggregate (`first_row(x ORDER BY ...)` in `EXPLAIN`): not the `array_agg(...)[1]` rewrite, which would sort
the rows of the groups, but the best row of each group, kept while the rows pass and compared with the sort operator, collation and `NULLS FIRST/LAST` of
the `ORDER BY`. The ORDER BY, LIMIT and OFFSET of the query run above it. With workers, each keeps its best rows and the leader merges them with the same comparison.
Several columns of the select list share one comparison per row.

The rows of a group tie unless the `ORDER BY` is a total order, and then the row is the choice of PostgreSQL's sort. So the scan is planned only when the sort
holds every column of a unique index of the table (all `NOT NULL` or made so by a strict condition of the `WHERE` clause such as `col > 0`, or `NULLS NOT DISTINCT`; not partial; of a partitioned table the index holds the partition key),
and has at most 4 columns after the ON columns. The test reads the catalog at plan time, so a prepared plan is replanned when the index is dropped.

The normal plan stays when the sort is not a total order, a sort or select list expression is volatile, a value is an array or a column of a joined table,
or the estimated groups times the bytes of their best rows are more than the memory of a hash aggregate (`work_mem` times `hash_mem_multiplier`): the groups are
not spilled. A volatile function of the select list that PostgreSQL evaluates after the rows are picked does not matter. Fixtures `do_*`, `t_distinct_on`,
`pa_distinct_on`, test `agg_distinct_on`.

## Counting distinct values over the rows of a scan

A report that aggregates per-row aggregates (`GROUP BY id, key`, a `UNION ALL` of such arms) and counts `count(DISTINCT id)` per group above them reads their rows through
a join (a spine of ranges) and a sort of every row on the group columns and the id. When the plan reads `Custom Scan (SiftAgg)` nodes and an aggregate counts distinct
`int2`, `int4` or `int8` values, `Custom Scan (SiftAggRows)` takes the rows instead: a hash table of the groups (the first spelling of a key is the one shown), `count`, `sum`
and `avg` per group (PostgreSQL's own transition and final functions, so the types and errors are the same), and for each argument of `count(DISTINCT x)` one set of
(group, value) pairs with a bit for every aggregate of that argument that took the row, so `FILTER`s share it. The set is bounded by `hash_mem`; past it the pairs go to a
tuplesort of narrow tuples and equal pairs are counted when the rows are done (`Distinct Sets Spilled` in `EXPLAIN ANALYZE`). The projection of the child plan evaluates the
group expressions, arguments and `FILTER`s, so any expression is allowed there. Grouping sets, ordered or `DISTINCT` aggregates other than `count`, and aggregates other than
`count`, `sum` and `avg` keep the normal plan. Sums and averages of floats add in arrival order (the sort adds in the order of the id), so the last digits can differ, as
between the scan's own aggregates and the normal plan's. `sift.agg_rows = off` restores the sort.

## Workload shapes

The shapes of an event analytics workload (an item table with jsonb metrics, a document with an array of parts, entity and mapping tables) are
the synthetic generator `test/col/workload.sql` and the files `bench/sql/workload/shapes/g*.sql` (`bench/sql/workload/run.sh build|plans|time`;
the status of each shape is the first line of its file). Regress test `agg_workload_shapes`, fixtures `wl_*`, coverage rows `WL01` to `WL09`.
Times are the serial plan at 300,000 items, best of 3, with `sift.enable_agg` off and on.

| Shape | Status | Off | On |
| --- | --- | --- | --- |
| g8 `EXISTS` over `jsonb_to_recordset(expr -> 'parts')` of an index expression (was a plan error, `invalid attnum`) | pushed | 1.64 s | 3 ms |
| g8 nested jsonpath `$.parts[*] ? (@.category == "x" && @.sign == "y")` | pushed | 1.52 s | 3 ms |
| g9 / g10 `x = ANY (ARRAY (SELECT ...))`, eight keys | pushed | 116 ms | 2 ms |
| g5 per-item arms, `avg(n::float8)` over a text leaf, count of the items | fused | 2.83 s | 0.07 s |
| g4 a second `count(DISTINCT)` over another column of the row | arms one by one | | follow-up |
| g7 fused arms for a selective scan (one entity) | not offered | | follow-up |
| g2 lookup keyed by a column of another joined table | pushed (regression and coverage verified) | | |
| g1 `jsonb_exists` / `?` on an object | normal plan | | follow-up |
| g3 `(doc -> 'k')::int` | normal plan | | follow-up |
| g6 one string in a typed leaf flags its unit | pushed for `jsonb_to_recordset` columns, the flagged rows read again from the heap; other reads: normal plan, leaf named at `DEBUG1` | | [design note](COL-DIRTY-UNITS.md) |

What the pushed shapes do:

- **Text-valued leaves (`astext`) in paths and recordsets.** `jsonb_to_recordset(expr -> 'a') AS o(k text)` reads the leaf of a list of objects whose
  member is declared `astext`; a text literal tests it for equality (`=`, `IN`) only when no number or boolean prints as that text (it does not start with a digit or `-` and is not `true` or `false`), so the leaf's text of the number 7 is never taken for the string `'7'`.
  A number or boolean literal against such a leaf, a strict path and a NOT of a test that may be NULL keep the normal plan (plain traps in `wl_path_parts_*`).
- **`EXISTS` over an index expression's array.** The planner deparses the expression with the query's own range table; the same expression in a
  sub-select's range table is what raised `invalid attnum`. The members of one element are tested together, as for a column.
- **`x = ANY (ARRAY (SELECT ...))`.** An uncorrelated, stable sub-select that cannot raise is an InitPlan; the scan runs it once when it starts and the
  rows are the lookup table of the leaf (as for a hashed sub-plan, so no parallel workers). A sub-select that reads the row, is volatile or may raise
  keeps the normal plan (`wl_any_array_subselect_*`).
- **Fused arms over a text leaf.** The inner `sum` / `avg` of a float8 expression the scan evaluates (a cast, `CASE`) is accumulated per row like the
  typed ones. The expression runs for every item the scan reads: the arms must share one WHERE (it is the scan's) and have no inner `FILTER`; a WHERE of
  each arm's own keeps the arms one by one. The plain cast raises for a score that is no number, in the scan as in the heap plan.

## Differential suite

`scripts/coldiff.sh` is the referee of the aggregate pushdown (`Custom Scan (SiftAgg)`): it runs a corpus of plain SQL aggregate queries
in one session with `sift.enable_agg = off`, which is the plan PostgreSQL has without sift, and again with the pushdown on, compares the
two results inside the database, and reads the plans to report which queries really used `Custom Scan (SiftAgg)`. Equality of results
and the shape of the plan are reported separately, so the harness is useful before the planner exists (both runs are then the same
plan: `scripts/coldiff.sh --no-index` is the self-check of the harness).

```sh
scripts/pgtest.sh install && scripts/pgtest.sh start       # a cluster (SIFT_TMP, SIFT_PORT)
scripts/coldiff.sh                                         # both suites, the scenarios, every variant
scripts/coldiff.sh --suite fixtures --corpus '^s7_'        # the fixture queries whose name matches
scripts/coldiff.sh --suite analytics --rows 200000         # the corpus of bench/sql/analytics on a larger table
scripts/coldiff.sh --bench --suite analytics --rows 1200000 --runs 5
scripts/coldiff.sh --selftest                              # the harness against injected differences
```

`make check` runs it as the jobs `difftest-col` (small fixtures, a subset of the variants and scenarios), `difftest-col-tbl` (the fixtures
and scenarios on a `sift_table`, `--table`) and `difftest-col-selftest` (`scripts/ci.sh`); the full tier uses larger tables and every scenario. Without the column index (no `sift_col` access method, or no
`sift_colstub`, the stub of the planner track, in a build with `SIFT_TESTING`; or a failing `CREATE INDEX`) the suites print
`coldiff: SKIP: ...` with the reason and exit 0.

### Suites

| suite | data | queries |
|---|---|---|
| `fixtures` | `test/col/fixture.sql`: an `orders` table of `--fixture-rows` rows (default 4000, `--seed`), hash generated, with NULLs, skews and a fill factor that allows HOT updates; helper tables; edge-case tables (`t_int`, `t_float`, `t_text`, `orders_odd`, `orders_part`) | `test/col/fixtures/*.sql` |
| `analytics` | the generator of `bench/sql/analytics` (`SIFT_ANALYTICS_DIR`, `--rows`, `--stores`), its indexes and `col_index.sql` | its `queries/*.sql` (`--queries GLOB`), the `pg` section of each file |

Each suite has a database of its own (`sift_coldiff_fix`, `sift_coldiff_ana`; `COLDIFF_DB` is the prefix, `--keep` keeps them) with the
conventional indexes (primary key, btree, GIN, and a BM25 index on the fixture's `memo` column), then the column index, then
`VACUUM (FREEZE, ANALYZE)`: every heap page is all-visible. The index DDL of the fixtures is in `scripts/coldiff.sh` (`--col-am`,
`--col-paths`); the analytics index is the statement of its `col_index.sql` (`--col-index FILE`), with `USING sift_col` replaced by
`--col-am` when the stub is the only access method.

### Variants

The reference run is `off`: `sift.enable_agg = off`, `max_parallel_workers_per_gather = 0`. Every other run is compared with it (`--variants`):

| variant | `sift.enable_agg` | parallel workers | `sift.simd` |
|---|---|---|---|
| `off-par` | off | 2 (free parallel costs) | auto |
| `on` | on | 0 | auto |
| `on-par` | on | 2 | auto |
| `on-scalar` | on | 0 | scalar |
| `on-par-scalar` | on | 2 | scalar |

`off-par` checks the tolerance below against PostgreSQL's own parallel plans. A variant is also run through `EXPLAIN (COSTS OFF)`: the plan
has `Custom Scan (SiftAgg)` or not, and `Gather` or not.

### Random queries (`test/fuzz/diff_fuzz.py`)

A seed gives a small database (`test/fuzz/diff_fuzz_data.sql`: NULLs, duplicates, a `sift_col` index on each fact table, a hash partitioned table, lookup tables) and
a script of episodes: a few `UPDATE`, `DELETE`, `INSERT` and `VACUUM` statements (dirty rows, write-buffer rows), then random queries of the pushdown
shapes: first-row lookups, two-level aggregates with spines, partition pruning, pre-aggregates (`FILTER` on a cube dimension, `int8` arithmetic, per-group
`min`/`max`), jsonb member casts, `EXISTS` over element lists and `jsonb_each` members, `UNION ALL` arms, top-N, and several lookups of one table in every join kind.
Each query runs as the reference and as variants (the pre-aggregates, the scan alone, the arms alone, parallel workers, scalar SIMD, no lookup pushdown) and
the rows are compared with the contract below. A divergence is shrunk (the query parts, then the statements before it) to a reproducer file; a worker or
backend that dies is reported with its query. `scripts/ci.sh` runs fixed seeds (`difftest-fuzz`, seconds); `test/fuzz/diff_fuzz_search.sh` runs a long search in capped slices.

### The comparison contract

Each result is materialized in a temporary table and compared by `coldiff.compare` (`test/col/run.sql`):

- the columns (names, types, typmods) and the number of rows are equal; a query must have a total `ORDER BY` and its rows are compared by
  position. A file of the analytics corpus has no `ORDER BY` but a `-- key:` line: both results are sorted by those columns, then by every
  column as text;
- every type but the floats is equal as text: integers, dates, text, booleans, numerics (the scale counts: `1.50` is not `1.5`), jsonb;
- `float4` and `float8`: `|x - y| <= 1e-12 + 1e-9 * max(|x|, |y|)` (`--abs`, `--rel`). Parallel workers and SIMD lanes add in another order, so
  a sum or an average of floats differs in its last digits. NaN equals NaN, an infinity only itself, `-0` equals `0`, NULL only NULL.
  The tolerance is not a license for float4: `sum(float4)` adds in float4 in PostgreSQL, so a plan that adds in double differs by about
  1e-7 relative and fails (the query `t_float4_sum` is there for it);
- a query that raises an error (a cast, an integer or float overflow) must raise an error with the **same SQLSTATE** in every variant (the
  message may name another row's value); such a file says `-- error: yes`, and a reference run that fails otherwise is a failure.

A per-order aggregate (`avg(x)`, `sum(x)` of a float under an outer aggregate) adds the elements of one order from the first to the last, in
double, and divides by their number: the value PostgreSQL gets when its plan feeds the rows of an order in the order of the array (a hash
aggregate over the unnest). PostgreSQL's own plans do not agree among themselves on the last digits: a plan that sorts by the order and the key
adds equal keys in the order of its sort, which is not stable. A condition on the average (`avg >= 9`) that holds exactly for a sum of decimals
(8.7 + 9.0 + 9.3 over three) can then count a few more or fewer orders in one plan than in another (three of 100 000 orders, between the two
arms of one query on the normal plan), whatever the scan does: the scan's count is the one of the first-to-last order. A threshold that must
be reproducible does not rest on the last digit of a float sum: the values are integers (their sums are exact) or the condition
leaves a margin.

A result that differs always fails (exit 1) and prints the first differing rows of both. Nothing is silenced: a query without a result
(an error the runner did not expect) fails too.

### The shape expectation

A file says what the plan should be: `-- expect: agg` (the pushdown must happen once it is implemented), `plain` (it must not happen:
volatile functions, unsupported or risky semantics) or `any`. Only `on` is checked for `agg`; every `on*` variant for `plain`. A mismatch
is a `NOTE` in the last column of the coverage table and in the summary line; it fails only with `--strict-shape`. The coverage table
has, per query and set: the shape, the expectation, `equal` or `DIFF`, whether `on` used SiftAgg, the time of the reference and of
`on` in ms and the speedup.

### Corpus format

One query per file, as in `bench/sql/analytics/queries`: header lines, then the query. Header lines that are not a tag below are the title.

| tag | meaning |
|---|---|
| `-- shape: S1` | the shape of the query (the name of an analytics file of the form `S1` or `G3` is its shape) |
| `-- expect: agg\|plain\|any` | the shape expectation (default `any`) |
| `-- sets: K10 K400 K4000` | the query runs once per set of `analytics_sets` (`{stores}`, `{sources}`, `{span(col)}`, `{range(col)}` are expanded as `bench/steps/analytics.py` does); `-` or absent: once |
| `-- key: 1 2` | sort both results by these columns first; `-` compares by position (the query has a total `ORDER BY`); absent: sort by every column |
| `-- scenario: yes` | part of the subset that the scenarios re-run |
| `-- guc: name = value` | a setting for every run of the file (repeatable) |
| `-- prepare: (int[], date)` and `-- args: ...` | the body is a prepared statement with `$1`...; the runs are `EXECUTE`s (with `plan_cache_mode` in a `guc` for a generic plan) |
| `-- requires: SELECT ...` | the file runs only if the statement returns true (repeatable), e.g. a collation that exists |
| `-- wrap: subquery` | materialize `SELECT * FROM (query)`, for a statement that `CREATE TABLE AS` does not take (`FOR UPDATE`) |
| `-- error: yes` | the query raises an error in the reference run |
| `-- error: either` | the query raises an error in the reference run and in the scan; their SQLSTATE may differ (a value the index cannot store: `0A000` against the cast error `22P02`) |
| `-- isolation: serializable` | the file runs in a transaction of that isolation level |

The sections `--@ pg`, `--@ flat`, `--@ ch` of an analytics file are cut: only `pg` is run. `-- tol:` is ignored: the contract above is the
tolerance.

The fixture corpus (about 180 files; lookup joins `j_*` with semi, anti, key expressions and ON filters, per-order arms with sets, gates and general HAVING `s2_per_order_*` / `t_two_level_*`, mapped aggregates `t_mapped_agg_*`) has the shapes of the report queries (S1 summary of an object with dynamic keys by key, mapped key and period;
S2 summary of an array of objects with per-order pre-aggregation, the three rollup levels as `UNION ALL` and as `GROUPING SETS`; S4 time
series at day, week, month and quarter, with a date spine; S5 per-store rows and top-N; S6 `jsonb_path_exists` on an array of objects; S7 the
same-element filter as `EXISTS`, `@>` and `jsonb_path_exists`), flat aggregates and quals (`IN`, `NOT IN`, `BETWEEN`, `LIKE`, `OR`, `NOT`,
`CASE`, `IS [NOT] NULL`, `FILTER`, `HAVING`, grouping by expressions), NULL-heavy documents and empty sets, a BM25 match with an aggregate,
a prepared statement with a generic plan, `numeric(p, s)` columns (predicates against constants of every precision, NaN and the infinities, keys, expressions, distinct counts), group keys up to the limit of 8, rollups and cubes of up to 7 columns (`k_keys_*`, `k_rollup_*`, `k_cube_*`) and the plain traps one past each limit (9 keys, 9 joined tables, 17 combos), lookup joins of up to 8 tables and 16 combos (`j_joins_*`, `j_combos_*`), generic aggregates (`g_*`: the variance family, `DISTINCT` forms over one or several arguments and with a spill, `ORDER BY` in the call, ordered and hypothetical sets, variadic and stable-fraction forms, `array_agg`/`string_agg`, expression arguments, joined columns, rollups, user-defined aggregates, parallel combine, `x_bool_*`: `bool_and`, `bool_or` and `every` merged from two workers) with their traps (a volatile transition function, a float read into text with `-0`), and the traps: numeric division, products past 38 digits and float8 casts, `random()` in the qual and in the aggregate, `sum(int4)` and `sum(int8)` past
the type, integer, float and cast errors, NaN, infinities and `-0`, `sum(float4)`, text `min`/`max` under a non-C collation, JSON values
that are not numbers, `DISTINCT ON` (`do_*`: latest and earliest rows, mixed sort directions, expressions, joins, every column type, and the plain traps), outer joins, a set-returning function in the select
list, window functions over the aggregate (`w_*`), `TABLESAMPLE`, `FOR UPDATE` and `SERIALIZABLE`; partitioned tables (`pa_*`: pruning at every level, a dropped column before the index, a default and an unlogged partition, `ONLY`, partitions without the index or with other options, inheritance, run-time pruning of parameters, stable functions and hash remainders) run DML through the parent in the scenarios.

### Scenarios

After the fresh run, the fixture subset (`-- scenario: yes`) is compared again after DML that leaves the heap in every state the column
index must answer for (`--scenarios`, default all). They are cumulative and only equality is checked:

| scenario | heap state |
|---|---|
| `insert` | 20% more rows (also of `t_jp` and `t_eo`) without a `VACUUM` (pages not all-visible) |
| `update` | an UPDATE of indexed columns (new tuple versions) and of the jsonb documents (also of `t_jp` and `t_eo`) |
| `delete` | hard deletes (also of `t_jp` and `t_eo`) and soft deletes (`deleted_at` set) |
| `abort` | a rolled-back INSERT, UPDATE and DELETE (dead tuples before any `VACUUM`) |
| `vacuum` | `VACUUM` after all that |
| `hot` | UPDATE of columns no index holds: HOT updates (the count is printed) |
| `recycle` | DELETE, `VACUUM (TRUNCATE off)`, then INSERT: new rows on the TIDs of deleted ones (the count is printed) |
| `held` | a second session with an uncommitted INSERT, UPDATE and DELETE next to the corpus, at READ COMMITTED and inside a REPEATABLE READ transaction; then a REPEATABLE READ snapshot taken before a writer commits |
| `maint` | `REINDEX`, then `sift.col_promote` and `sift.col_merge` when they exist |

### jsonpath truth tables

The fixtures `jp_*` read `t_jp`, whose members `s` (text), `n` (int4) and `b` (boolean) take every value of their leaves (`1` and `1.0`, the
empty string, non-ASCII text, `2147483647`), a JSON null and a missing key. The table is the comparison of an element `e` with a literal `L`
of the member's type; a scalar or null element, and a missing key, are the same as a missing member:

| member of `e` | `@.k == L` | `@.k != L` | `!(@.k == L)` | `!(@.k != L)` |
|---|---|---|---|---|
| a value equal to `L` | true | false | false | true |
| another value of the type | false | true | true | false |
| JSON null | false | true | true | false |
| missing | false | false | true | true |
| a value of another JSON type | unknown | unknown | unknown | unknown |

Against a `null` literal: `== null` is true for a JSON null only, `!= null` for every present member that is not null, and under `!()` the
other rows. Each form pushed down is equal to the heap in every variant and scenario; a form that is not provable (the last row of the
table, a number against a float leaf, strict `!()`, `jsonb_path_match`) is a `plain` fixture. A member the leaf cannot hold (an array or
an object in a leaf of numbers, a number out of range) keeps the normal plan. Complete kinds are guaranteed by column minor 6; older units or write buffers with uncertain kinds also keep the normal plan for tests that require them. REINDEX or VACUUM FULL restores the guarantee.

### An element against a column of its row

The fixtures `eo_*` read `t_eo`: the members `q` (int4), `q8` (int8) and `f` (float8) of its array against the columns `r4`, `r8` and `rf` of the same row,
with NULL columns, a NaN and a `-0`, a JSON null and a missing member, empty arrays and a NULL document. Element `j` of row `i` holds the value that
`r4` has in the row `i + j - 1`, so every row has elements equal to the columns of its neighbours and the rows that must not match are there. An
element is compared with the column of its own row (`= <> < <= > >=`, either side first), NULL on either side is unknown, and the other tests of the
same element (`AND`, `OR`, `IS NULL`, a literal) are tested on that element. An expression of the column, a column of another type class (an int4 member
against a float8 column), `LIMIT` that is not a constant, `OFFSET` and a second array keep the normal plan (`eo_trap_*`).

### Bench mode

`--bench [--runs N]` runs only the first pass (no scenarios) with the median of N runs (default 5, after a warm run) of the reference and of
`on`, and prints the go/no-go lines: the K4000 queries that must push down (`expect: agg`, and the S shapes of the analytics corpus) at
least 20 times faster than the reference, the K10 and K400 ones that push down not slower (0.95). A target that is missed is printed;
it fails the run only with `--strict-shape`. The times include the temporary table of the result.

### Self-test

`scripts/coldiff.sh --selftest` runs a subset of the fixture corpus plain against plain (must pass) and then with `--mutate KIND`, which perturbs
every `on*` result as a bug of the pushdown would. `int` (+1), `float` (relative 1e-6, beyond the tolerance), `text`, `null`, `rows` (the last
row lost), `dup` (the first row twice), `order` (rows 1 and 2 swapped, seen by a query compared by position), `type` (a wider column type)
and `cols` (an extra column) must fail; `float-tiny` (relative 1e-11, inside the tolerance) must pass; a mutation that applied to no result
fails the self-test.

## Plan-choice gate

`scripts/plangate.sh` guards the choice between the normal plan and `Custom Scan (SiftAgg)`. It builds the tables of `test/plan/gen.sql` (events, dims, facts, a month spine and a partitioned copy;
`--rows`, default 30000, and `--seed`; every row is a hash of its number, and `ANALYZE` reads all of them, so the statistics are the same on every run) and runs each shape of
`test/plan/shapes.sql` three ways: native (`sift.enable_agg` off), chosen (the planner's own choice) and scan (the table scans off, so the pushdown runs where the query allows it).
The results must agree by the contract of the differential suite (floats within 1e-12 + 1e-9 of the larger), the plan chosen must be the one the shape expects, and the planner's cost
of the scan over the native one (the cost ratio) must stay in the shape's band, so that a drift shows before it flips a choice. The table prints, for each shape, the plan chosen, the expected one, the
ratio and its band, the time of native and of scan (median of `--runs`, default 3) and a verdict: `win` or `lose` when the scan is chosen and faster or slower by more than 1 ms and 20%, `miss` when
it is not chosen but would win by as much, else `tie`. A wrong result, an unexpected plan or a ratio out of its band fails; so does a `lose`, unless `--warn-time` (the full tier runs at 50000 rows with it: hosted runners
are noisy). The quick tier runs it without timings (`--runs 0`). The expectations hold from 20000 to 50000 rows; a shape whose plan changes inside that range expects `either`.

To add a shape, append three lines to `test/plan/shapes.sql`: `-- name: x`, `-- expect: new` and the query (a total `ORDER BY` when it has a `LIMIT`); run `scripts/plangate.sh --update` and read the diff:
it writes the plan chosen and the band of a factor of 2 around the measured ratio (and refreshes the bands of every shape; a plan stays as it is except `new`).
