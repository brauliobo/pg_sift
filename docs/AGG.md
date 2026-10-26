# Aggregate pushdown

How `Custom Scan (SiftAgg)` handles lookup joins, partitions, partial indexes, sub-selects, `ctid` ranges and distinct counts, and how the differential suite checks it.

## Joins to plans

The right side of a lookup join may be a table or a plan: a materialized CTE, a `VALUES` list, a function (`generate_series`), or a sub-select PostgreSQL keeps (`GROUP BY`, `LIMIT`, `DISTINCT`, a set operation, a ranked scan with `LIMIT`) in an inner, `LEFT`, `IN` or `NOT EXISTS` join. `UNION [ALL]` of sub-selects, a statement-level CTE's sub-select and a nested `WITH` count too, and so does the `WITH` of a derived table (a ranked top-N over an aggregate) while the derived table reads nothing of the queries around it; a CTE of it that several aggregates read is planned inline like the statement's own (the cheaper plan wins). A lateral sub-select that reads the row around it keeps the normal plan. Test `agg_nested_ctes`, fixtures `nf_nested_cte_*`, row J32.

PostgreSQL's plan of the right side is the child of the node (`EXPLAIN`: `Lookup Joins: s (subquery)`). The scan runs it once at start with the query's snapshot; its rows are the lookup table, so duplicate keys multiply rows and a NULL key matches nothing, as in the normal plan (`EXPLAIN ANALYZE`: `Lookup Rows Read`). A rescan replays the groups; prepared-statement parameters are read by the child plan as usual.

The normal plan stays when the right side reads a volatile function, the join has no condition (`ON true`) and is not lateral, the right side is estimated above `sift.agg_lookup_max_kb` (rows times width; for a lateral one, its rows per distinct input of the scanned table, together), or its plan (an `Append`) outputs only the relation's columns and a condition reads another. A scan with such a join runs without workers.

**Lateral right sides** that read the joined row (`LEFT JOIN LATERAL (SELECT .. LIMIT 1)`, `UNION [ALL]` arms, `generate_series`, `unnest` of an array column, a set-returning function) keep PostgreSQL's parameterized plan as the child. The scan runs it once per distinct value of the columns it reads (lookup keys of the table or another joined table's columns) and multiplies rows by what it returns, duplicates and NULL-extended rows included.

- A call on a document column that cannot raise (`jsonb_exists(doc, 'k')`, `doc ? 'k'`, `doc ->> 'k'`, `jsonb_typeof(doc)`) passes its value as the parameter if the index holds it as an expression column spelled like the query's. `jsonb_exists(doc, 'k')` gives key presence for any JSON root (a path leaf cannot tell a missing key from a JSON null).
- The scan runs the child for the tuples that reach its join, PostgreSQL's plan in another order. A child that can raise (a division, a text cast, a sub-select) is therefore joined only when the query joins nothing else (the scanned table's conditions come first in both plans); else the normal plan stays, as for a child that reads the whole document or a column of a query above the aggregate's own (a sub-select of an outer query: its value changes per row of that query).
- A `jsonb_each` member `r.value` reaches the child as the jsonb its stored float stands for (the number, or a JSON null when absent), so `CAST(r.value AS integer)` works; the cast runs only for members of rows that reach the child and raises PostgreSQL's own error (`integer out of range`). A document the index cannot hold as numbers (a string or object member) keeps the normal plan.
- A path declared `float8` carries no scale: a child reading the member otherwise (a cast to `numeric` or `boolean`, `r.value` itself, as text) would see `12.5` for `12.50`, so it keeps the normal plan (test `agg_child_member_value`).
- A lateral sub-select PostgreSQL does not pull up because its query is on the nullable side of an outer join (`dates LEFT JOIN cte`; below `RIGHT` or `FULL` the normal plan stays) is pulled up within its own query level by the planner hook (`agg/flat.c`), which plans both ways and keeps the flat plan when it runs in the scan and costs less. A sub-select with a join of its own attaches through its leftmost table, as `(a JOIN jsonb_each(a.doc) r) LEFT JOIN m ON ..`.

`LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'` with a constant key is member `k` of each row (one member per key; a row without it stays, with NULL columns). The scan reads it at row level as `(doc ->> 'k')::float8` and unnests nothing. A correlated child reads `r.value` only through casts of jsonb to an integer or float (a missing member and a JSON null are the same NULL; a JSON null casts to NULL in PostgreSQL 18); before PostgreSQL 18, with another condition, or with `r.key` read, the normal plan stays.

`LEFT JOIN LATERAL` over `jsonb_to_recordset` or `jsonb_array_elements` of a list of the scanned row, `ON true` or with its own conditions (sub-select `WHERE`, `ON`), is answered too: the conditions say which elements exist, and a row none of whose elements passes (no list, empty, all failing) is one tuple with NULL element columns, found before the snapshot check so a row the snapshot does not see is no tuple. The normal plan stays for a `WHERE` above the join that PostgreSQL could not turn into an inner join (`p.n IS NULL` sees the NULL tuple), a lateral condition that may raise, a lateral reading the row's other lists, and other outer joins of an unnested JSON function; `sift.agg_left_lateral` off keeps it for all of them. Test `agg_left_lateral`, fixtures `j_left_lateral*`, fuzz family `leftlateral`.

Other rules:

- Predicate-only joins (inequalities, range containment) test every candidate row; overlapping and duplicate matches multiply the inputs.
- Lookup keys, grouping expressions and predicates may read earlier lookup outputs; duplicates and NULL-extended rows stay distinct inputs. Inner lookups are ordered by dependency within each join boundary; statement-level materialized CTEs can be referenced from nested queries.
- Aggregate `FILTER` predicates may read lookup columns and source values together. They run per surviving joined tuple after all joins; an excluded tuple never evaluates a filter expression that could raise.
- A condition PostgreSQL pushed into a sub-select (the run condition of a ranking window, `rn <= 3`) is enforced by its plan, which then does not output the column the condition read; the scan does not test it again (a column the query reads keeps its place and the condition is tested as usual). Test `agg_pushed_conditions`, fixtures `nf_pushed_*`, row J33.

Tests `agg_correlated_children`, `agg_correlated_fanout`, `agg_derived_correlation`, `agg_append_children`, `agg_raw_value_children`, `agg_lateral_flatten`, `agg_left_each`, `agg_left_unnest`, `agg_plan_tables`, `agg_child_predicates`, `agg_lookup_dependencies`, `agg_statement_cte_children`; fixtures `pt_*`.

## Join key filters and shared lookups

An inner or semi lookup join whose key is one integer (`int8` too) or date leaf of the index (`JOIN stores s ON s.id = o.tenant_id`, `IN (SELECT ..)`, `EXISTS`) needs a row of the table with that key. The table's keys, loaded at scan start with its own conditions applied, therefore become a scan conjunct `leaf IN (keys)`: a byte table over the key range (a hash table above 4 million values), with the same test, zone pruning and source filters as an `IN` list of constants. A unit whose zone misses the range is not read (`Units Pruned`); other rows are tested as a set before a tuple is made, where the join would resolve every tuple through the combo cache. A multi-column join adds one conjunct per integer key.

- A join keyed by a column of that table (`JOIN ids ON ids.id = d.id` after `JOIN d ON d.id = f.d_id`, inner or semi, no condition on the scan's own columns) drops the rows of `d` it has no match for, so only the others' keys enter the conjunct; no key at all (empty selection) reads no unit. The chain is also probed from the scanned row's column inside the rows of a calendar `LEFT JOIN`: the keys of `B` are a conjunct there too, and a join no tuple of the conjuncts fails is not looked up per tuple.
- Two tables keyed by the same scanned column give one conjunct, the narrower: `IN (A)` with `A` within `IN (B)` is `IN (A)`, and the pass over the rows for `B` is not made.
- `EXPLAIN ANALYZE`: `Join Key Filters`; `sift.agg_join_filter = off` restores the plain join.
- When the key is a column a partitioned table is split by, the keys also drop partitions none can be in (`Partitions Removed`): a range or list bound by the key range and, with few keys, by each key; a hash level by the keys' remainders; no key reads no partition.
- Rows equal the normal plan's, and a tuple the join drops never reaches an expression (a conjunct on the fact rows that may raise runs on rows whose key the table holds, as in a plan that joins before filtering).
- No conjunct for `LEFT`, `RIGHT`, `FULL` and anti joins, text keys, element-level leaves and top-N scans. A plan with a preserved side still gets the conjuncts of its inner and semi joins (a tuple failing one never matches a preserved row, and unmatched preserved rows remain the NULL rows of the groups).
- When the join returns no column of the table, the filter holds the keys and an inner join's keys are unique, tuples the filter passed are not looked up again: `Join Key Passes`.
- Keys spread over more than 4 million values also skip units whose zone holds none. Two sets on one leaf (an `IN` list or a dimension's keys, plus a join's keys) are met before the zones are tested: a unit is read when its zone holds a value of both (test `agg_zone_sets`).
- Cost keeps the units the keys' zones leave (the leaf's mean zone width is stored with the index; keys count as points).

Tests `agg_join_filter`, `agg_join_key_types`, isolation spec `agg_join_filter`, fixtures `j_join_filter_*`, row J28.

A combo of one input (a join key, a one-leaf grouping expression) finds the entry of an id below 65,536 by array index; larger or negative ids and multi-input combos use the hash table. A combo whose inputs are columns of one earlier lookup's output row (and the one leaf that lookup is keyed by, as the month calendar of a period join) finds its entry by that row: the row names the leaf's value, so a tuple costs one array read.

**Sharing.** `UNION ALL` arms (or a CTE read several times) of one statement that join the same table with the same key, output columns and conditions read the same rows in one snapshot: the first node loads them into a statement context and the others reuse the table (`sift.agg_share_lookups`, `Shared Lookup Tables`). Arms with other columns or conditions, a plan as table, a preserved side, and parallel workers load their own. Test `agg_share_lookups`, isolation spec `agg_share_lookups`, fixture `j_share_arms`, row J29.

**Per-backend cache.** A backend keeps such a table's rows for its next statement (`sift.agg_lookup_cache_kb`, `Cached Lookup Tables`), since loading a mapping table of half a million rows is the fixed cost of every query joining it. Rows are kept under the table's conditions while every heap page is visible to all transactions (vacuumed) and has the LSN it had at load. A write in flight, committed or rolled back, a vacuum that rewrites a page and a table rewrite all force a reload. Never kept: a table with a page not yet visible to all (rows not vacuumed since a write), a non-constant condition, a preserved side, a plan or non-heap table, and parallel workers. Test `agg_lookup_cache`, isolation spec `agg_lookup_cache`.

**Cross-backend cache** (`sift.agg_lookup_shared_kb`, on by default; superuser; -1 is the smaller of `sift.agg_lookup_cache_kb` and 5% of `shared_buffers`, 0 turns it off). A backend that loads a table writes its rows, in the flat form of a parallel scan, into a dynamic shared memory segment and lists it in a directory of 64 entries reachable by name from every backend (the DSM registry: no restart, no preload). A backend that finds the key's entry with the pages it sees now (same LSNs, all visible, as above) attaches and reads in place, so a one-query connection pays the attach, not the load.

- The segment holds key and pages and they are compared in full; the directory only has hashes. Rows are per role, as in a backend's cache.
- A write moves the LSNs; the old state's entry goes when the key is kept again.
- Entries together take up to the setting (least recently used first; a table over half of it is not kept). A segment dropped from the directory stays until its readers finish. A crash restart discards registry and segments.
- A backend loading while others load the same table (cold start) loads for itself.

Test `agg_lookup_shared`, isolation spec `agg_lookup_shared`, TAP test `018_lookup_shared`.

**Semi/anti trees.** The right side of a semi or anti join may be a tree of joined tables (`IN (SELECT .. FROM a JOIN b ..)`, `EXISTS (SELECT 1 FROM a JOIN b ON .. WHERE a.k = o.k)`, or the `coalesce` of the LEFT JOIN PostgreSQL makes of `p.id IN (SELECT k FROM (SELECT coalesce(a.k, b.k) AS k FROM a LEFT JOIN b ..) t)`). The tree is PostgreSQL's join of those tables; the scan runs its cheapest serial plan once at start, as for a sub-select, and looks keys up. The scan-side condition reads one table of the tree (the key); ON conditions on the tree's own columns are already in its plan. The normal plan stays for a tree that reads the query's other tables, has only a parameterized or parallel plan, or has more keys than `sift.agg_lookup_max_kb` allows, and for a key that is an expression of two tables of the tree straight under the scan (`o.k IN (SELECT coalesce(a.k, b.k) FROM a LEFT JOIN b ..)`; `from_shape`). Test `agg_semi_tree`, fixtures `j_semi_tree*`.

## Outer joins that keep the other table

A `RIGHT` or `FULL JOIN` to a table, and a `LEFT JOIN` whose right side is the indexed table, aggregate over the table that keeps its rows (`EXPLAIN`: `Lookup Joins: stores (preserved)` or `(full)`). The scan reads that table at start and marks the rows the index's tuples match; when the index is read, unmatched rows become tuples of NULL leaves going through the same keys, combos and aggregates (`count(o.id)` adds 0, `count(*)` 1, `FILTER (WHERE o.id IS NULL)` is true). A `FULL JOIN` also keeps tuples with no match, with NULL columns. `ON` only decides matches: a condition on the table's own columns, a NULL key or a duplicate key leave rows unmatched or multiply tuples, as in the normal plan.

- The normal plan stays when a `WHERE` conjunct over only the indexed table must be evaluated after preservation.
- The indexed table may be partitioned: the preserved table is read whole at start, whichever partitions are read. The preserved or looked-up table may be partitioned too (its plan runs once at start). A `FULL JOIN` to a view or sub-select with conditions keeps the rows they leave out out of the join; a `FULL JOIN` to a view left of the scanned table stays plain.
- A nullable indexed input may contain ordinary lookup joins and expand one JSON object or array before joining the preserved side. Empty preserved buckets contribute one NULL fact input, NULL lookup columns and expressions included.
- `ON` conjuncts of such a `LEFT JOIN` that read only the indexed table (and rows it unnests) decide which of its rows can match, so they are scan conditions: an `EXISTS` over array elements (a sub-select PostgreSQL pulls up) or a `jsonb_path_exists` runs there, and so does an aggregate filter on a test computed inside the nullable input. The test of an absent input (`CASE WHEN absent THEN NULL ELSE x END`) is read as `x` only when `x` is not true for the NULL leaves an empty bucket contributes (comparisons, `IS NOT NULL`, `EXISTS`); `x IS NULL` keeps the normal plan.
- A scan with such a join runs without workers: one scan sees every row.

Fixtures `ps_*`, `j_dim_partitioned*`, `j_full_*`, `nf_*`, test `agg_nullable_filters`, rows J06, J07, J30, J31 of the coverage table.

An `EXISTS` over array elements may read a column of a joined table, or look each element up in a mapping table (`LEFT JOIN LATERAL (SELECT .. LIMIT 1) m ON true`, keyed by the element and a joined column) before testing the result. The lookup is PostgreSQL's plan, run once per distinct combination of the values it reads, and a row's answer is kept while its elements and joined values repeat. `NOT EXISTS`, `EXISTS` inside an aggregate `FILTER` and an expression that can raise keep the normal plan. Fixtures `ee_*`, row J34.

## Partitioned tables

A partitioned table is one source of `Custom Scan (SiftAgg)` when every partition left after PostgreSQL's plan-time pruning (at every level) is a `sift_table` or has a `sift_col` index, all of one kind, of the same layout: the same columns and expressions in order, the same `paths`, the same pre-aggregate options. Attribute numbers may differ (a dropped column), the order of the columns of a `sift_table` may not; sub-partitioned branches, default and unlogged partitions are partitions like the rest. The query is compiled once against the partitioned table's columns; the scan reads each partition's units (segment by superblock) one after the other, each with its own dictionaries, so groups, `DISTINCT` sets and parallel workers (which take units across all partitions) work as on one table, with the normal plan's result. `EXPLAIN` shows `Index: (partitions)` and `Partitions: a, b, ..`; cost is the sum of the partitions' pages and rows. Pre-aggregates answer per unit as on a plain table. Privileges and row security are checked on the partitioned table, as in PostgreSQL (a role without access to partitions reads through the parent).

**Per-partition finalization.** When the group key holds every column of the partition key (the column itself, not an expression) and the partitions are direct children, a partition's groups are final when its units end. A scan without workers and with a `count(DISTINCT)` (the aggregate whose per-group sets make the table large; other aggregates keep the single table) then reads a partition as one item, emits its finished groups (with integer sums and averages, counts, minima and maxima beside the sets) into the result tuples and restarts the group table. The table holds one partition's groups, not all, and does not spill at `work_mem`. `EXPLAIN`: `Groups Finished: per partition`; cost is that of one partition's table. The single table stays for a key without the partition key, a partition of a partition, an expression as partition key, grouping sets, `DISTINCT ON`, ordered-set aggregates, a scan with workers, and a float sum or average (its lanes add in an order partitions would change). Test `agg_part_final`.

The normal plan stays when:

- a partition has no `sift_col` index or one with other columns or options, is a foreign table, partitions are of both kinds (`sift_table` and heap with an index), is read with `ONLY`, or the table is an inheritance parent (a mixed tree too);
- a parameter is compared with a hash-level column other than by an equality of the column's type (`k > $1`, a value of another type, an expression key: the normal plan prunes the remainders);
- the scan reads one index for the shape (a ranked search with `sift.score`, a text search, a time series of unions).

**Run-time pruning and limits**

- A condition on a partition-key column whose operand is a generic-plan parameter or a stable function (`d >= $1`, `k = ANY ($1)`, `ts >= now() - interval '30 days'`) prunes at plan start as in PostgreSQL: the scan evaluates the operand at start and skips partitions whose bounds it refutes (`EXPLAIN ANALYZE`: `Partitions Removed`); a default partition stays unless the others' bounds refute it.
- A hash level is pruned by remainders: an equality or `= ANY` on every column of its key (parameter, stable function or constant) gives the values, the key's hash function their remainders, and a partition none of them hits is not read (`k = $1`, `k = ANY ($1)`, `a = $1 AND b = $2`; a NULL value or empty list reads no partition). Remainders are computed once per level for the key values (up to 1,048,576 combinations), not per partition.
- A plan reads up to 32,768 partitions. The planner reads each index's layout, statistics and pre-aggregates once and keeps them in the backend (a few kB each) until the index's directory, its table's pages or visibility map, or the catalog entry of the index or table change. A table with more partitions, before run-time pruning, keeps the normal plan and `sift.explain_why` names `partition_count` with the number.
- The scan opens only the partitions pruning leaves (the whole query over 5,000 partitions of 6 rows each runs 312 million instructions, the normal plan 300 million) and releases one's pages before reading the next, so a statement pruning thousands of partitions to a few opens only those (range-key bounds are bisected, not proved per partition).
- PostgreSQL takes two relation locks per partition its plan keeps (heap and index), and the scan's plan takes no more: `max_locks_per_transaction` needs about 2 x the plan's partitions whichever plan runs (the default 64 x `max_connections` covers about 3,200).
- A plan is invalidated when a partition or its index is attached, detached or dropped. `CREATE INDEX` on the parent makes one per partition (also for partitions attached later).

A top-N of a partitioned table (`ORDER BY .. LIMIT`, docs/COL-SEARCH.md) keeps its best rows with their partition (number in the high bits of the tie-break key, TID in the low ones), reads the zones of all partitions' units as one range of superblocks, and fetches kept rows from their partition's heap by the partitioned table's column names. An unindexed table written first in `FROM` is joined to the partitioned table the scan reads (inner joins). Fixtures `pa_*`, `topn_part_*`, `t_partitioned`, test `agg_partitions`.

## Partial indexes

A `sift_col` index with a `WHERE` predicate holds only rows passing it, so the planner takes it only when the query proves the predicate (PostgreSQL's `predOK`: the table's conditions imply it, as for a partial btree or BM25 index). A query that may need a row outside the predicate keeps the normal plan, as does a generic plan of a prepared statement whose parameter the proof needs. Rows an `UPDATE` moves in or out of the predicate are inserted into or deleted from the index like any change. Among usable indexes the cheaper wins (a partial one is smaller; likewise an index cut by a key (`key_cut`) against one cut by block range), whatever the creation order. Every index that gives a plan is planned and priced, about one plan of a single-index table (0.05 to 0.1 ms) per extra index. Fixtures `pi_*`, `t_partial`, test `agg_partial_index`.

## Uncorrelated sub-selects

`col NOT IN (SELECT ...)`, and `col IN (SELECT ...)` under `OR` or `NOT`, are hashed `SubPlan`s in PostgreSQL (no semi join is possible); `Custom Scan (SiftAgg)` takes them as a lookup table over the leaf. PostgreSQL's own `SubPlan` answers once per distinct column value, so the NULL rule of `IN`/`NOT IN` is PostgreSQL's (one NULL in the sub-select makes every `NOT IN` NULL, no row; an empty sub-select makes it true for a NULL value too), and the test composes with `AND`, `OR`, `NOT`, `CASE` and `FILTER`. The sub-select runs once at scan start with the query's snapshot; `EXPLAIN` shows a `SubPlan` under the node. An integer column whose values span more than 4 million (statistics may not show it) is tested row by row instead of through a table. A rescan keeps the answers; prepared-statement parameters are read by the sub-select as usual.

No lookup table is built for a test of two columns (`(a, b) NOT IN (SELECT x, y ...)`), of a column the table does not hold (an `int8` column), or inside an expression that reads the rows of an outer join's nullable input (a group key `CASE WHEN country IN (SELECT ..) THEN jsonb_build_object(..) END`): the scan asks the same `SubPlan` for each tuple that reaches it.

The normal plan stays when the sub-select is correlated or reads an outer-query parameter, is not hashed (too large for `work_mem` and `hash_mem_multiplier`), reads a volatile function, locks rows (`FOR UPDATE`), or holds a sub-select of its own or an expression that may raise (a division, a cast through text). A scan with such a test runs without parallel workers: the leader runs the sub-plans. Fixtures `t_not_in_subquery*`, `t_in_subquery_or`, tests `agg_subplan_lut`, `agg_semi_lut`, `agg_nullable_input_keys`.

## ctid ranges

`ctid` compared with a constant TID (`=`, `<`, `<=`, `>`, `>=`, either side, `BETWEEN`) is a scan predicate when it is a top-level `WHERE` conjunct, on a `sift_col` or `sift_table`. The conjuncts make one half-open TID range (keys `block << 16 | offset`, in PostgreSQL's TID order). The block part is a range of superblocks (256 pages, or about 12000 rows of a `sift_table`): units outside are not read (`Units:` in `EXPLAIN ANALYZE`), units inside are read whole, and rows of the first and last block are tested one by one. A heap index stores the TID of a HOT chain's root while `ctid` is the visible version's, so those rows are looked up in the heap through the snapshot (HOT keeps versions in their page, so other blocks need no lookup). An empty or crossed range, or one beyond the table's end, reads no unit; an invalid TID constant is PostgreSQL's error. Cost keeps the fraction of the range's pages.

The normal plan stays for `<>`, `ctid` in an expression, in `GROUP BY` or compared with a column, an `OR`, `IN`/`= ANY`, a generic-plan parameter (a custom plan folds it), a partitioned table (TIDs are per partition) and top-N shapes (their probes read zones outside the range). PostgreSQL plans a tid range scan for a narrow range (`enable_tidscan = off` shows the scan). Fixtures `tr_*`, test `agg_tid_range`.

## Memory of the group table

The planner prices a scan's group table (expected groups times bytes per group, about 185 for one key and five aggregates) against `hash_mem`; past it the scan is charged the spill, and the arm keeps the normal plan with slug `group_memory` when that makes it dearer.

The executor holds the same budget. When the table of an aggregate made of `count`, `sum`, `avg`, `min` and `max` passes it at the end of a unit, the groups go to disk hash partitions as partial blobs (the form workers send the leader); partitions merge one by one at the end, each split again by another hash while it does not fit, up to `group_spill_depth` levels (`group_spill_parts` partitions at most). A scan that must spill runs without workers. Float sums add in partition-merge order, so last digits can differ from an unbounded table's.

- `count(DISTINCT)` and distinct `sum`/`avg` of numbers spill with their sets, united when partitions merge. Each set is walked once for all partitions of a spill, and a `count(DISTINCT)` whose bitmaps held less than a value per 128 bytes of chunks at spill time starts the next tables as a hash set.
- The set of a column whose values fall in a window of at most 512 (`dist_narrow_words` words of 64 bits) keeps a few words per group instead of a directory and a 1,024-byte chunk, becoming a bitmap when a value falls outside; an integer column whose zones span more than the window starts as a bitmap.
- Shapes that cannot spill (arms of a two-level aggregate, grouping sets, set keys, top-N, ordered-set and generic aggregates, and `sum`/`min`/`max` over combos such as numeric and text columns) keep the scan until the table is `group_nospill_factor` times hash memory (8 MB at least), then the normal plan.

Tests `agg_distinct_narrow`, `agg_group_memory`, `agg_group_spill`, `agg_group_spill_fuzz`, `agg_group_spill_limits`.

## DISTINCT ON

`SELECT DISTINCT ON (keys) ... ORDER BY keys, sort` is a group by the ON columns keeping the first row's columns per `ORDER BY`. Each such column is a "first row" aggregate (`first_row(x ORDER BY ...)` in `EXPLAIN`), not the `array_agg(...)[1]` rewrite (which sorts the groups' rows): it keeps each group's best row, compared with the `ORDER BY`'s operator, collation and `NULLS FIRST/LAST`. The query's ORDER BY, LIMIT and OFFSET run above it. With workers each keeps its best rows and the leader merges them with the same comparison; several select-list columns share one comparison per row.

Rows of a group tie unless the `ORDER BY` is a total order, and then the row is the choice of PostgreSQL's sort. So the scan is planned only when the sort holds every column of a unique index of the table (all `NOT NULL` or made so by a strict `WHERE` condition such as `col > 0`, or `NULLS NOT DISTINCT`; not partial; on a partitioned table the index holds the partition key) and has at most 4 columns after the ON columns. The catalog is read at plan time, so a prepared plan is replanned when the index is dropped.

The normal plan stays when the sort is not a total order, a sort or select-list expression is volatile, a value is an array or a joined table's column, or estimated groups times bytes of best rows exceed hash-aggregate memory (`work_mem` times `hash_mem_multiplier`): groups are not spilled. A volatile select-list function PostgreSQL evaluates after the rows are picked does not matter. Fixtures `do_*`, `t_distinct_on`, `pa_distinct_on`, test `agg_distinct_on`.

## Counting distinct values over the rows of a scan

A report that aggregates per-row aggregates (`GROUP BY id, key`, a `UNION ALL` of such arms) and counts `count(DISTINCT id)` per group above them reads their rows through a join (a spine of ranges) and a sort of every row on group columns and id. When the plan reads `Custom Scan (SiftAgg)` nodes and an aggregate counts distinct `int2`, `int4` or `int8` values, `Custom Scan (SiftAggRows)` takes the rows instead:

- a hash table of groups (the first spelling of a key is shown) with `count`, `sum` and `avg` per group (PostgreSQL's own transition and final functions: same types and errors);
- per `count(DISTINCT x)` argument, one set of (group, value) pairs with a bit for every aggregate of that argument that took the row, so `FILTER`s share it. The set is bounded by `hash_mem`; past it the pairs go to a tuplesort of narrow tuples and equal pairs are counted at the end (`Distinct Sets Spilled` in `EXPLAIN ANALYZE`).

The child plan's projection evaluates group expressions, arguments and `FILTER`s, so any expression is allowed. Grouping sets, ordered or `DISTINCT` aggregates other than `count`, and aggregates other than `count`, `sum`, `avg` keep the normal plan. Float sums and averages add in arrival order (the sort adds in id order), so last digits can differ, as between the scan's aggregates and the normal plan's. `sift.agg_rows = off` restores the sort.

**Streaming `count(DISTINCT x)` of an integer column.** When a `count(DISTINCT x)` scan's sets would outgrow `hash_mem` and the column's superblocks hold `x` ranges that overlap little (a column growing with the heap: a time, a serial id, a key loaded in order), the scan visits superblocks in order of zone minimum of `x` and, before each, drops from the sets the values below the minimum of the superblocks still to come: no later row can hold them, so they are not counted twice, and the group's count already includes them. The answer is the plain scan's exact one. The sweep runs in one process and does not spill the group table (a spill would drop pairs from the sets it dumps): when that table would outgrow `hash_mem`, or a unit has rows the zones do not bound (a write buffer, read first), the scan restarts in plain order. `EXPLAIN ANALYZE`: `Distinct Stream Superblocks`, and `Distinct Stream Given Up` after a restart. A column in random order within every superblock, two sets, or a grouping set keeps the plain scan. `sift.agg_dist_stream = off` restores it.

## Workload shapes

Shapes of an event analytics workload (an item table with jsonb metrics, a document with an array of parts, entity and mapping tables) are in the synthetic generator `test/col/workload.sql` and `bench/sql/workload/shapes/g*.sql` (`bench/sql/workload/run.sh build|plans|time`; each file's first line is the shape's status). Regress test `agg_workload_shapes`, fixtures `wl_*`, coverage rows `WL01` to `WL09`. Times: serial plan at 300,000 items, best of 3, with `sift.enable_agg` off and on.

| Shape | Status | Off | On |
| --- | --- | --- | --- |
| g8 `EXISTS` over `jsonb_to_recordset(expr -> 'parts')` of an index expression (was a plan error, `invalid attnum`) | pushed | 1.64 s | 3 ms |
| g8 nested jsonpath `$.parts[*] ? (@.category == "x" && @.sign == "y")` | pushed | 1.52 s | 3 ms |
| g9 / g10 `x = ANY (ARRAY (SELECT ...))`, eight keys | pushed | 116 ms | 2 ms |
| g5 per-item arms, `avg(n::float8)` over a text leaf, count of the items | fused | 2.83 s | 0.07 s |
| g4 a second `count(DISTINCT)` over another column of the row | arms one by one |  | follow-up |
| g7 fused arms for a selective scan (one entity) | not offered |  | follow-up |
| g2 lookup keyed by a column of another joined table | pushed (regression and coverage verified) |  |  |
| g1 `jsonb_exists` / `?` on an object | normal plan |  | follow-up |
| g3 `(doc -> 'k')::int` | normal plan |  | follow-up |
| g6 one string in a typed leaf flags its unit | pushed for `jsonb_to_recordset` columns, the flagged rows read again from the heap; other reads: normal plan, leaf named at `DEBUG1` |  | design note |

What the pushed shapes do:

- **Text-valued leaves (`astext`) in paths and recordsets.** `jsonb_to_recordset(expr -> 'a') AS o(k text)` reads the leaf of a list of objects whose member is declared `astext`. A text literal tests it for equality (`=`, `IN`) only when no number or boolean prints as that text (it does not start with a digit or `-` and is not `true`/`false`), so the leaf text of the number 7 is never taken for the string `'7'`. A number or boolean literal against such a leaf, a strict path and a NOT of a test that may be NULL keep the normal plan (plain traps in `wl_path_parts_*`).
- **`EXISTS` over an index expression's array.** The planner deparses the expression with the query's own range table (the same expression in a sub-select's range table is what raised `invalid attnum`). One element's members are tested together, as for a column.
- **`x = ANY (ARRAY (SELECT ...))`.** An uncorrelated, stable sub-select that cannot raise is an InitPlan; the scan runs it once at start and its rows are the leaf's lookup table (as for a hashed sub-plan: no parallel workers). A sub-select that reads the row, is volatile or may raise keeps the normal plan (`wl_any_array_subselect_*`).
- **Fused arms over a text leaf.** The inner `sum`/`avg` of a float8 expression the scan evaluates (a cast, `CASE`) is accumulated per row like typed leaves. The expression runs for every item the scan reads, so the arms must share one WHERE (the scan's) and have no inner `FILTER`; an arm's own WHERE keeps the arms one by one. The plain cast raises for a non-numeric score, in the scan as in the heap plan.

## Differential suite

`scripts/coldiff.sh` referees the aggregate pushdown: it runs a corpus of plain SQL aggregate queries in one session with `sift.enable_agg = off` (PostgreSQL's plan without sift), again with the pushdown on, compares both results inside the database, and reads the plans to report which queries really used `Custom Scan (SiftAgg)`. Result equality and plan shape are reported separately, so the harness works before the planner exists (both runs then use the same plan; `scripts/coldiff.sh --no-index` is the harness self-check).

```sh
scripts/pgtest.sh install && scripts/pgtest.sh start       # a cluster (SIFT_TMP, SIFT_PORT)
scripts/coldiff.sh                                         # both suites, the scenarios, every variant
scripts/coldiff.sh --suite fixtures --corpus '^s7_'        # the fixture queries whose name matches
scripts/coldiff.sh --suite analytics --rows 200000         # the corpus of bench/sql/analytics on a larger table
scripts/coldiff.sh --bench --suite analytics --rows 1200000 --runs 5
scripts/coldiff.sh --selftest                              # the harness against injected differences
```

`make check` runs the jobs `difftest-col` (small fixtures, a subset of variants and scenarios), `difftest-col-tbl` (fixtures and scenarios on a `sift_table`, `--table`) and `difftest-col-selftest` (`scripts/ci.sh`); the full tier uses larger tables and every scenario. Without the column index (no `sift_col` access method, no `sift_colstub` (the planner-track stub) in a `SIFT_TESTING` build, or a failing `CREATE INDEX`) the suites print `coldiff: SKIP: ...` with the reason and exit 0.

### Suites

| suite | data | queries |
|---|---|---|
| `fixtures` | `test/col/fixture.sql`: an `orders` table of `--fixture-rows` rows (default 4000, `--seed`), hash generated, with NULLs, skews and a fill factor that allows HOT updates; helper tables; edge-case tables (`t_int`, `t_float`, `t_text`, `orders_odd`, `orders_part`) | `test/col/fixtures/*.sql` |
| `analytics` | the generator of `bench/sql/analytics` (`SIFT_ANALYTICS_DIR`, `--rows`, `--stores`), its indexes and `col_index.sql` | its `queries/*.sql` (`--queries GLOB`), the `pg` section of each file |

Each suite has its own database (`sift_coldiff_fix`, `sift_coldiff_ana`; `COLDIFF_DB` is the prefix, `--keep` keeps them) with conventional indexes (primary key, btree, GIN, and a BM25 index on the fixture's `memo` column), then the column index, then `VACUUM (FREEZE, ANALYZE)` (every heap page all-visible). The fixtures' index DDL is in `scripts/coldiff.sh` (`--col-am`, `--col-paths`); the analytics index is the statement in its `col_index.sql` (`--col-index FILE`), with `USING sift_col` replaced by `--col-am` when the stub is the only access method.

### Variants

The reference run is `off`: `sift.enable_agg = off`, `max_parallel_workers_per_gather = 0`. Every other run is compared with it (`--variants`):

| variant | `sift.enable_agg` | parallel workers | `sift.simd` |
|---|---|---|---|
| `off-par` | off | 2 (free parallel costs) | auto |
| `on` | on | 0 | auto |
| `on-par` | on | 2 | auto |
| `on-scalar` | on | 0 | scalar |
| `on-par-scalar` | on | 2 | scalar |

`off-par` checks the tolerance below against PostgreSQL's own parallel plans. Each variant also runs through `EXPLAIN (COSTS OFF)` to record whether the plan has `Custom Scan (SiftAgg)` and `Gather`.

### Random queries (`test/fuzz/diff_fuzz.py`)

A seed gives a small database (`test/fuzz/diff_fuzz_data.sql`: NULLs, duplicates, a `sift_col` index on each fact table, a hash partitioned table, lookup tables) and a script of episodes: a few `UPDATE`, `DELETE`, `INSERT` and `VACUUM` statements (dirty rows, write-buffer rows), then random queries of the pushdown shapes: first-row lookups, two-level aggregates with spines, partition pruning, pre-aggregates (`FILTER` on a cube dimension, `int8` arithmetic, per-group `min`/`max`), jsonb member casts, `EXISTS` over element lists and `jsonb_each` members, `UNION ALL` arms, top-N, and several lookups of one table in every join kind. Each query runs as the reference and as variants (pre-aggregates, scan alone, arms alone, parallel workers, scalar SIMD, no lookup pushdown) and rows are compared by the contract below. A divergence is shrunk (query parts, then preceding statements) to a reproducer file; a dying worker or backend is reported with its query. `scripts/ci.sh` runs fixed seeds (`difftest-fuzz`, seconds), and again with `--table` (`difftest-fuzz-tbl`): the fact tables are `sift_table`s with their jsonb paths declared, the statements include rolled back writes, `VACUUM FULL` and `sift.table_maintain`, and the families that need an option of the column index (`key_cut`, `order_columns`, the point directory) are skipped; `test/fuzz/diff_fuzz_search.sh` runs a long search in capped slices.

### The comparison contract

Each result is materialized in a temporary table and compared by `coldiff.compare` (`test/col/run.sql`):

- columns (names, types, typmods) and row count are equal; a query needs a total `ORDER BY` and rows are compared by position. An analytics file has no `ORDER BY` but a `-- key:` line: both results are sorted by those columns, then by every column as text;
- every type but floats is equal as text: integers, dates, text, booleans, numerics (scale counts: `1.50` is not `1.5`), jsonb;
- `float4`/`float8`: `|x - y| <= 1e-12 + 1e-9 * max(|x|, |y|)` (`--abs`, `--rel`), because parallel workers and SIMD lanes add in another order. NaN equals NaN, an infinity only itself, `-0` equals `0`, NULL only NULL. This is no license for float4: `sum(float4)` adds in float4 in PostgreSQL, so a plan adding in double differs by about 1e-7 relative and fails (query `t_float4_sum`);
- a query that raises (a cast, integer or float overflow) must raise the **same SQLSTATE** in every variant (the message may name another row's value); such a file says `-- error: yes`, and a reference run that fails otherwise is a failure.

A per-order aggregate (`avg(x)`, `sum(x)` of a float under an outer aggregate) adds one order's elements from first to last in double and divides by their count: the value PostgreSQL gets when its plan feeds an order's rows in array order (a hash aggregate over the unnest). PostgreSQL's own plans disagree on last digits (a plan that sorts by order and key adds equal keys in its unstable sort order). A condition on the average (`avg >= 9`) that holds exactly for a decimal sum (8.7 + 9.0 + 9.3 over three) can then count a few more or fewer orders in one plan than another (three of 100 000 orders between the two arms of one query on the normal plan), whatever the scan does; the scan's count is the first-to-last one. A threshold that must be reproducible therefore uses integer values (exact sums) or leaves a margin, never the last digit of a float sum.

A differing result always fails (exit 1) and prints the first differing rows of both; nothing is silenced, and a query without a result (an unexpected error) fails too.

### The shape expectation

A file states the expected plan: `-- expect: agg` (the pushdown must happen once implemented), `plain` (it must not: volatile functions, unsupported or risky semantics) or `any`. Only `on` is checked for `agg`; every `on*` variant for `plain`. A mismatch is a `NOTE` in the coverage table's last column and in the summary line; it fails only with `--strict-shape`. The coverage table has, per query and set: shape, expectation, `equal` or `DIFF`, whether `on` used SiftAgg, reference and `on` times in ms, and the speedup.

### Corpus format

One query per file, as in `bench/sql/analytics/queries`: header lines, then the query. Header lines that are not a tag below are the title.

| tag | meaning |
|---|---|
| `-- shape: S1` | the shape of the query (the name of an analytics file of the form `S1` or `G3` is its shape) |
| `-- expect: agg\ | plain\ | any` | the shape expectation (default `any`) |
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

The sections `--@ pg`, `--@ flat`, `--@ ch` of an analytics file are cut: only `pg` runs. `-- tol:` is ignored (the contract above is the tolerance).

The fixture corpus (about 180 files) covers:

| area | fixtures and cases |
|---|---|
| lookup joins and arms | `j_*` (semi, anti, key expressions, ON filters); per-order arms with sets, gates and general HAVING `s2_per_order_*` / `t_two_level_*`; mapped aggregates `t_mapped_agg_*` |
| report shapes | S1 summary of an object with dynamic keys by key, mapped key and period; S2 summary of an array of objects with per-order pre-aggregation, the three rollup levels as `UNION ALL` and as `GROUPING SETS`; S4 time series at day, week, month and quarter with a date spine; S5 per-store rows and top-N; S6 `jsonb_path_exists` on an array of objects; S7 same-element filter as `EXISTS`, `@>` and `jsonb_path_exists` |
| flat aggregates and quals | `IN`, `NOT IN`, `BETWEEN`, `LIKE`, `OR`, `NOT`, `CASE`, `IS [NOT] NULL`, `FILTER`, `HAVING`, grouping by expressions; NULL-heavy documents, empty sets; BM25 match with an aggregate; a prepared statement with a generic plan; `numeric(p, s)` columns (predicates against constants of every precision, NaN and infinities, keys, expressions, distinct counts) |
| limits and their plain traps one past each | group keys up to 8, rollups and cubes of up to 7 columns (`k_keys_*`, `k_rollup_*`, `k_cube_*`; traps: 9 keys, 9 joined tables, 17 combos); lookup joins of up to 8 tables and 16 combos (`j_joins_*`, `j_combos_*`) |
| generic aggregates | `g_*`: variance family, `DISTINCT` over one or several arguments and with a spill, `ORDER BY` in the call, ordered and hypothetical sets, variadic and stable-fraction forms, `array_agg`/`string_agg`, expression arguments, joined columns, rollups, user-defined aggregates, parallel combine; `x_bool_*`: `bool_and`, `bool_or`, `every` merged from two workers; traps: a volatile transition function, a float read into text with `-0` |
| other traps | numeric division, products past 38 digits, float8 casts, `random()` in the qual and aggregate, `sum(int4)`/`sum(int8)` past the type, integer/float/cast errors, NaN, infinities, `-0`, `sum(float4)`, text `min`/`max` under a non-C collation, JSON values that are not numbers; `DISTINCT ON` (`do_*`: latest and earliest rows, mixed sort directions, expressions, joins, every column type, plain traps); outer joins, a set-returning function in the select list, window functions over the aggregate (`w_*`), `TABLESAMPLE`, `FOR UPDATE`, `SERIALIZABLE` |
| partitioned tables | `pa_*`: pruning at every level, a dropped column before the index, default and unlogged partitions, `ONLY`, partitions without the index or with other options, inheritance, run-time pruning of parameters, stable functions and hash remainders; DML runs through the parent in the scenarios |

### Scenarios

After the fresh run, the fixture subset (`-- scenario: yes`) is compared again after DML that leaves the heap in every state the column index must answer for (`--scenarios`, default all). They are cumulative and only equality is checked:

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

The fixtures `jp_*` read `t_jp`, whose members `s` (text), `n` (int4) and `b` (boolean) take every value of their leaves (`1` and `1.0`, the empty string, non-ASCII text, `2147483647`), a JSON null and a missing key. The table compares an element `e` with a literal `L` of the member's type; a scalar or null element and a missing key count as a missing member:

| member of `e` | `@.k == L` | `@.k != L` | `!(@.k == L)` | `!(@.k != L)` |
|---|---|---|---|---|
| a value equal to `L` | true | false | false | true |
| another value of the type | false | true | true | false |
| JSON null | false | true | true | false |
| missing | false | false | true | true |
| a value of another JSON type | unknown | unknown | unknown | unknown |

Against a `null` literal, `== null` is true for a JSON null only, `!= null` for every present non-null member, and under `!()` the other rows. Each form pushed down equals the heap in every variant and scenario; a form that is not provable (the last row, a number against a float leaf, strict `!()`, `jsonb_path_match`) is a `plain` fixture. A member the leaf cannot hold (an array or object in a number leaf, a number out of range) keeps the normal plan, as do older units or write buffers with uncertain kinds for tests that need complete kinds (guaranteed by column minor 6; REINDEX or VACUUM FULL restores the guarantee).

### An element against a column of its row

The fixtures `eo_*` read `t_eo`: members `q` (int4), `q8` (int8), `f` (float8) of its array against columns `r4`, `r8`, `rf` of the same row, with NULL columns, a NaN and a `-0`, a JSON null and a missing member, empty arrays and a NULL document. Element `j` of row `i` holds the value `r4` has in row `i + j - 1`, so every row has elements equal to its neighbours' columns, plus rows that must not match. An element is compared with its own row's column (`= <> < <= > >=`, either side first), NULL on either side is unknown, and the element's other tests (`AND`, `OR`, `IS NULL`, a literal) apply to that element. An expression of the column, a column of another type class (int4 member against a float8 column), a non-constant `LIMIT`, `OFFSET` and a second array keep the normal plan (`eo_trap_*`).

### Bench mode

`--bench [--runs N]` runs only the first pass (no scenarios) with the median of N runs (default 5, after a warm run) of reference and `on`, and prints go/no-go lines: K4000 queries that must push down (`expect: agg`, and the analytics corpus's S shapes) at least 20 times faster than the reference; K10 and K400 ones that push down not slower (0.95). A missed target is printed and fails the run only with `--strict-shape`. Times include the result's temporary table.

### Self-test

`scripts/coldiff.sh --selftest` runs a subset of the fixture corpus plain against plain (must pass), then with `--mutate KIND`, which perturbs every `on*` result as a pushdown bug would. These must fail: `int` (+1), `float` (relative 1e-6, beyond tolerance), `text`, `null`, `rows` (last row lost), `dup` (first row twice), `order` (rows 1 and 2 swapped, seen by a position-compared query), `type` (a wider column type), `cols` (an extra column). `float-tiny` (relative 1e-11, inside tolerance) must pass; a mutation applied to no result fails the self-test.

## Plan-choice gate

`scripts/plangate.sh` guards the choice between the normal plan and `Custom Scan (SiftAgg)`. It builds the tables of `test/plan/gen.sql` (events, dims, facts, a month spine, a partitioned copy; `--rows`, default 30000, and `--seed`; rows are hashes of their number and `ANALYZE` reads all of them, so statistics repeat) and runs each shape of `test/plan/shapes.sql` three ways: native (`sift.enable_agg` off), chosen (the planner's own choice) and scan (table scans off, so the pushdown runs where the query allows). Results must agree by the differential suite's contract (floats within 1e-12 + 1e-9 of the larger), the plan chosen must be the expected one, and the scan-over-native planner cost ratio must stay in the shape's band, so drift shows before it flips a choice. For each shape the table prints plan chosen, expected plan, ratio and band, native and scan times (median of `--runs`, default 3) and a verdict: `win`/`lose` when the scan is chosen and faster/slower by more than 1 ms and 20%, `miss` when it is not chosen but would win by as much, else `tie`. A wrong result, unexpected plan or out-of-band ratio fails; so does a `lose`, unless `--warn-time` (the full tier runs at 50000 rows with it: hosted runners are noisy). The quick tier runs without timings (`--runs 0`). Expectations hold from 20000 to 50000 rows; a shape whose plan changes inside that range expects `either`.

`--lane tuned` runs the same shapes with a tuned planner profile (`seq_page_cost` 100, `random_page_cost` 4, `max_parallel_workers_per_gather` 8, `hash_mem_multiplier` 2, `work_mem` 4GB, `jit` off). Results must agree and a lose still fails; the plan and cost band are the lane's own, in `test/plan/bands.tsv` (the tuned profile prices a sequential page at 100, which moves every ratio by a regime: the stock bands do not hold), so a shape taking another plan or a ratio leaving its band fails on PostgreSQL 18, as in the stock lane (a warning on another major; quick and full tiers run the lanes: `plan-gate-tuned`, `plan-gate-table` and `plan-gate-table-tuned`). A band `any` in a lane's rows marks a scan path that exists at some sizes only (`h02` in the tuned lane): nothing to hold, nothing to miss; `--update` rewrites it, so restore it by hand. `seq_page_cost` above `random_page_cost` is the setting that moves the picks: it prices a scan's heap check at the sequential price, which can refuse the scan where the normal plan is many times slower.

`--table` runs the same shapes on the tables as `sift_table` (`test/plan/table.sql` drops the `sift_col` indexes, rewrites every table and partition into the table access method and declares the typed `doc` paths): the results must agree as ever, the plan and cost band are the lane's own in `test/plan/bands.tsv` (`table`, and `tabletuned` with `--lane tuned`), so a plan or ratio that leaves them fails on PostgreSQL 18. The shapes that stay on the normal plan there are the partitioned ones (`h01`, `h03`, `h04`, `h05`: a partitioned `sift_table` has no pushdown yet) and the three month-spine lookups (`f01`, `f03`, `f04`: the table's lookup join loads its rows slowly).

To add a shape, append three lines to `test/plan/shapes.sql`: `-- name: x`, `-- expect: new` and the query (a total `ORDER BY` with a `LIMIT`); run `scripts/plangate.sh --update` and read the diff: it writes the plan chosen and a band of a factor of 2 around the measured ratio (refreshing every shape's bands; a plan stays unless `new`). In another lane the same flag rewrites that lane's rows of `test/plan/bands.tsv` (the plan the lane chooses, `either` stays, and the band around its ratio): do it when a cost change moved the ratios on purpose, and say in the commit which change it was. A band is a factor of 2 around the ratio at the default size (30000 rows); a shape whose ratio moves with the rows (the gate holds from 20000 to 50000, and the full CI tier runs 50000) takes instead the smallest and largest ratio measured at 20000, 30000 and 50000 rows, divided and multiplied by 1.3 (`n01`, `n02`, `n05` in the tuned lane, `n05`, `m02` in table tuned): rerun `--update` at the three sizes and keep those rows by hand.
