# Changelog

This file is the changelog of the PostgreSQL extension. The SQLite extension (`sift` for SQLite, artifact `sift-sqlite`) has its own: [sqlite/CHANGELOG.md](sqlite/CHANGELOG.md).

## 3.2 Unreleased

- Added: pg_sift: SiftAgg runs an `EXISTS` over the elements of an array whose test reads a lookup per element (`LEFT JOIN LATERAL (SELECT .. FROM mapping WHERE mapping.k = elem.k AND mapping.kind = dim.kind LIMIT 1)`) or a column of a joined table (the subject of a check by the kind of its attribute): the lookup runs once for each distinct value of the columns it reads, a row passes when some element does, and a row whose elements and joined value repeat is not tested again. `NOT EXISTS`, `EXISTS` in an aggregate `FILTER` and expressions that can raise keep the normal plan.
- Changed: pg_sift: the arms of a `UNION ALL` that look a name up with the same correlated `LIMIT 1` sub-select (a mapping table joined on values of the unnested list) run in one pass over the index although they read other columns of it or ask for other expressions of it (one the mapped name, one also its sub-name): the scan, the lookups and the rows read again from the heap are shared. An arm with another kind of join (a `LEFT JOIN` without the `IS NOT NULL` of the others) or another condition keeps its own pass.
- Changed: pg_sift: the arms of a `UNION ALL` that read the same rows for the same aggregates run in one pass over the index also when they join a table (a lookup join or a combo the arms share), read leaves of their own, or have more than four keys together (a key two arms share counts once; up to 31 distinct keys): the rows read again from the heap for values the index cannot store are read once for all the arms, and `EXPLAIN ANALYZE` shows their count on every arm.
- Added: pg_sift: SiftAgg evaluates `jsonb_exists(doc, 'k')` of the document whose list (`jsonb_to_recordset(doc->'items')`, `jsonb_array_elements(doc->'items')`) it unnests, in keys, `CASE` branches and filters, from the JSON type of the member `k` the index holds (a JSON `null` member exists); it stays with PostgreSQL elsewhere, where an array or a string document can hold the key.
- Fixed: pg_sift: SiftAgg reads again from the heap the rows of `jsonb_to_recordset` whose document holds a value the index cannot store (a number written as a string) also when the query reads the column inside an expression (a `CASE` key, a bucket of the value): the query kept PostgreSQL's plan before.
- Fixed: pg_sift: SiftAgg joins a sub-select whose plan PostgreSQL gave a condition pushed down into it (the run condition of a ranking window: `IN (SELECT .. FROM (SELECT .., row_number() OVER (..) rn ..) r WHERE rn <= 3)`) instead of keeping the normal plan: the plan of the sub-select enforces the condition and does not output its column, and the scan no longer tests it again.
- Added: pg_sift: SiftAgg runs aggregates whose derived table has a `WITH` of its own (a ranked top-N over an aggregate, `UNION ALL` arms over one CTE): the CTEs of a derived table that reads nothing around it are lookup plans of the scan, and a CTE of it that several aggregates read is planned inline like the statement's own.
- Added: pg_sift: SiftAgg runs an `EXISTS` over the elements of an array that is written with a sub-select PostgreSQL pulls up, and the conditions of the nullable input of an outer join (a date spine `LEFT JOIN` a sub-select or CTE) that test its arrays (an `EXISTS`, `jsonb_path_exists`) and the aggregate filters that read a test computed there: the `ON` conjuncts that read only the scanned table filter its rows before matching, and a test guarded by the absent input is read through the guard when it is not true for an absent input.
- Changed: pg_sift: SiftAgg prices the filters and expressions on the values of an object member or array element once per distinct value (PostgreSQL's default number of distinct values, not once per row), so a per-key score aggregate with `FILTER (WHERE value >= 8)` is chosen when the scan is cheaper.
- Fixed: pg_sift: SiftAgg no longer fills a table of the whole range of an integer group key (an entity id of a table ordered by another column, whose units hold few values far apart) before it uses the hash table: such a key now costs the groups it has, not its range, which made a per-key aggregate several times slower than PostgreSQL.
- Fixed: pg_sift: SiftAgg aggregates `GROUPING SETS` / `ROLLUP` over group keys computed inside the nullable input of an outer join (a date spine `LEFT JOIN` a CTE or sub-select that derives the keys) on PostgreSQL 18: the keys are the expressions of the input, NULL for the rows no input row matched, instead of the query being left to PostgreSQL.
- Fixed: pg_sift: SiftAgg counted the preserved row of an outer join that no row matched as one value of `count(DISTINCT key)` when the key is the unique column of the scanned table (a date spine `LEFT JOIN` an empty selection answered 1 where PostgreSQL answers 0).
- Changed: pg_sift: SiftAgg prices a lookup join without keys (a date spine joined on an expression) by the distinct values the scan's own conditions leave and by the rows a set-returning spine makes, not by every distinct value of the table and PostgreSQL's 1000 rows a call. Aggregates over a date spine and correlated arrays of a joined row pick the scan on their own costs where it is faster.
- Fixed: pg_sift: SiftAgg no longer reads a value the index cannot store (a string, an object or an array in a `float8` path, a scalar where an array is declared) as NULL when the query takes a key of an object leaf with free keys (`(metrics ->> 'k')::float8`, `(metrics -> 'k')::float8`) or `jsonb_array_length(doc -> 'l')`: such a query keeps the normal plan, so a numeric string converts and anything else raises PostgreSQL's own error, and a plan made before the value was written raises the scan's error. In the columns of `jsonb_to_recordset` a string `NaN` or `Infinity` converts instead of raising the scan's overflow error (the string `-0` still raises it).
- Added: pg_sift: an expression key or predicate that reads one column twice under binary-compatible types (the column, and the column relabeled for a comparison with a `char(n)` column of an `IN (SELECT ..)`) runs in SiftAgg.
- Added: pg_sift: a correlated child of SiftAgg (a `UNION` arm, a lateral sub-select) can read the value of a `jsonb_each` member and cast it: the child is given the jsonb the stored number stands for, runs only for the members a row reaches, and raises PostgreSQL's own cast error. `x IS TRUE` over an existence test (`jsonb_path_exists`, `@?`) runs in the scan.
- Added: pg_sift: a lateral sub-select that PostgreSQL leaves in place because its query is read on the nullable side of an outer join (`dates LEFT JOIN cte`) is pulled up inside its own query level when that gives a SiftAgg plan that costs less, so `jsonb_each(doc)` joined to a mapping table runs in the scan instead of as a correlated child of the whole document.
- Added: pg_sift: `LEFT JOIN LATERAL jsonb_each(doc) r ON r.key = 'k'` (a constant key) runs in SiftAgg on PostgreSQL 18 as the member `k` of each row, with its NULL columns for the rows without it and the casts of its value (also inside a correlated child) raising PostgreSQL's own errors.
- Fixed: pg_sift: SiftAgg no longer treats `LEFT JOIN LATERAL jsonb_each(..)` / `jsonb_array_elements(..)` as an inner join: the rows without a member (NULL columns) were dropped and the ON clause was ignored. Such queries keep the normal plan.
- Added: pg_sift: SiftAgg joins a lateral sub-select, a `UNION` / `UNION ALL` of arms, `generate_series`, `unnest` of an array column or a set-returning function that reads the joined row. PostgreSQL's parameterized plan of it is the child, run once per distinct input, with duplicates and NULL-extended rows as in the normal plan. A call of a document column that cannot raise (`jsonb_exists` or `?`, `->>`, `jsonb_typeof`) takes the index expression column of that call as the parameter. A child that can raise (a division, a cast of text, a sub-select) is joined only when the query joins nothing else, and one that reads a column of a query above the aggregate's own never is, so no statement fails only under the scan. A `UNION ALL` / `UNION` of sub-selects, a sub-select of a statement-level CTE and a `WITH` inside the sub-select are lookup plans too.
- Added: pg_sift: SiftAgg reads the columns of `jsonb_to_recordset` from an index that flags values its typed leaves cannot store (a numeric string in a number path, a document that is an array or a scalar, a number in a text path). The flagged rows are read again from the heap and read as the function reads them, so the answer, errors included, is the normal plan's; `EXPLAIN ANALYZE` shows `Rows Read Again From The Heap`. No format change.
- Added: pg_sift: an inner or semi lookup join on an integer or date leaf tests the leaf against the keys of the loaded table before tuples are made (`sift.agg_join_filter`, `Join Key Filters` in `EXPLAIN ANALYZE`): units the keys' range misses are not read and the other rows are tested as a set instead of through the combo cache. A selective join that returns a column of the table runs about 2 times faster (3.0 against 6.4 ms at 500,000 rows).
- Changed: pg_sift: a combo of one small key finds its cached entry by array index instead of a hash probe and `memcmp`.
- Added: pg_sift: `UNION ALL` arms of one statement that join the same table with the same columns and conditions load it once (`sift.agg_share_lookups`, `Shared Lookup Tables`); nothing is kept between statements.
- Added: pg_sift: SiftAgg joins a lateral sub-select, a `UNION` / `UNION ALL` of arms, `generate_series`, `unnest` of an array column or a set-returning function that reads the joined row. PostgreSQL's parameterized plan of it is the child, run once per distinct input, with duplicates, NULL-extended rows and errors as in the normal plan. A call of a document column that cannot raise (`jsonb_exists`, `->>`, `jsonb_typeof`) takes the index expression column of that call as the parameter. A `UNION ALL` / `UNION` of sub-selects, a sub-select of a statement-level CTE and a `WITH` inside the sub-select are lookup plans too.
- Added: pg_sift: hashed `IN (SELECT ..)` / `NOT IN (SELECT ..)` tests inside group keys, predicates and aggregates of an outer join's nullable input, row-valued tests and tests of an `int8` column run in SiftAgg (the scan asks the sub-plan for each tuple that reaches it).
- Fixed: pg_sift: grouping by more than eight keys with the two-level aggregate no longer overflows its key mask.
- Fixed: aggregate null-input markers cannot alias PostgreSQL executor parameters; preserved-input expressions can be explained on PostgreSQL 17. Predicate-only lookups account for testing every candidate row.
- Added: grouping expressions and aggregate FILTER predicates can read up to eight scalar inputs. Expression caches store and compare every input, with storage sized to the actual tuple width.

- Added: aggregate FILTER predicates over lookup columns preserve duplicate matches and NULL extensions, and evaluate expressions only after all joins select their input.

- Fixed: aggregate planning ignores unique LEFT joins PostgreSQL has removed, avoiding a planner crash. Preserved buckets can contain several dependent lookups; empty buckets retain NULL values for inner expressions and constants.

- Fixed: VACUUM relocation invalidates cached write targets when each replacement is published, including when maintenance later errors before truncation; failed unpublished copies return their allocations and follow maintenance backoff.

- Added: Homebrew formula in the source repository for PostgreSQL 17 and 18, tested on macOS arm64 with BM25, extension upgrade and column aggregate smoke tests; bitmap dispatch uses the existing scalar kernels on non-x86 CPUs.
- Added: pg_sift: `col @@> ANY (array)` is scanned as the OR of its elements, scored and ranked like `col @@> 'a OR b'` (`sift.score()`, `ORDER BY sift.score(...)`, `LIMIT`, `count(*)` and the filters work with it); a NULL element is skipped and a NULL or empty array matches no rows. Measured against the bitmap scan it replaces: `count(*)` 1.6 to 54 times faster, `LIMIT 10` and `LIMIT 1000` 1.5 to 130 times faster (0.1 ms slower on queries that take under 0.2 ms), the full match list equal or up to 2 times faster, planning under 0.6 ms at 100 elements. A list with no score or order that is estimated above 100,000 rows (after a `LIMIT`) keeps the bitmap scan, which was as fast or faster there.
- Changed: column format minor 6 guarantees complete JSON kinds for declared paths, including element leaves. This fixes wrong answers after merging a minor-3 segment into minor 4: JSON null could become a missing key. Old minors 3 through 5 and legacy write buffers remain readable; queries needing kinds those inputs cannot prove keep the normal plan, including partition parents. REINDEX and a sift_table VACUUM FULL reconstruct the kinds from the documents. No extension SQL changes are needed.
- Compatibility: a 3.1 library refuses a minor-6 column segment with a format error and REINDEX hint. Install 3.2 on every reader before writing minor 6. `sift.col_format_minor = 4` keeps new narrow segments readable by 3.1; 3 keeps the 3.0 layout, while wide leaves still require at least minor 5. Schemas without JSON paths keep their previous minor.
- Fixed: VACUUM returns internal free space of `sift_col`, `sift_table` and `sift` to the operating system by moving tail segments into existing free runs before truncating. Relocation preserves the stored format, reserves every destination below the planned end, and shares one write allowance across all moves. `vacuum_truncate = false` disables it; a conflicting reader skips it without waiting.
- Changed: `sift_col` VACUUM promotes sufficiently filled open write buffers and continues merging within `sift.vacuum_rewrite_limit` (default 128 MB). The same setting separately caps page relocation; zero retains the first column merge and ordinary free-tail truncation while disabling relocation. No on-disk format change.

- Added: pg_sift: SiftAgg answers DISTINCT ON over indexed columns. The first row of each group by the ORDER BY is kept while the rows pass, with no sort, in the leader and the workers, when the ORDER BY holds the columns of a unique `NOT NULL` index; a tie-break that is not unique, a volatile or array value and groups that do not fit the hash memory keep the normal plan (`docs/AGG.md`). Test `agg_distinct_on`; fixtures `do_*`.
- Added: pg_sift: SiftAgg aggregates over the preserved side of outer joins. `RIGHT JOIN`, `FULL JOIN` and a `LEFT JOIN` whose right side is the indexed table keep the rows of the other table that nothing matched, as one row of NULLs in the groups; a scan with such a join runs without workers.
- Added: pg_sift: SiftAgg pushes down ctid range predicates. `ctid` `=`, `<`, `<=`, `>`, `>=` and `BETWEEN` against a constant TID, at the top level of the `WHERE`, read only the superblocks of the range and test the rows of its first and last block (HOT chains included); `<>`, expressions, `OR`, lists, partitioned tables and top-N keep the normal plan (`docs/AGG.md`).
- Added: pg_sift: SiftAgg on partitioned tables whose partitions carry the same sift_col index. The partitions left after pruning are read as one source (each through its own index, whatever its attribute numbers, with parallel workers over all of them) and `EXPLAIN` lists them; a partition without the index or with other options, an inheritance parent, `ONLY`, and a parameter that prunes at run time keep the normal plan. Test `agg_partitions`; fixtures `pa_*`.
- Added: pg_sift: SiftAgg pushes down `NOT IN (SELECT ...)` and uncorrelated `IN` sub-selects under `OR`. PostgreSQL's hashed `SubPlan` runs once as the lookup table of the column, so the NULL rule is the normal plan's; a correlated, volatile or raising sub-select keeps the normal plan, and the scan runs without workers. Test `agg_subplan_lut` and the `t_not_in_subquery*` coldiff fixtures.
- Added: pg_sift: sift_col indexes inet, cidr, macaddr, macaddr8, timetz, bit and varbit columns. A value is stored as the image of the datum and `Custom Scan (SiftAgg)` reads it through the dictionary of datums, so keys, `count(DISTINCT)`, `min`/`max` (`inet`, `cidr`, `timetz`), conjuncts, `OR`/`CASE`/`FILTER` and expressions such as `host(ip)` are PostgreSQL's own functions on the value (`10.0.0.1/24` and `10.0.0.1/32`, or `12:00+01` and `11:00+00`, stay different values). `GROUP BY` a `bit` or `varbit` column keeps the normal plan (PostgreSQL has no hash operator class for them). `point`, `tsvector`, `xml` and `json` stay refused at `CREATE INDEX`. The column format minor is still 5; the new type tags 20 to 22 are in the same unreleased schema. Tests `col_agg_types33` and the `n_net_*` coldiff fixtures.
- Added: pg_sift: `sift.score()` compared with a constant in `WHERE` (`>`, `>=`, `<`, `<=`, either side; a constant, a parameter or a stable expression of them) filters the ranked scan: the scan tests the cutoff on the score it computed, so the rows equal those of a subquery that filters on the score, with `LIMIT`, `OFFSET`, `ORDER BY` and prepared statements. Any other use of the score in `WHERE` (`OR`, `=`, `<>`, an expression of the score) is still an error. Test `rank_score_filter`.
- Added: a `sift_col` index accepts `numeric(p, s)` of any precision (and scale up to 18 or beyond, or negative), unconstrained `numeric`, `money`, `interval`, `time`, enum, `uuid`, `bytea`, array, range and multirange columns. A value is stored as ordinary leaves of the index: a `numeric(p, s)` of 19 to 38 digits as two 128-bit lanes, an unconstrained or larger `numeric` as a canonical image whose byte order is the numeric order (`1.0` and `1.00` are one value, NaN and the infinities sort as PostgreSQL has them), `money` as an `int64`, `interval` as its total (months, days and microseconds in PostgreSQL's comparison order), `time` as microseconds, an enum as its OID (its order is read from the catalog when a query runs, so `ALTER TYPE ... ADD VALUE` after the index was built is safe), and `uuid`, `bytea` and the other varlenas as the image of the value. `Custom Scan (SiftAgg)` pushes down, for these columns, a `WHERE` conjunct (`= <> < <= > >=`, `BETWEEN`, `IN`, `NOT IN`, `IS [NOT] NULL`, decided on the stored lanes for the numeric, money and interval ones; `Wide Filters In Source` in `EXPLAIN ANALYZE`), `OR`, `NOT`, `CASE` and `FILTER (WHERE ...)` trees, `GROUP BY` and `count`, `count(DISTINCT)`, `min`, `max`, `sum`, `avg` and expressions (casts, `n + 1`, `IS DISTINCT FROM`, `= ANY`), by calling PostgreSQL's own functions on the value, so the rows, their text and the errors are the normal plan's (every shape is a coldiff fixture and a regress check). A group of equal values spelled differently (`1.0`/`1.00`, `1 mon`/`30 days`) shows the first spelling seen, as a serial hash aggregate; a key or an aggregate over such a column runs without parallel workers, and the planner keeps the normal plan for a column with a distinct value per row. `point`, `tsvector`, `xml`, `json` and the other types without a leaf form are still refused at `CREATE INDEX`. Before, `CREATE INDEX` refused all of these types. Column format minor 5 (`SIFT_COL_FORMAT_WIDE`, numbered in `docs/FORMATS.md`): a segment is written as minor 5 only when its schema has such a leaf, indexes without JSON paths keep writing minor 3 or 4 and stay readable by 3.1, and a 3.1 library cannot open an index that has one of these columns (drop it first). `sift.col_verify` checks the lanes, the image and the interval total against the heap; `sift_table` stores these types as before (and now also `numeric(p, s)` with a scale over 18). Tests `col_index_wide`, `col_format5`, `col_agg_wide`, `col_agg_wide_keys`, `col_agg_types32`, the `w_*` and `m_*` coldiff fixtures and the `colwide` fuzz target.
- Added: top-N scans accept `LIMIT`/`OFFSET` parameters of generic plans and an OR of searches on one index. `ORDER BY .. LIMIT $1` keeps the top-N plan (the scan reads the value when it starts: up to 100000 rows in a heap, `NULL` or more rows in a sort of the matches that still reads the heap only for the rows the `Limit` takes), and `WHERE col @@> a OR col @@> b ORDER BY .. LIMIT n` takes the union of the matches of the arms.
- Added: pg_sift: sift_table returns the row's xmin, xmax, cmin and cmax. They read zero before; now `xmin` and `cmin` are the stored versions of the row (write buffer record or the version section of its column group; `FrozenTransactionId` once frozen) and `xmax` and `cmax` those of its last delete, update or lock tombstone (0 if none). A row locked by several transactions shows the last locker, not a MultiXactId. Tests `tbl_sysattr`, the isolation specs `tbl_sysattr` and `tbl_sysattr_groups` and the `t_sysattr_xmin` coldiff fixture.
- Added: a set filter sent as one array parameter (`dim_a_id = ANY($1::int4[])` of a prepared statement on a generic plan) takes `Custom Scan (SiftAgg)` by default. The planner cannot see how long a parameter array is and assumed 10 elements, which kept the bitmap scan at any size unless the caller switched index and bitmap scans off; the paths that fetch the rows of such a set one at a time are now costed for `sift.agg_param_set_size` elements (default 1000, 0 gives the old guess back), only for a query that can run as `SiftAgg`. 3M events, a 12-month window, `count` and `avg` per channel, generic plan: 4.8 / 5.6 / 10.4 ms at 1,000 / 10,000 / 100,000 members against 10.3 / 32.4 / 100.5 ms before and 6.0 / 17.8 / 129.8 ms for the same set as a literal `IN` list; a set of about 100 members is 2.3 ms slower (use a custom plan or `SET sift.agg_param_set_size = 10`). `docs/USAGE.md` shows how a client binds the `int4[]` (libpq, Sequel). Test `agg_set_array`.
- Fixed: the overflow bitmap buffers of `SiftAgg` and `sift_table` reads were sized in 64-row words, not in the 512-row units the readers fill: a numeric leaf with an overflow bit or NaN in a unit that did not end on 512 rows wrote up to 56 bytes past the buffer (found by the memory-context checks of a PostgreSQL built with assertions).
- Fixed: a `LIKE 'prefix%'` on a text column of a `sift_col` index, and a comparison on an unconstrained `numeric` one (stored as images), was turned into a lookup table over the whole dictionary of its segment again for every unit and every range of the scan, so `Custom Scan (SiftAgg)` took time in proportion to units times dictionary entries. On 2 000 000 rows with 200 000 distinct keys `LIKE 'key-12%'` took 171 ms (3.1.0; 73 ms for the normal plan); on a million distinct `numeric` values `n > c` took 0.5 s and `n BETWEEN a AND b` 2.3 s against 0.06 s for the normal plan. The test is now made once per segment and predicate: 10 ms, 17 ms and 33 ms (PostgreSQL 18, 2026-10-20). The planner also prices the dictionary of such a numeric conjunct. Test `col_index_dict_tests` (`tests_made` of `sift._test_col_counters`).
- Fixed: `col NOT IN ()` / `col <> ALL('{}')` with an empty array parameter or constant did not keep the rows whose `col` is NULL in `Custom Scan (SiftAgg)` (the condition is true for every row); found by `agg_set_array`.
- Fixed: `col = ANY(array)` / `col IN (..)` on an integer or date leaf returned every row of a unit instead of the matching ones when the list had gaps (or an empty, `NULL`-only or very wide list) and the unit's values lay inside its range: the unit was answered from its pre-aggregates as if the range were the set. The pre-aggregates now answer such a unit only when the list holds every value of its range (`PredState.in_dense`); a `NULL` element no longer drops the range test that prunes units. Test `agg_set_array` (it recorded the wrong rows for an empty array and one out-of-range query before).
- Changed: `col IN (..)` / `col = ANY($1)` / `NOT IN` on an `int8`, `timestamp` or `timestamptz` leaf is tested natively like an integer leaf: a byte table over the range of the list (up to 4 MB), else a hash table of the values, with the same zone pruning, pre-aggregate and in-source filter rules. It was a PostgreSQL expression evaluated once per distinct value of the column against the array searched linearly, which made an array parameter of 1,000 to 5,000 `int8` keys on a 200,000-key column 20 to 450 times slower than the normal plan once the generic plan took `SiftAgg`. A `text` array parameter (and any other expression on a text leaf) now hashes the array once instead of searching it per distinct value (3,100 ms to about 25 ms for 1,000 keys). `docs/USAGE.md` lists the key types the array form pushes down (`uuid` and `bytea` cannot be indexed).
- Added: `Custom Scan (SiftAgg)` answers a semi or anti join (`IN (SELECT ..)`, `EXISTS`, `NOT EXISTS`) on an integer or date leaf from a byte table over the keys of the joined table, built when the scan starts (`sift.agg_semi_lut`, default on; `sift.agg_semi_lut_size`, default 4 MB of table per scan and per worker, wider keys keep the generic lookup join; `Semi Lookup Tables` and `Semi Lookup Table Size` in `EXPLAIN ANALYZE`). The table's own conditions are tested once per row of the table, and a key outside the range of the leaf is ignored. Synthetic 3M-event dataset, 12 months, `count` and `avg` per channel: membership `IN` of 1,000 to 5,000 members 6.0 to 7.0 ms (`EXISTS` 5.6 ms and `NOT EXISTS` 9.7 ms at 1,000) and a dimension attribute of 3,300 or 17,000 entities 10.0 to 13.3 ms (with the lookup load below), against 63 to 91 ms on the normal plan and 25 to 36 ms with the generic lookup join. `NOT IN (SELECT ..)` stays on the hashed subplan. Tests `agg_semi_lut` and the `j_semi_lut_*` coldiff fixtures.
- Changed: the planner prices the semi and anti joins of a `SiftAgg` path by the rows PostgreSQL expects the join to keep, so a set filter is chosen over the hash semi join; and a parallel `SiftAgg` is planned for a query whose semi joins PostgreSQL pulled up from a sub-select (the query-wide parallel hazard counted them as restricted; the relations' own flags, as PostgreSQL's grouping uses, decide now).
- Added: the lookup table of a `SiftAgg` join (the table of `IN (SELECT ..)`, `EXISTS`, or an inner join to an attribute table) is loaded with only the rows its own conditions can match. The planner collects the `column = value` conditions of the join (`SiftAggJoin.idxatt` and the rest); the executor evaluates the values once and reads the longest matching prefix of a btree index of the table (an index scan, or heap scan keys on a table without one), and the conditions it cannot push are applied to each row as it is read, so the table keeps only the rows that can match. A group of 1,000 members in a membership table that has a btree on `(group_id, ..)`: the generic lookup join 70.8 to 22.9 ms and the byte table 24.8 to 5.5 ms. The planner prices the load by the rows it expects the condition to keep (`AGG_IDX_ROW`) instead of the rows of the table. `EXPLAIN ANALYZE` shows `Lookup Rows Read`, `Lookup Rows Loaded`, `Lookup Index Scans` and (with `TIMING`) `Lookup Load Time`; `sift.agg_lookup_pushdown = off` loads the whole table as before. Test `agg_lookup_load`.
- Added: pg_sift: `Custom Scan (SiftAgg)` joins to VALUES lists, sub-selects and functions, and aggregates over a ranked top-k. The right side of a lookup join (`JOIN (VALUES ..)`, `JOIN (SELECT .. GROUP BY / LIMIT / DISTINCT / UNION ..)`, `JOIN generate_series(..)`, `LEFT JOIN`, `IN (SELECT .. ORDER BY sift.score(ctid) LIMIT n)`) is run by PostgreSQL's own plan once when the scan starts, shown under the node in `EXPLAIN`, and its rows become the lookup table: duplicate keys multiply rows, NULL keys match nothing, the result is the normal plan's. It stays on the normal plan when the right side is volatile, `ON true` and not lateral, has no equality, is estimated above `sift.agg_lookup_max_rows` (new, default 1000000), is read only by a condition PostgreSQL pushed into it, or is the nullable side of an outer join to the table; no parallel workers. Tests `agg_plan_tables` and the `pt_*` fixtures.
- Added: pg_sift: SiftAgg tests an array element against a column of the same row (`EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) i(qty int) WHERE i.qty = o.tenant_id)`, also `<> < <= > >=` and `NOT EXISTS`): the element vector is compared with the value of the column of each element's row. The column must be a stored column of the same type class as the member; an expression of it, `OFFSET` and a second array keep the normal plan.
- Added: pg_sift: SiftAgg pushes down `EXISTS` over `jsonb_to_recordset` of an array that an index expression returns, `jsonb_path_exists` with nested paths (`$.a[*] ? (@.b == "x")`, `$.a[*].b`) and text-valued (`astext`) members of a list of objects in them, with plain-text equality only (a number or boolean literal, a strict path and a NOT of a test that may be NULL keep the normal plan); about 500 times faster than the normal plan at 300,000 items.
- Fixed: a correlated `EXISTS` over the recordset of an index expression failed at plan time with `invalid attnum N for relation "o"` while `sift.enable_agg` was on: the planner deparsed the expression with the range table of the sub-select. Found by an analytics workload (`test/col/workload.sql`).
- Added: pg_sift: SiftAgg pushes down `x = ANY (ARRAY (SELECT ...))` of an uncorrelated, stable sub-select that cannot raise: the InitPlan runs once and its rows are the lookup table of the leaf (no parallel workers, as for a hashed sub-plan).
- Added: pg_sift: per-item arms under an outer aggregate fuse into one scan when the inner `sum` or `avg` is a float8 expression PostgreSQL evaluates, such as the cast of a text-valued leaf (`avg(n::float8)`); they ran one by one (33 times slower at 300,000 items). The arms must share one WHERE and have no inner `FILTER`.

## Unreleased

- Changed: the join key filter of `SiftAgg` covers `int8` fact keys and integer keys that span more than 4 million values (a hash table of the keys, replacing the sorted array of a wide `IN` list); a join that returns no column of the dimension passes the filtered tuples without a lookup (`Join Key Passes`). 3M-row fact, 200k-row dimension: `int8` key at 50% of the dimension 227 to 31 ms, sparse `int4` keys 211 to 53 ms.

- Changed: `SiftAgg` answers an inner lookup join that returns no column of the joined table (a dimension that only filters, `JOIN dim d ON d.id = f.dim_id WHERE d.country < 30`) from the byte table of the semi join when no key of the kept rows repeats; 100,000 of 200,000 dimension keys over 3,000,000 rows: 76 to 27 ms.
- Changed: a unit answered from its rollup decodes only the dims and measures the query reads (and of a measure only the counts, the sums or the minima and maxima it adds up); the other columns are passed over without being stored. 100,000 documents, 29 units of 1,440 tuples: 17.3 against 24.6 million user instructions for `GROUP BY kind` with `count`, `avg`, and 22.6 against 28.7 for three keys.

- Added: the rollups of the elements of a list answer aggregates with `FILTER (WHERE ...)` on dims (`count(*) FILTER (WHERE band >= 8)`, the shares of a score band): each filter runs on the tuples. A filter on a measure still scans.

- Added: `SiftAgg` answers the integer cast of a jsonb member the index holds as float8 (`(metrics -> 'k')::int`, `CAST(jsonb_extract_path(metrics, 'k') AS bigint)`) in a filter, a group key or an aggregate: the cast runs over the jsonb the stored float stands for, so it rounds as numeric does and raises `integer out of range` for the rows that reach it (PostgreSQL 18). Test `agg_jsonb_int_cast`, fixture `j_jsonb_int_cast`.
- Added: the rollups of the elements of a list answer a `GROUP BY` of a function of one dim (`left(day, 7)`, `upper(kind)`, a `CASE` band of an integer dim, `coalesce`), evaluated once per distinct value of the unit's tuples. Before, any expression key scanned every unit.

- Added: `EXPLAIN (ANALYZE, VERBOSE)` of a `SiftAgg` node that does not answer units from the pre-aggregates prints `Pre-aggregates: <reason>` (the aggregate is `count(DISTINCT)`, has `FILTER`, a group key is an expression, a conjunct tests a measure, a joined table, grouping sets, and so on). Test `agg_rollup_shapes` lists the shapes of an element-level analytics query with their reason and the units answered.
- Changed: `SiftAgg` loads the joined table of a lookup join through its btree for a `column IN (list)` condition (`dim.country IN (3, 17, 41)`) as for `column = value`, when the statistics expect the list to keep a small share of the table; with a 200,000-row dimension a filter on 3 countries loads 10,000 rows instead of 200,000.
- Changed: `SiftAgg`: a joined table that is joined to another joined table by their key (`fact.k = a.id AND a.id = b.id`, inner joins) is probed with the column of the scanned table, so the keys of `b` (a materialized set of ids) prune units and filter rows like a table joined to the fact directly. A selective set reached through a dimension no longer reads every unit.

- Added: SiftAgg lookup keys and expressions may read earlier lookup outputs, retaining duplicate matches and NULL extensions. Materialized CTEs can supply lookup rows through their PostgreSQL plans.

- Added: SiftAgg predicate-only joins to child plans, including overlapping date ranges, and preserved date buckets over a nullable JSON expansion. Duplicate matches and empty-bucket NULL inputs keep PostgreSQL semantics.

- Added: pg_sift: SiftAgg pushes down jsonpath `!=` and constant variables where the result equals the heap's. The same forms push down with `!()`, a `null` literal, `@>` with a JSON null member and a strict path without `!()`; a JSON null member is told from a missing key by the complete kinds of column format minor 6; older units keep the normal plan when their kinds are uncertain. What stays on the normal plan: a literal of another JSON type than the member's, a number against a `float4` / `float8` member, strict `!()`, `jsonb_path_match` / `@@`, `@>` with a float or `{}`.
- Added: `SiftAgg` group by up to 8 columns (7 with grouping sets), 8 joined tables and 16 combos; `bool_and`, `bool_or` and `every` run in parallel workers.
- Added: `SiftAgg` reads the `numeric(p > 18)`, unconstrained `numeric`, `interval`, `uuid`, `bytea`, `inet`, array and range columns of a `sift_table` through a dictionary of the values, as a `sift_col` index does: keys, `count(DISTINCT)`, `min`, `max`, `sum` and WHERE conjuncts (also under `OR` and `FILTER`) of those columns and of `money`, `time` and enums run in the source. A `money`, `time` or enum conjunct or key of a `sift_table` raised `XX000` `unexpected operand type` before.
- Added: the jsonpath `!=` and `!()` shapes that tell a JSON null from a missing key run on the declared paths of a `sift_table` too.
- Added: a query that writes the expression of a stored generated column of a `sift_table` (`GENERATED ALWAYS AS (date_trunc('month', ts)::date) STORED`) reads the column as a key, a filter or an aggregate argument, with its pre-aggregates, like an index expression of `sift_col`: a monthly sum over 300,000 rows from 1.4 ms to 0.2 ms.
- Added: `SELECT sift.table_set_paths('t', 'doc', 'doc->ch text, doc->w int4, doc->items[]{k int4, p float8}')` declares typed jsonb paths of a `sift_table` (the `paths` grammar of a `sift_col` index): promotion, merge and `VACUUM FULL` write a leaf per path next to the exact bytes, and `->>`, `->`, casts, `jsonb_to_recordset`, `jsonb_array_elements` and `jsonb_typeof` of the path run in `Custom Scan (SiftAgg)` once every segment has the leaves (`VACUUM FULL` after declaring); a row of another JSON type under a path keeps the normal plan for the queries that read it. The declaration is the extension table `sift.table_paths` (dumped with the database, cleared when the table is dropped). No format change; the extension SQL goes from 3.1 to 3.2 (`pg_sift--3.2.sql`, `pg_sift--3.1--3.2.sql`): run `ALTER EXTENSION pg_sift UPDATE TO '3.2'` in every database. Tests `tbl_paths`, `test/t/extension_lifecycle.sh`, `scripts/coldiff.sh --table` runs the fixture corpus on a `sift_table`.

## 3.1.0 - 2026-10-19

Feature release. The extension SQL goes from 3.0 to 3.1: install the new files and run `ALTER EXTENSION pg_sift UPDATE TO '3.1'` in every database. BM25 indexes, `sift_col` indexes and `sift_table` data of 3.0.x keep working across it, with no `REINDEX`; what 3.1 writes afterwards is in newer formats (`docs/FORMATS.md`) that a 3.0.x library refuses. Highlights: smaller BM25 indexes (segment format 3), rollups of the elements of a list that answer any `GROUP BY` over their dimensions and refresh on demand, a `sift_table` write path with per-row versions, field weights and facet counts, the JSON type tag of `jsonb` leaves and calendar group keys, aggregates from the bitmap of a chunk, and an opt-in Vulkan backend (off by default, not part of the packages).

Changes:

- Changed: the opt-in valgrind lane (`SIFT_CI_JOBS=valgrind scripts/ci.sh quick`) runs in a pinned Debian bookworm container (`SIFT_VALGRIND_CONTAINER`) on a host whose glibc has no debug symbols, and `scripts/pg-assert.sh` works without `pg_config`. Tooling only, not part of any gate.
- Added: a group key built of `date_trunc` (second to millennium), `::date` or `extract` (minute to millennium, `dow`, `doy`, `week`) of a `timestamp` column is evaluated by PostgreSQL once per second, minute, hour or day step of the data instead of once per row, and the planner estimates its groups from the column statistics: month-over-month questions on a `sift_table`, which stores no month leaf, now plan as `SiftAgg` by default (`docs/COL-PLAN.md`, calendar keys; `timestamptz` keys stay combos). 1M orders on a `sift_table`, one backend, same rows: revenue per month 244 to 9.1 ms, distinct customers per month 945 ms (a `GroupAggregate`) to 12.7 ms, revenue per month and category 193 to 11.5 ms; with 1 or 3 workers the answer is as fast and no worker is started. Test `agg_bucket`.
- Changed: `UPDATE`, `SELECT ... FOR UPDATE` and a point read of a `sift_table` after a transaction that wrote cost about 1.2M instructions instead of 9M (100,000 rows, a random row, one statement per transaction). The scan that fetches rows by TID was dropped whenever the backend's xmin moved, which every write does; it is kept while the directory has not changed (`test/t/013_tbl_fetch_xmin.pl`).
- Added: three jsonb shapes run in `SiftAgg` instead of the normal plan, without a format change: `jsonb_array_elements_text` over an array of strings (`tags[] text`), the tests `?`, `?|`, `?&` and `@>` on that array, and `jsonb_array_length` of an indexed array as a key, a filter or an argument (100,000 documents: 85 to 2.6, 33 to 1.8 and 42 to 3.0 ms). A number or a boolean among the strings raises the pushdown's usual error (its hint turns the pushdown off); a JSON null where `jsonb_array_length` looks counts as absent, where the normal plan raises an error. Test `agg_jsonb_array`.
- Added: the path type `astext` (`scores.* astext`, `items[]{score astext}`): a leaf that holds what `->>` gives for a string, a number (as `numeric_out` prints it) or a boolean, so a regular expression or any text function on the text of a number runs on the column. Index definition only, no segment change (a 3.0 library reads it as a text leaf). `jsonb_each_text` with a regular expression on the value and `e->>'score' ~ ...` over an array of objects (100,000 documents): 417 to 7.0 and 408 to 6.6 ms. Never used for `@>`, `?`, jsonpath or `jsonb_each`. Test `agg_jsonb_astext`.
- Added: the extension SQL goes from 3.0 to 3.1 (`pg_sift--3.1.sql`, `pg_sift--3.0--3.1.sql`, `default_version = '3.1'`): `sift.col_segments` lists the column format minor of each segment (`format`) and the bytes of its pre-aggregates (`preagg_bytes`), `sift.table_segments` the format. Run `ALTER EXTENSION pg_sift UPDATE TO '3.1'` in every database after installing the files; `sift_col` indexes, `sift_table` data and BM25 indexes keep working across it. Test `col_format4`, `test/t/extension_lifecycle.sh`.
- Added: column format minor 4 keeps the JSON type of every value a `sift_col` leaf could not hold (half a byte per such value, nothing for a chunk without one; `sift.col_verify` checks it against the heap), and `jsonb_typeof(path) = 'number'` (also `string`, `boolean`, `object`, `array`, and `IN` over them) on a scalar leaf of the index runs in `SiftAgg`: the rows of the other types are dropped without the overflow error (100,000 documents, a number among strings: 100 to 2.1 ms; 1,000,000: 396 to 11.9 ms). A kept row whose value the leaf cannot hold, or whose type an index written before 3.1 did not keep (the hint is `REINDEX`), still raises the error of the pushdown. Every segment 3.1 writes is minor 4, which a 3.0.x library refuses (`sift.col_format_minor = 3` keeps writing minor 3). Tests `col_tags`, `agg_jsonb_typeof`, `agg_jsonb_typeof_parallel`.
- Added: the other `jsonb_typeof` shapes run in `SiftAgg` too, on a scalar or `astext` leaf of a `sift_col` index: `<>` and `NOT IN` (a path with no value is NULL and is dropped, as PostgreSQL does), `= 'null'` (a JSON null, which minor 4 now tells from a missing key by a kind bitmap per chunk, only where the chunk has one), `IS [NOT] NULL`, the type as a `GROUP BY` key (the five types, `'null'` and a NULL group), and the type in an `OR`, `NOT`, `FILTER`, `CASE` or function. A query that reads no value of the leaf no longer raises for a kept row the leaf cannot hold. An index built by a prerelease 3.1 has no kinds and answers `= 'null'` and `astext` types with the hint `REINDEX`. Not taken: `sift_table` (one opaque jsonb leaf, needs declared paths) and a guard of the same path's values in an expression. Tests `agg_jsonb_typeof`, `agg_jsonb_typeof_parallel`, `col_tags`, the `k_typeof_*` fixtures.
- Added: column format minor 4 stores a **rollup of the elements of a list** in the pre-aggregates of a unit: the distinct tuples of up to 6 low-cardinality leaves of an array of objects, an array of scalars or a map (the dims) with the element count and, per numeric leaf, the count, sum, min and max. `GROUP BY` any subset of the dims (`GROUP BY e.kind, e.flag` of `jsonb_to_recordset`, `jsonb_array_elements`, `jsonb_each` keys), tests on the dims (`=`, `<>`, `IN`, `IS NULL`, `LIKE`, `OR`) and `count`, `sum`, `avg`, `min`, `max` are answered from the tuples of the units that are all visible (`Units From Pre-aggregates`, `Pre-aggregate Dims`; the others are scanned in the same plan, also by parallel workers). 100,000 documents, 28 units: `GROUP BY kind` 3.1 to 0.12 ms and the two-key `GROUP BY kind, flag HAVING` 3.9 to 0.13 ms; 1,000,000 documents, 276 units: 28.9 to 2.0 ms and 33 to 1.3 ms; the sidecar grows 1.7 kB a unit (+2.0% of the index) and a serial `CREATE INDEX` about 4% (instructions). Count(DISTINCT), percentiles, a test on a leaf that is not a dim and keys of more than 4096 tuples a unit scan as before. A minor 3 segment has no rollups and is read as before; a 3.0.x library refuses a minor 4 segment. Tests `col_preagg_elem`, `agg_preagg_elem`, `agg_preagg_rollup`, `agg_preagg_elem_parallel`, the `pa_elem_*` and `pa_roll_*` fixtures and the crash and replica TAPs.
- Added: the `sift_col` option `rollup` names the dims of the rollups (`WITH (paths = 'items[]{kind text, sub text, flag text, score float8}', rollup = 'items[].kind, items[].sub')`; `rollup = 'off'` disables them) and is checked at `CREATE INDEX` and `ALTER INDEX ... SET`: an error names a leaf that is not declared in `paths`, is not a leaf of an array element or a map value, or is not a boolean, integer, date, numeric or text leaf. Without it the unit takes the leaves of at most a 32nd of its elements (the dim of most distinct values is dropped first) within 2% of its bytes.
- Added: a `SELECT` that scans exact units only because their rollup is missing, has other dims or lacks a measure queues the segment (`Rollup Refresh Queued` in `EXPLAIN ANALYZE`) and a background worker rewrites it with the options the index has now; the query writes nothing, its result is the same, a hot standby never queues, VACUUM, merges and writers are not blocked and a crash leaves the old segment. 1,000,000 documents: every unit answers from the rollups 1.9 to 3.1 s after `ALTER INDEX ... SET (rollup = ...)`. `sift.col_rollup_on_demand` (on), the index option `rollup_on_demand` (true) and `sift.col_rollup_worker` (on) turn it off; `sift.col_rollup_state`, `sift.col_rollup_requests` and `sift.col_rollup_refresh(index)` are in the extension SQL of 3.1 (fresh install and `ALTER EXTENSION pg_sift UPDATE` from 3.0). Tests `col_rollup_refresh`, the isolation spec `col_rollup_refresh`, `test/t/col_rollup_ondemand.sh`, `test/t/extension_lifecycle.sh`.
- Changed: `count`, `sum` and `avg` (with or without `FILTER`, ungrouped or grouped by text columns) of an integer or float column or of the elements of an array of objects are aggregated from the bitmap of the rows of a chunk instead of a list of tuples (`sift.agg_dense`, `Dense Chunks` in EXPLAIN ANALYZE; `docs/COL-PLAN.md`): `count(*) FILTER (WHERE ..)` per flag over 387,593 elements costs 10.9M instructions instead of 34.0M, `GROUP BY` kind with `count` and `avg` 26.9M instead of 41.1M. The sums add in the same order, so the results do not change.
- Docs: `docs/COL-SHRED.md` measures jsonb arrays and objects on `sift_col` against plain PostgreSQL, GIN and ClickHouse arrays (bench files in `bench/sql/jsonb`) and proposes paths for `sift_table` and element rollups for the pre-aggregates (column format minor 4); `docs/COL-TAM.md` documents why the file of a `sift_table` keeps the write pages of a load after a first VACUUM on an idle database.
- Changed: the benchmark harness (`bench/bench`) runs by size (`--size smoke|quick|standard|full`, `bench/bench --list`), caches the cells of the other engines by key and host, resumes an interrupted run (`bench/bench resume`, exit status 75), measures the single-client place cells in one persistent libpq process and compares two result files with a bootstrap interval (`bench/bench compare`); `bench/bench final` is the staged release pass and replaces `bench/analytics`. Tooling only, no change to the extension.
- Added (3.1, experimental, opt-in): Vulkan compute for aggregates (`docs/GPU.md`). `make VULKAN=1` builds a backend that runs the grouped and filtered integer counts and sums of a `sift_col` index on a Vulkan device: `count(*)`, `count(col)`, `sum` and `avg` of integer and `numeric` columns, at most four comparisons, one key of an integer or dictionary-coded column with at most 64 values per unit, exact (16-bit halves added in 128 bits, so the result does not depend on the order). Settings `sift.gpu` (`off` by default; `auto` takes grouped aggregates only, `on` also the ungrouped), `sift.gpu_min_rows` (5000000, for `auto`), `sift.gpu_device` and `sift.gpu_cache_mb` (256); they exist and are forced off in a build without Vulkan (one NOTICE, the CPU answers). Clean, all-visible stored units run on the device and the others on the CPU in the same query; the compressed chunks stay on the device between queries (per backend). A device that fails runs the query on the CPU and is not used again in the session (one NOTICE). `EXPLAIN ANALYZE` shows `GPU: units n / total`, the device and the upload, kernel and readback times. Float aggregates, `min`/`max`, text filters and `sift_table` stay on the CPU. The default build, the packages and CI have no Vulkan dependency: `libvulkan` is loaded with `dlopen`, the Vulkan declarations and the SPIR-V are checked in.
- Fixed: a wide wildcard, regex or fuzzy leaf scored 0 next to an optional phrase. In a query such as `/lev.*/ OR "two words"` whose regex (wildcard or fuzzy) leaf has more terms than `sift.max_expansions`, the documents that match the leaf lost its contribution to `sift.score()` (a document matching only the leaf scored 0, one matching both scored the phrase alone), so the ranking differed from the documented score of a wide leaf (one term, tf the number of words it matches). Counts and the set of matches were right. Wide prefixes were not affected. No `REINDEX` is needed.
- Fixed: a comparison of a `numeric(p, s)` leaf with a constant that no stored value can equal or that lies beyond every value (`n = 5.555` in a column of two decimals, `n < 1e20`, `n <> 'NaN'`) returned wrong rows when the units answered from their pre-aggregates (an ungrouped aggregate, or grouped by one column): the test no longer prunes the zones by a range it does not have. Test `agg_numeric_const_cmp` runs every operator against such constants with and without the pushdown, the pre-aggregates, the write buffer and a promotion.
- Changed: a transaction that writes through many commands (a loop of single-row `INSERT`s, `INSERT ... ON CONFLICT`, savepoints and subtransactions, several concurrent writers filling the same write buffers) no longer stops at the 128 entries of the directory. Once the directory is half full, every sealed write buffer becomes a column group whatever wrote its rows: the rows that are final are frozen, the others keep their transaction and command in the group (version vectors, column format 1.4; a run of single-row inserts takes no bits per row). Scans, index scans, bitmap scans, parallel scans, fetches, `SiftAgg`, `ANALYZE` and the index builds read the visibility of each row from them, at every isolation level, so the rows of a running or aborted transaction stay invisible to everybody else, and its own rows follow `cmin` as in a heap. `VACUUM` clears the rows of an aborted writer after the index cleanup, freezes a group once its live rows are committed and older than every snapshot, and the merges keep the versions. A loop of 10,000,000 single-row `INSERT`s into a table with a primary key, in one transaction, takes 58 s with a directory of at most 65 entries and a backend resident size of 189 MB at the default `shared_buffers` (flat from 4,000,000 to 8,000,000 rows; the resident size counts the shared buffers the backend touched); the same load rolled back is gone after `VACUUM` (the relation goes from 134,432 blocks to 7). A write that finds the directory full waits for the promotion in progress before it fails; the error is now for a write buffer that holds a deleted row `VACUUM` has not seen or a speculative insertion in progress. The load speed of a 1,000,000-row `COPY` and the cost of a single-row `INSERT` are unchanged. Tests `tbl_writepath`, `tbl_writepath` (isolation) and `014_tbl_writepath`. See `docs/COL-TAM.md`, sections 3 and 7.
- Changed: the on-disk format. A `sift_table` writes its column segments in the format `sift.col_format_minor` says (4, or 3 to keep them readable by 3.0), and a segment with rows that are not final is column format 4 whatever it says, with a version section after the superblock directory; the metapage format of a table goes to 1.3 when the first such segment is published, and is never lowered. A 3.0.x library refuses such a table with a format error (it would read the rows of a running or aborted transaction as final); `VACUUM FULL` under `sift.col_format_minor = 3` rewrites the table in files a 3.0.x library reads again (`test/t/old_lib_refusal.sh`). Groups that 3.0.1 tagged with their writer are read as constant versions, frozen by `VACUUM` when committed and dropped when aborted.
- Added: `sift.table_segments` reports `format`, `unfrozen`, `min_xid` and `max_xid` of a group (`pg_sift--3.1.sql` and the update step `pg_sift--3.0--3.1.sql` declare the 12 columns).
- Fixed: an `INSERT ... ON CONFLICT` row kept its speculative flag after the insertion was confirmed, which kept its write buffer from being promoted.
- Changed: the live-row counts are exact for a frozen group and, for a group that is not frozen, count the rows that are committed or the transaction's own (`ANALYZE` and the planner's estimate read them).
- Not done in 3.1: a write-buffer backpressure that merges first at 75% of the directory; a bulk write that goes straight to groups; promotion concurrent with the writers.
- Changed: smaller BM25 indexes (segment format 3). On 1M short titles the index goes from 11.27 to 7.93 MiB (7.88 to 4.54 MiB with `positions = off`); on 50,000 articles from 31.13 to 30.21 MiB (12.95 to 12.02 MiB). Three parts shrink: the per-page document length rows take 4 or 8 bits where the superblock's longest document allows, the posting containers of mid-frequency terms are packed (about 9 bits per posting instead of 16), and dictionary entries lose a byte of lengths and one of document frequency per term and two bytes per inline key. Queries cost the same or less (count queries 17% fewer instructions on the titles, ranked top-10 within 1%).
- Upgrade note, BM25 segment format 3: no `REINDEX`. The new library reads format 2 segments (3.0) next to format 3 ones and writes format 3 for new segments, merges and VACUUM rewrites; the metapage becomes format 1.3 with the first format 3 segment, **and a 3.0 library refuses such an index** (`sift index "NAME" has on-disk format 1.3, this build reads formats 1.0 to 1.2`, hint `REINDEX the index.`), so going back to 3.0 needs a `REINDEX` with the 3.0 library. `sift.merge(index, true)` rewrites every segment of an index in format 3 at once.
- Added: `sift.facet_counts(table, key, query, facets [, filter [, hits]])` returns the counts of several facets of one search in one call: terms facets with a top N, an exact count of the other values and a NULL bucket, range, histogram and date-histogram buckets (with gap filling, capped at 10000 buckets), arrays counted once per row, columns of any type, and the facets of a multi-select (a facet ignores its own `selected` values). Every count is exact. Each facet is its own `GROUP BY` scan (the ranges of a list share one scan of `FILTER` aggregates), a `Custom Scan (SiftAgg)` with the search as the row selection when the columns are in a `sift_col` index and the ordinary plan otherwise, so a table with no column index, a `sift_table` and a role under row-level security work. `sift.facet_sql(...)` returns the statement for `EXPLAIN`. `sift.facets` is unchanged. Tests `facet_counts`, the isolation spec `facet_counts_writes` and the column fixtures `h_search_facet_scans` and `h_search_facet_counts`.
- Added: `sift.sections(a, b, ...)` builds the key of an index with `fields` without writing `chr(29)`, and `sift.field_query(query, 'title^3, body' [, index])` weighs the fields per query: each word, prefix, wildcard and fuzzy term is searched in every listed field with the weight times its boost, a row scores the sum of its fields' BM25, and the match set is that of the unweighted query. Phrases, regular expressions and `NOT` terms are not scoped to a field. Test `field_weights`: the scores equal a per-field SQL reference and the top-k equals the full ranking for 60 weight combinations.
- Docs: [docs/FIELDS-FACETS.md](docs/FIELDS-FACETS.md) has the design of both and what is left out (BM25F, a multicolumn index, an analyzer, phrases and highlights per field). The comparison row S7 (Facets) is now `Yes`; S6 (Field weights) stays `Partly`.

Upgrade notes:

- **Extension SQL:** 3.0 to 3.1 (`pg_sift--3.0--3.1.sql`). Install the files, then run `ALTER EXTENSION pg_sift UPDATE TO '3.1'` once per database. No index or table is rebuilt.
- **Formats** (`docs/FORMATS.md`): a BM25 index becomes metapage format 1.3 with its first segment of format 3; a `sift_col` index writes column format minor 4 (type tags, kinds, element rollups); a `sift_table` becomes metapage 1.3 with its first group that carries a version section. Segments of 3.0 are read next to the new ones, and a merge or `VACUUM` rewrite writes the new format.
- **A 3.0.x library refuses the new structures** with a format error and a `REINDEX` hint (the version section would otherwise show rows of running or aborted writers as final), so install 3.1 on every server that opens the data (standbys included) before the first write.
- **Going back to 3.0.x:** first rewrite every `sift_table` with the 3.1 library under `sift.col_format_minor = 3` (`VACUUM (FULL)`), which a 3.0 library reads again; then install the 3.0 files and `REINDEX` every BM25 and `sift_col` index with the 3.0 library. There is no 3.1 to 3.0 extension script. `test/t/old_lib_refusal.sh` covers the refusal and the `VACUUM (FULL)` path.
- **Behaviour changed:** `GROUP BY` of calendar keys on a `sift_table`, the new `jsonb` shapes and the element rollups plan as `SiftAgg` by default (`EXPLAIN` shows it; `sift.enable_agg = off` restores the normal plan). `sift.score()` of a wide wildcard, regex or fuzzy leaf next to an optional phrase now includes the leaf (3.0.x scored a document that matched only that leaf 0).
- **Vulkan:** `make VULKAN=1` only; `sift.gpu` is `off` by default and forced off in the packages, which have no Vulkan dependency.

## 3.0.2 - 2026-10-19

Bug fixes for 3.0.1: the aggregate pushdown for the values a `sift_col` index cannot store (the write buffer, prepared statements, a `VACUUM` that skips the indexes, a scan that is running when another session writes, numbers out of the range of their path, a NaN in a `numeric(p, s)` column), a text test on a member of a free-key object, and a much cheaper `ORDER BY <column>, sift.score(ctid) DESC LIMIT n`. Install the new files; the extension SQL does not change (it stays 3.0), so there is no `ALTER EXTENSION pg_sift UPDATE` step, and no index or table is rebuilt.

Changes:

- Fixed: a value a `sift_col` leaf cannot store that was still in the write buffer, or written after a prepared statement's generic plan was made, still raised "a column of the aggregate scan holds values its index cannot store". The directory entry of a write buffer now records the leaves it holds such a value of: the appender sets them before its row goes in and invalidates the table's plans in every session at once (committed or not), and the planner reads the mask with the segment zones, so the query keeps the normal plan and the rows are the normal plan's (a prepared statement plans again on its next use). Queries that read other leaves stay on the scan. A scan that is already running when another session writes such a value returns the rows of its snapshot (next entry). Test `col_agg_overflow_buffer`.
- Fixed: after the rows that held a value a `sift_col` leaf cannot store were repaired, the queries that read the leaf kept the normal plan until `REINDEX` when the `VACUUM` skipped the vacuuming of the indexes (dead rows on under 2% of the heap pages: `ambulkdelete` is never called). The `VACUUM` of the index now looks up the heap row of each row that holds such a value, clears the liveness of those that are dead to every snapshot (HOT chains followed, live rows kept) and the merge rewrites the segment. Test `col_agg_overflow_vacuum`.
- Test `col_agg_overflow_range`: a JSON number out of the range of its `int2`, `int4`, `int8`, `float4` or `float8` path keeps the normal plan (its rows, or its error) in every aggregate shape.
- Fixed: a `Custom Scan (SiftAgg)` that was running, or planned, when another session wrote a value a `sift_col` leaf cannot store raised "a column of the aggregate scan holds values its index cannot store" for the unit that held it, whether the writer was in flight, rolled back or committed after the query's snapshot. Only a row the snapshot sees is an error now: before a unit is filtered, the scan checks the rows that hold such a value against the snapshot (the live ones, and the heap for their pages), so the rows are the normal plan's. Isolation spec `agg_overflow`.
- Fixed: a `numeric(p, s)` column that holds a NaN raised the same error for the kernel shapes (`min`, `max`, `sum`, `avg`, `count`, `count(DISTINCT)`, a group key, `ORDER BY ... LIMIT`, a comparison, `n + 1`) where the normal plan returns the NaN. The scan now gives PostgreSQL's numeric order and arithmetic, in segments and in the write buffer: NaN is greater than every number and equal to itself, `sum`, `avg` and the arithmetic give NaN for a NaN operand, a comparison of two expressions orders them as `numeric_cmp` does, and the cast of a NaN to an integer raises `cannot convert NaN to integer` for the rows the query takes. Test `agg_numeric_nan`; fixtures `n_numeric_nan_*`. A `sift_table` table still raises for it.
- Fixed: `WHERE col @@> q ORDER BY name, sift.score(ctid) DESC LIMIT n` (the score not the first sort key, also with a leading timestamp or through a join) scored, verified and fetched every match, and ran its searches for 1024 hits, 16 times as many, then 4 times as many, each from scratch, because a Sort between the scan and the `LIMIT` gave the scan no limit hint. A scan that nothing limits (a Sort above it, or no `LIMIT`) now ranks every match in one search (a drained `ORDER BY score DESC` on 40,122 matches: 108,994 heap checks to 80,244, two per match, the minimum of a verification and a fetch). A `LIMIT` with no `ORDER BY` is the scan's limit hint (1,874 heap checks to 16).
- Added: a keyed scan for `ORDER BY <columns of the table>, sift.score(ctid) DESC [, more] LIMIT n [OFFSET m]`. A pass over the matches in heap order keeps the rows of the best `n` leading keys and those tied with the last, and the ranker scores only them; the answer is the one of ranking every match and sorting afterwards (ties, NULLS FIRST/LAST, DESC, OFFSET, joins and prepared statements, checked against that on 4,700 generated queries). The planner picks it by cost; `EXPLAIN` shows `Order: name, score DESC` and `Key Passes`. User-space instructions of the transaction, before to after, at 100K rows (40,122 matches): `ORDER BY name, score DESC LIMIT 5` 170.5M to 53.9M, `ORDER BY created_at DESC, score DESC` 184.3M to 51.0M, through a join 160.6M to 55.2M (an owner with half of the rows: 231.3M to 54.4M), `ORDER BY id DESC LIMIT 5` 169.4M to 63.3M; at 1M rows (399,781 matches) 2124M to 543M, 2261M to 504M, 2023M to 544M and 2112M to 629M. Heap checks per match 2.7 (3.3 at 1M) to 1.07, and the heap pages read are the same cold. `ORDER BY score DESC LIMIT n` and the unordered scans are unchanged (1.00). A tie larger than the limit under an Incremental Sort takes a second pass (`ORDER BY name, score DESC, id LIMIT 5` with a large tie group: 0.85 of the instructions at 100K rows, 1.03 at 30K, 0.73 at 1M). Not covered: a partition, a leading key of another table of a join, the filter pushdown and a generic plan whose query term is unknown keep the plain plan. Regressions `rank_sort_key` and `rank_unbounded`.
- Fixed: a text test (`=`, `<>`, `<`, `IN`, `LIKE 'x%'`) on a member of a free-key object (`m.* text`, `m->>'k1' = 'v4'`) raised `sift_col: predicate on leaf N of N` instead of running in the aggregate pushdown; the planner offered the encoded-text pruning to a virtual leaf. Test `agg_freekey_text`.

Upgrade notes:

- **Extension SQL:** unchanged (3.0). Install the files; nothing else is required. The index and table formats are those of 3.0.0 and 3.0.1, so their indexes and `sift_table` tables open as they are, and going back to 3.0.1 needs nothing.
- **Behaviour changed, `numeric(p, s)` with a NaN:** the aggregate pushdown (`Custom Scan (SiftAgg)`) now follows PostgreSQL for a NaN instead of raising "a column of the aggregate scan holds values its index cannot store": NaN is greater than every number and equal to itself, `sum`, `avg` and arithmetic give NaN, and the cast of a NaN to an integer raises `cannot convert NaN to integer`. A `sift_table` table still raises for it.
- **Behaviour changed, `ORDER BY <column>, sift.score(ctid) DESC LIMIT n`:** the planner can pick the new keyed scan (`Order: name, score DESC` in `EXPLAIN`), which scores far fewer rows; the rows returned are the same as ranking every match and sorting afterwards. A scan with no limit (a Sort above it, or no `LIMIT`) ranks every match in one search. Plans of these queries change; their results do not.
- **Values the index cannot store:** queries that read a leaf holding such a value keep the normal plan (its rows, or its error) also while the value is in the write buffer, in a prepared statement planned before it was written, after a `VACUUM` that skipped the indexes, and for a scan that was running when another session wrote it. A prepared statement plans again on its next use.
- **Free-key objects:** a text test on a member of a free-key object runs in the aggregate pushdown instead of raising `sift_col: predicate on leaf N of N`.

## 3.0.1 - 2026-10-19

Bug fixes for 3.0.0: a `COPY` or `INSERT ... SELECT` of any size into a `sift_table` in one transaction, four aggregate pushdown fixes (a crash, a wrong refusal, a stale overflow flag and a missed row) and the CI test harness. Install the new files; the extension SQL does not change (it stays 3.0), so there is no `ALTER EXTENSION pg_sift UPDATE` step, and no index or table is rebuilt.

Fixes:

- Fixed: a single `COPY` or `INSERT ... SELECT` into a `sift_table` is no longer limited to about 500 MB (about 4 million rows of the orders layout): it failed with `Too many write buffers waiting for promotion` because the rows of a running transaction cannot be promoted into columns. Once the directory is half full, the write buffers that one command of one transaction wrote become column groups that carry the writer's transaction and command, are invisible to everybody else until it commits (at every isolation level, through scans, indexes, bitmap scans, parallel plans and `SiftAgg`), and are dropped by `VACUUM` if it aborts (a rollback, a savepoint rolled back, a crash) or frozen once it is committed. A 5,000,000-row `COPY` into a table with a primary key, in one transaction, loads in 18 s with a backend peak of 182 MB; 10,000,000 rows take 32 s and 180 MB; the load speed of a 1,000,000-row `COPY` is unchanged. A transaction that writes through many commands (a loop of single-row `INSERT`s) or concurrent writers into the same write buffers are still limited by the directory, and the error now says so. Upgrade note: a library of 3.0.0 reads a group that still carries a writer (not yet frozen by `VACUUM`) as frozen, so the rows of a rolled-back load would show up: run `VACUUM` on the tables loaded by this version before going back to 3.0.0. See `docs/COL-TAM.md`, sections 3 and 7.
- Fixed: a backend crash (SIGSEGV) in `Custom Scan (SiftAgg)` for per-item aggregates under an outer aggregate (the arms of a `UNION ALL` over a shared CTE) when an inner aggregate has a `FILTER` on a text leaf (`count(*) FILTER (WHERE p = 'x')`). The scan did not register the predicates of the inner aggregates' `FILTER`. Test `col_agg_arms`.
- Fixed: a query that reads a `sift_col` leaf a unit flags with an overflow value (a JSON string in a `float8` path, a text over its declared length) raised "a column of the aggregate scan holds values its index cannot store" where the normal plan reads the value from the heap. The planner now keeps the normal plan for such a query, so the rows are the normal plan's; the queries that read other leaves are still pushed down. The write buffer (until `sift.col_promote`) and a cached generic plan made before the value was written still raise the error. Test `col_agg_overflow`.
- Fixed: the overflow flag of a unit stayed after the rows that held the value were repaired and vacuumed, until `REINDEX`. `VACUUM` now rewrites a lone segment that flags the overflow of a leaf only in dead rows, whatever its share of dead rows.
- Fixed: `ORDER BY <column> LIMIT n` through the top-N scan of a `sift_col` index returned other rows than PostgreSQL when an `UPDATE` had moved a row: the new version of an updated row is stored wherever the heap has room, with any value, and the scan stopped at the first window whose zone had nothing better, so a row moved to a middle superblock was missed. A window is now read when the zones of its units, taken together, can beat the n-th row (the probes only choose the visit order); the in-order cases cost the same instructions as before (1.00x at 80 superblocks). Test `col_topn_moved`.
- Test infrastructure, no product impact: the CI harness flakes of the 3.0.0 release commit are fixed with a test that fails first. The jobs that install a library flavor run after the lanes before PostgreSQL 18; the regression clusters run without autovacuum, so `VACUUM` removes what a test deleted; the `VACUUM`s of `col_preagg_maint` wait for the cleanup lock; the standby of the WAL consistency test reserves WAL from the creation of its slot; `query_dos` no longer leaves a 1 ms timeout armed for its own `RESET`. `scripts/release.flakes` is empty.

Upgrade notes:

- **Extension SQL:** unchanged (3.0). Install the files; nothing else is required. The index and table formats are those of 3.0.0, so 3.0.0 indexes and `sift_table` tables open as they are.
- **Going back to 3.0.0:** run `VACUUM` on every `sift_table` loaded by 3.0.1 first. A `COPY` or `INSERT ... SELECT` too big for the write buffers is stored as column groups that carry the writer's transaction until `VACUUM` freezes them; a 3.0.0 library reads a group that is not yet frozen as frozen, so the rows of a rolled-back load would show up.
- **`sift_col` overflow:** a query that reads a leaf which a unit flags with an overflow value (a JSON string in a `float8` path, a text over its declared length) now runs the normal plan and returns its rows; queries that read other leaves are still pushed down. The write buffer (until `sift.col_promote`) and a cached generic plan made before the value was written still raise "a column of the aggregate scan holds values its index cannot store". `VACUUM` clears the flag when only dead rows held the value.
- **Limit of 3.0.0 removed:** the "Known issue" of 3.0.0 below (a `COPY` of more than about 500 MB failing with `Too many write buffers waiting for promotion`) is fixed. A transaction that writes through many commands (a loop of single-row `INSERT`s) or concurrent writers into the same write buffers are still limited by the directory, and the error now says so.

## 3.0.0 - 2026-10-19

The column track joins the BM25 index in one extension. Install the new files, then run `ALTER EXTENSION pg_sift UPDATE TO '3.0'` in every database (the extension SQL goes from 1.2 to 3.0; `pg_sift--1.2--3.0.sql`). Existing BM25 indexes keep working with no `REINDEX`.

Highlights:

- **Column engine**: `sift_col`, an index access method that stores the columns of a heap table once more, in compressed columns with zone maps, and `sift_table` (`CREATE TABLE ... USING sift_table`), a table access method with native MVCC that also takes BM25 indexes. `GROUP BY` queries with `count`, `count(DISTINCT)`, `sum`, `avg`, `min` and `max`, filters, `GROUPING SETS`/`ROLLUP`/`CUBE` and text search as the row selection run as `Custom Scan (SiftAgg)` on the column data when the planner can prove the result is the plain plan's; anything else keeps its plan (`SET sift.enable_agg = off` turns the pushdown off). Every unit is written with its pre-aggregates (exact sums and histograms, `docs/COL-PREAGG.md`).
- **Smaller and faster segments:** the chunk codec picks the smallest cascade of encodings per chunk, filters run on the encoded form with scalar, AVX2 and AVX-512 kernels that give identical results (`sift.simd`), and the dictionaries of a segment use it too (column format 1, minor 3). `CREATE INDEX` and `REINDEX` of a `sift_col` index are parallel.
- **BM25:** a segment keeps its live documents, so a vacuumed-but-not-yet-merged index counts and scores as its live documents (`count(*)` costs 1.16 to 1.28 times the clean index's instructions at 10 to 45% deleted, was 6.6 to 6.7 times), and `sift.score()` before and after a merge are equal. `WHERE col @@> q ORDER BY <columns> LIMIT n` is answered from the column units.
- **One term, the write buffer:** "mutable segment", "mutable lane" and "delta segment" are now the write buffer. The reloption `mutable_limit` is `write_buffer_limit`; `mutable_limit` stays accepted as a deprecated alias.
- **Fixes carried from 2.0.3:** `count(*)` and `sift.facets` no longer count rows deleted before the query's snapshot when a `VACUUM` runs at the same time (the same race is fixed in the column scans), and two more corrupt-index hardenings (a damaged index raises a corruption error instead of reading past the data or crashing a backend).

Upgrade notes:

- **Extension SQL:** `ALTER EXTENSION pg_sift UPDATE TO '3.0'` in every database after installing the files.
- **BM25 indexes:** no `REINDEX`. New segments are written in segment format 2 and the metapage becomes format 1.2 with the first one; **a 2.x library refuses such an index**, so going back to 2.x needs a `REINDEX` there. Older segments are read as they are and rewritten as format 2 when merged.
- **Column indexes:** `sift_col` and `sift_table` are new in 3.0, so no 2.x index needs a rebuild. A `sift_col` index built by a pre-release build of 3.0 (column format minor below 3) is refused with the hint `REINDEX`.
- **Renames:** `mutable_limit` is `write_buffer_limit`, with the old name accepted as an alias.
- **Known issue (fixed in 3.0.1):** a single transaction can write about 500 MB (about 4 million rows of the orders layout) into a `sift_table`; a larger `COPY` fails with `Too many write buffers waiting for promotion`. Load larger data in several transactions, for example 1,000,000 rows each (`docs/USAGE.md`, Limits).
- **Test infrastructure, no product impact:** GitHub CI on the 3.0.0 release commit showed three harness flakes, all fixed in 3.0.1: PG17 `difftest` (the `hardened` job reinstalls `pg_sift.so` under the running lanes: "Permission denied"), PG18 `plan_estimate` (a snapshot older than the `DELETE` keeps the rows a `VACUUM` would remove) and PG18 `tap-004_wal_consistency` (the standby of one phase has no replication slot). The release gate (`scripts/ci.sh release`) passed 17 of 17 jobs on this tree.

- Changed (3.0): the "mutable segment", "mutable lane", "write lane" and "delta segment" are one term, the **write buffer**. The reloption `mutable_limit` is now `write_buffer_limit` (kB, 64-1048576, default 4096); `mutable_limit` stays accepted as a deprecated alias with the same bounds, `ALTER INDEX .. SET / RESET` and a quiet restore of dumps of 1.x and 2.0 (`pg_class.reloptions` keeps what was typed; the new name wins when both are set). The `kind` of `sift.segments` is `buffer` and `sealed buffer` instead of `mutable` and `sealed` (`immutable` is unchanged), so a script that matches the old values has to change; the texts of corruption and verify messages say "Write buffer" where they said "Mutable". On-disk formats are unchanged. Only new in 3.0, so nothing released changes there: `sift_col`, the settings `sift.table_write_buffer_limit` and `sift.table_inline_promote`, the kinds of `sift.table_segments` and `sift.col_segments`, `sift.col_index_info.buffers`.
- Security: a damaged index whose dictionary entry stores 4294967295 as the count of a term's tf nibbles no longer makes a query read about 4 KB
  past the term's blob. The byte counts of the term's L1 bits and nibbles were summed in 32 bits, so that count wrapped to zero and passed the
  check that the stream is inside the blob; they are 64-bit now and the term raises the corruption error. Bytes that are not what the writer
  wrote (a bad disk, a torn page, a file edited with the server stopped) were the only way in.
- Security: a damaged index whose term blob ends between its codec and the next 4-byte boundary, with a large group count, could make a query
  read far past the blob (a crash). The length left after the codec wrapped around when the codec length was not a multiple of 4; the term now
  raises the corruption error.
- Added: `WHERE col @@> q [AND filters] ORDER BY <columns> LIMIT n` is answered from the `sift_col` units (`docs/COL-SEARCH.md`, "Top-N"): a bounded heap over the match set, zone maps to skip units and stop early on columns that follow the table, and the heap read for the `n` winners only. Q10 shape: 7.1 to 2.4 ms at 1M, 191 to 19.5 ms at 10M. Setting `sift.enable_topn`; `EXPLAIN` shows `Top-N:`. The planner compares it with an ordered btree on the sort column by cost: the top-N is charged the opening of the match set whole, and a correlated sort column is read best superblock first.
- Added: text search as the row selection of the aggregate pushdown. `SELECT ... FROM t WHERE <filters> AND col @@> q GROUP BY ...` on a table with a `sift` and a `sift_col` index (or `sift_table`) takes the matches from the BM25 index without reading the heap, keeps only those rows in the column units and aggregates from columns (`docs/COL-SEARCH.md`); facet-style counts per category are the same query. The plan shows `Search:` in `EXPLAIN`.
- Added (3.0, `ALTER EXTENSION pg_sift UPDATE TO '3.0'`): the column track, one extension and one set of kernels.
  `sift_col` is an index access method that stores the columns of a heap table once more, column by column with zone maps
; jsonb attributes become typed columns for the paths of its `paths` option (shredder, `docs/COL-SHRED.md`);
  `sift_table` (`CREATE TABLE ... USING sift_table`, `docs/COL-TAM.md`) is a table access method with native MVCC that also takes BM25
  indexes, with `SiftAgg` aggregate pushdown over its column segments; its limits and the one case that needs `shared_preload_libraries` (publishing it) are in `docs/USAGE.md`. A plain scan decodes only the columns a query reads, scans go both ways (`SCROLL` cursors), `VACUUM FULL` and `CLUSTER` write column
  segments directly, and `VACUUM` counts dead rows as a heap does, rewrites mostly dead segments (`sift.table_dead_rewrite_pct`) and gives the free end of the file back. The scripts are
  `pg_sift--3.0.sql` and `pg_sift--1.2--3.0.sql` (no second extension). Settings: `sift.enable_col`, `sift.enable_agg`, `sift.simd`
  (all kernels). The library reports 3.0.
  `CREATE INDEX` and `REINDEX` of a `sift_col` index are parallel on large tables (`max_parallel_maintenance_workers`): each process writes the segments of the superblocks it claims.

- Changed (3.0): segment format 2 for vacuumed documents. A segment now keeps its live documents by offset in each superblock and, for each
  term, the number of its postings that VACUUM dropped. A dirty segment (vacuumed documents, not yet merged) therefore counts and scores as
  its live documents: `count(*)` of a term costs 1.16 to 1.28 times the clean index's in user instructions at 10 to 45% deleted (was 6.6 to 6.7
  times with 2.0.1), and N, df and avgdl exclude vacuumed documents, so `sift.score()` before and after the merge are equal (`live_stats` test).
  Indexes of 2.x keep working with no `REINDEX`: their segments are read as they are (the old statistics, until they are merged) and rewritten
  as format 2 when merged; the metapage becomes format 1.2 with the first format 2 segment, and a 2.x library refuses such an index. The
  SQLite and MariaDB hosts keep writing format 1. The index grows by 4 bytes per term of each segment (+1.4 to 2.4% in `scripts/merge-bytes.sh`).
  Queries that read postings (OR, rare terms, scans) still pay for the dead ones until the merge (up to 2 times at 45% dead).
  Stored 2.0.0 files are the fixture of the `compat_2x_fixture` test.

For each chunk it picks, from exact sizes, the
  smallest cascade of constant, frame of reference (vertical bit-packing or plain bytes), delta, run-length, sorted dictionary, decimal-float
  (ALP-style) and common-divisor (timestamps of whole seconds, amounts in cents) encodings, and layers LZ4 or zstd on a stream only where the cost rule says the saved bytes are worth the decode time. Filters run on the
  encoded form with scalar, AVX2 and AVX-512 kernels that are bit-identical and selected by `sift.simd`; what is written never depends on the CPU.
  A damaged chunk is a clean `ERRCODE_DATA_CORRUPTED` error (corruption fuzz under ASan/UBSan). The per-column `compression` policy (`auto`, `none`,
  `lz4[:N]`, `zstd[:N]`, explicit encodings) is parsed here and applied by the column engine (reloption `compression`, per column `'auto; col=zstd:9'`). Build: LZ4 and zstd are linked when their development packages are
  installed (`liblz4-dev`, `libzstd-dev`; the packages depend on them); `make SIFT_NO_LZ4=1` / `SIFT_NO_ZSTD=1` build without one. Tests: `colcodec_basic`,
  `colcodec_simd` and `make -C tools/colcodec test`.

- Added (column format 1.2, clean break from the pre-release 1.1): pre-aggregates of every unit, written with it by every path that writes a unit: exact 128-bit sums and sums of squares of the integer columns and, per low-cardinality column (dictionary text, integers, dates, `numeric(p, s)`), a histogram with the row count and the non-NULL count and sum of up to three measures per group, so counts and sums per country, category or month need no scan of the units that are clean and all-visible. Reloption `preagg` (`default`, `off`, `cube(dim: measure, ...; ...)`); about 1% of the index on the store benchmark. `sift.col_unit_agg`, `sift.col_unit_hist` and the columns `preagg_bytes` and `preagg_units` of `sift.col_index_info` show them (`docs/COL-PREAGG.md`). Segments written by a build before this one are refused with a REINDEX hint.

- Changed (column format 1.1, clean break from the pre-release 1.0): the leaf type in the stored schema is a host-independent tag
  (`SiftColType`: bool, int2/4/8, float4/8, numeric, text, bytea, date, timestamp(tz), uuid, jsonb, other) instead of a PostgreSQL type OID, so
  the format and its schema hash port to other hosts; the OIDs are mapped at the PostgreSQL boundary only (`sift_col_type_of_oid`,
  `sift_col_def_typid`). Segments written by a build before this one are refused with a REINDEX hint.

- Added (column format 1.1): text and bytea of any length in the column engine. A string longer than 255 bytes (or a dictionary past 64 MiB) is
  stored inline in its chunk with a sortable 8-byte prefix, so there is no length limit below 1 GB per unit and the `0A000` that `SiftAgg` raised
  for long strings is gone. Equality, `IN`, comparisons, prefix and length tests run on the encoded form (prefix zone maps prune units; the
  prefix filters rows before any string is compared); sources can offer the same to the planner through the optional `prune_str` / `filter_str`
  callbacks of `src/agg/sift_colsrc.h`. `sift.col_leaves` has a new column `inline_chunks`. Mutable-lane strings have no length limit either.

- Added: `CREATE INDEX ... `char(n)` and
  `name` are pushed to `SiftAgg` like text; a `bpchar` without a length is stored but never filtered or grouped by the column plans, because
  equal values can differ in trailing blanks. Test: `col_index_chars`.

- Fixed: a `sift_col` scan (and `SiftAgg`) counted rows that its snapshot does not see when a `VACUUM` ran after the scan began (found by the
  differential suite under the sanitizer: 280 against 218 orders, and by the `soak-col` job). The scan took "live in the index" on an
  "all-visible" heap page for "visible", and the two facts were read in the wrong order or from the wrong generation: the liveness of a unit
  came from the directory the scan copied at its start (or a cached flag) while the VM was read later, after a `VACUUM` had removed the dead
  tuples and made their pages all-visible; a merge that replaced the scan's segments meant that no later `VACUUM` pass reached them at all;
  and the mutable lane is a copy made when the scan begins. The all-visible mask is now read first and the unit's dirty flag from its LIVE
  page after it, the shortcut is dropped once a segment of the scan's copy has left the directory, and rows of the mutable lane are always
  checked against the heap. Tests: `col_vacuum_scan` (a VACUUM, a merge, DML and promotion between the
  open and the reads of a scan) and the `soak-col` job (`scripts/colsoak.sh`). Present in `v3` as well. The BM25 count path
  (`sift_eval.c`: the dirty flags of a segment copied at its open, the all-visible mask read later) had a similar read order; it is fixed
  by the `count(*)` entry below.

- Fixed: `-0.0` in a float column keeps its sign (the column engine stored it as `+0.0`, and a `sift_table` float column holding it raised
  `0A000` under `SiftAgg`); grouping and `DISTINCT` treat `-0` and `+0` as one value, as PostgreSQL does.

- Added: aggregate pushdown on column indexes (`sift.enable_agg`, default on). A `GROUP BY` query over a table with a `sift_col` index
  (or over a lateral unnest of an indexed jsonb array or object) with `count`, `count(DISTINCT)`, `sum`, `avg`, `min` and `max`, filters,
  `IN` lists and group keys that are expressions of one column runs as `Custom Scan (SiftAgg)` on the column data when the planner can
  prove the result is the plain plan's; everything else keeps its plan (`docs/COL-PLAN.md`). `scripts/coldiff.sh` runs a corpus both
  ways and compares the results, including float tolerances, errors and the DML states of the table.

- Added: the aggregate pushdown now matches `GROUPING SETS`, `ROLLUP`, `CUBE` and `GROUPING()` (all sets in one scan, with the NULLs and
  bit masks PostgreSQL gives), float4/float8 group keys (`-0` and `0` are one group, every NaN is one group), `count`, `count(DISTINCT)`,
  `min`, `max`, `sum` and `avg` of the columns of a lookup join, and a `WHERE` on the outer query of per-order aggregates (quals on the
  keys filter the rows, quals on the inner aggregates are a per-order HAVING). Anything else keeps its plan.

- Added: numeric(p <= 18, s) predicates, group keys, expressions and distinct counts in the aggregate pushdown. A `numeric` column is an int64
  leaf with a scale: `= <> < <= > >=`, `BETWEEN`, `IN` and `IS NULL` against constants and parameters (scaled with PostgreSQL's `numeric_mul`,
  `numeric_floor` and `numeric_ceil`; NaN, infinite and out of range constants are decided without a scan), `GROUP BY` the column or an
  expression, `count(DISTINCT)`, `sum`/`avg(DISTINCT)`, and `+ - *`, `abs`, `round`, `trunc`, `floor`, `ceil` and integer casts in aggregate
  arguments, keys and filters, with the scales PostgreSQL gives and exact int128 arithmetic; division, other
  functions, casts and wider numbers are evaluated by PostgreSQL over the decoded values, once per distinct tuple of leaf values
  (`docs/COL-PLAN.md`, Expressions PostgreSQL evaluates). `GROUP BY` an `int8` column is pushed down too (`docs/COL-PLAN.md`, Numeric).

- Added: the vectorized operators of the aggregate pushdown: comparisons and range tests on
  int32, int64 and double vectors, dictionary and integer lookup tables, bitmap algebra, selection vectors, gathers, group ids, sums,
  min/max, per-group accumulation and adjacent-distinct counting, as one scalar, AVX2 and AVX-512 table chosen at run time by `sift.simd`.
  Every level returns bit-identical results (doubles follow PostgreSQL's float8 order, the float sum has one fixed accumulation order).
  The generic operations (integer comparisons, null flags, dictionary lookups, bitmap algebra, selection vectors, gathers, parent
  ordinals) are slots of the column kernels, one implementation each.
  The `aggkern_simd` regression test and `tools/aggkern` (`make test`, `make bench`) check every operator at every level against scalar.

- Tooling: `bench/bench analytics` measures aggregate queries over jsonb (objects with dynamic keys, arrays of objects) on a deterministic
  orders corpus: the queries S1-S7 on store sets of 10, 400 and 4,000 stores and the whole-table scans G1-G8 as the CPU time and instructions
  of one backend, the baseline plan against the sift_col plan, optionally against flat tables and a throwaway ClickHouse; `analytics
  clickbench` runs the 43 ClickBench queries.

- Fixed: `count(*)` through the index (and `sift.facets`, an unordered scan that returns no column, and the aggregate pushdown with a text search or a `sift_col` index) could count rows its snapshot does
  not see while a `VACUUM` ran. A `VACUUM` clears the index entries of the tuples it removes and only then marks their pages
  all-visible; a scan reads the liveness of a segment (the directory's dirty flag, the dirty-SB bitmap, the live bits, the mutable
  segments' records) before it reads the visibility-map bit of the pages, so when the `VACUUM` finished in between it took the page as
  all-visible, took the liveness as read before the removals, and counted the dead documents (rows deleted before the query began and not
  yet vacuumed: the index count was higher than the sequential count of the same snapshot, by up to the rows deleted). A merge or a promotion that
  replaced the segments under the scan did the same, because the `VACUUM` after it clears the bits of the new segments and the scan
  still read the old ones. The heap decides every row this can touch: a `VACUUM` that removed rows now ends with a directory update, and a scan
  that finds the directory changed since it read it (after the map, per superblock) does not take any page as all-visible for the rest of the scan.
  Rows returned by a search (`SELECT ... WHERE body @@> ...`) were never affected: the heap checks them. No change to the on-disk format or the SQL.
  Found while fixing the same race in the column engine; seen on 2.0.2 and every earlier release as an index count above the sequential
  count (96 differing counts in a 90 s run with 6 readers, deleters, updaters, inserters and a `VACUUM`/`sift.merge` loop; none without the `VACUUM`).
  Tests: `vacuum_agg_snapshot`, `vacuum_agg_stress` and `vacuum_count_snapshot` (the scan waits before the map read while the `VACUUM`, merge or promotion runs: clean segment, dirty segment
  whose second superblock turns dirty, merge, mutable segment, page-by-page scan) and `vacuum_count_stress` (the run above, 40 s).

## 2.0.2

No change to the extension SQL (it stays 1.2: no `ALTER EXTENSION pg_sift UPDATE` step), to the on-disk format or to the results: install the new files, no `REINDEX`. Two fixes (the first is a security hardening, the second a correctness fix) and no new behavior.

- Security: a query that read a damaged index could read outside the term it was decoding, write to the stack, or never end. The query path
  follows the lengths, counts and offsets that a term blob stores (the sizes of its blocks and groups, its Rice parameter, the extents of
  its bitmap and array containers, the nibbles and escape bytes of its tf stream) without the exhaustive checks of `sift.verify`, and used them
  unchecked. Bytes of the index that are not what the writer wrote (a bad disk or controller, a torn page, a file edited with the server
  stopped, a physical copy from a damaged or hostile source) could therefore make a backend read past the blob (an out-of-bounds read, or a
  `memcpy` with a negative length), write past a stack array, or return one group without end. Fuzzing the
  decoder (libFuzzer with ASan and UBSan) found 35 distinct crash inputs on 2.0.1 in five minutes; the decoder is now clean on all of them
  and on about 4 million further executions, and on the six reproductions of the audit. **There is no remote vector**: no SQL role can write
  index pages, so this needs damage on disk or write access to the data directory. What to do: install 2.0.2. A damaged index now fails the
  query with `ERRCODE_DATA_CORRUPTED` (`XX001`, `sift: corrupt posting data: ...`, naming what is wrong) instead; it still has to be
  rebuilt (`REINDEX`), which is needed only for an index that is actually damaged, and `sift.verify` reports it. Checks are made once per blob
  header, per group and per block, not per posting. Also fixed: `sift.verify` read one nibble past the tf stream of a term whose L1 bits
  counted more nibbles than the stream holds, and a varint of any length was accepted by the decoder (it ended on the zero bytes after the
  blob). Test: `read_corrupt` (every byte of ten term blobs, LIST and GROUPED, with and without tf, set to five values, and every length of the
  dictionary entry changed: the read finds the blob intact or raises `XX001`/`XX002`, and one case per kind of damage with its message), run
  under `SANITIZE=1` in CI. Cost, user instructions per operation against 2.0.1 (`bench/steps/cpu_gate.sh`, interleaved, 7 rounds): the
  queries 0.995x to 1.006x (the cells with a tight interval 1.000x to 1.003x), `VACUUM` after deleting 80% of the rows 1.007x (it opens
  every term blob of the segment it rewrites), `CREATE INDEX` 1.004x, `COPY`/inserts/`sift.merge`/`sift.promote` 1.000x to 1.003x.
- Fixed: a lazy `VACUUM` running next to queries could reuse the index blocks of a segment while a query was still reading them. A
  `VACUUM` (manual or autovacuum) is left out of snapshots and of the xid horizon, its own xid included, so a query that began after the
  `VACUUM` had taken its xid saw that xid as passed, and the blocks the `VACUUM` released with it (the segments it merged or rewrote) became
  reusable while the query still held the directory that listed them. The query then failed with `sift index "t" is corrupted`
  (`Block 23: expected page type 4 generation 107, found type 8 generation 0`): the page checks every read makes turn the reuse into an error;
  no wrong result was seen. Reproduced on 2.0.1 within seconds by 24 pgbench clients (inserts, deletes, and searches of 3,000 stable
  rows with a content check) with `VACUUM` in a loop, and not without the `VACUUM` loop; with the fix the same run passes (three runs). **Who is
  affected:** indexes that are searched while `VACUUM` or autovacuum runs on their table and the segments get rewritten (dead documents,
  merges by the `VACUUM`); only the reuse of released blocks changes, never the contents of a segment. What to do: install 2.0.2; the
  error is transient (retry), nothing is stored wrong, no `REINDEX`. A `VACUUM` now releases with the next xid, so the blocks it frees are
  reusable once a later transaction has started (its own truncation of the free end of the file, which holds an exclusive lock that keeps
  every query out, counts them as reusable at once, so the file still shrinks in the `VACUUM` that freed the space). Test:
  `test/t/maint_readers.sh` (inserters, deleters, searches of fixed rows with a content check, and `VACUUM` in a loop); `vacuum_truncate` takes
  a transaction between the `VACUUM` and the merge that reuses its space.
- Docs: sample queries and documents in the documentation and tests use neutral wording; no change for users.

## 2.0.1

No change to the extension SQL (it stays 1.2: no `ALTER EXTENSION pg_sift UPDATE` step), to the on-disk format or to the results: install the new files, no `REINDEX`. One behavior changes for roles under row-level security (first entry).

- Security: a role under row-level security or reading a security barrier view could learn about the rows the policy hides. The ranked scan and
  the match estimate ran before the security quals of the relation, so the BM25 statistics of the whole index (document count, document
  frequencies, average length) entered the scores of the visible rows, and the planner's costs and row estimates followed the postings of the
  hidden rows. With a policy hiding 3,000 of 3,020 rows, a visible row of a term that 20 visible documents have scored 0.000184, a value
  computed from the 3,020 documents. What a role could infer is aggregate (how many documents the hidden rows have and how common a term is
  in them); the hidden rows themselves, and the rows and counts the queries return, were never exposed. Affected: any table with a sift index
  that is read under a policy (`ENABLE ROW LEVEL SECURITY`, for the roles the policy applies to) or through a `security_barrier` view, by a
  role that can run ranked queries or `EXPLAIN`; not affected: tables without row-level security, and the table owner and the roles that
  bypass it. The match must now be securely promotable, as an index qual must be, and the estimate uses the default selectivity when the
  relation has security quals: for those roles the rows come from a sequential scan, there is no `SiftScan` or `SiftCount`, and
  `sift.score()` raises `sift.score() is not available on "t" while row-level security or a security barrier view applies`. A query that
  ranked under a policy before now fails; rank as a role that the policy does not apply to. `sift.facets` already refused tables with
  row-level security. Test: `rls_stats` (plans and `EXPLAIN` costs of two tables that differ only in their hidden rows are identical;
  no scan, count pushdown or score under a policy or a security barrier view; the owner keeps the scan).
- Performance: `count(*)` over vacuumed documents (segments with dirty superblocks, until a merge rewrites them) costs a fifth to a half of
  what it did in 2.0.0. User instructions per query, interleaved A/B against 2.0.0, 200,000 documents with 10, 25 and 45% deleted and
  vacuumed: the count of a term 0.21x, of two terms 0.49x, of three with OR 0.30x; ranked AND 0.87x to 0.88x, ranked OR 0.84x to 0.85x, the
  filtered scan 0.91x to 0.94x. Against the same live rows rebuilt, the count of a term went from 18.5 to 18.6 times the clean index's to 6.6
  to 6.7 times, two terms from 3.5 to 4.4 to 1.7 to 2.3, three with OR from 4.5 to 6.2 to 1.5 to 2.1. A dirty superblock now cuts a group out
  of its liveness bits in one pass instead of one expansion per page; a clean superblock of a segment that has dirty ones is counted without
  decoding its documents; and the page index of a superblock is built only by what reads a page's rows. **Known limit:** a count over vacuumed
  documents still costs several times a clean index's (6.6 to 6.7 times for a term) until a merge rewrites the segment; closing that gap needs
  a change of the on-disk format and is not part of this release. Clean indexes are no slower: the clean count of a term takes 0.57x of the
  instructions (a group counted in bulk no longer walks its pages), of two terms 0.95x to 0.97x and of three with OR 0.87x to 0.91x, and the
  instruction gate (`bench/steps/cpu_gate.sh`, 16 query cells on 61 segments, one segment and after `VACUUM`) shows no regression (0.98x to
  1.00x). No change of the format; counts, rows and scores are the same. The `expand` kernel takes the mask and the start bit and returns the
  popcount (the digest pinned by `kernels_simd` changes; every level stays bit-identical). `bench/dirty-query.sh` also times the count of two
  terms, of three with OR and the parallel count.
- CI: `make check` keeps its scratch small (admission of jobs by the scratch they need, 1 MB WAL segments and a small WAL for the throwaway
  clusters, the postings comparison of `index_build_postings` in four batches over the same rows); no change for users.

## 2.0.0

A major version because several behaviors change (listed first); the on-disk format is unchanged, so no `REINDEX`.

### Upgrade

- **Run `ALTER EXTENSION pg_sift UPDATE` in every database after installing the new files.** The extension SQL goes from 1.1 (what 1.4.0 shipped) to 1.2:
  `sift.segments` gains `extents` and `dead_docs` (it is dropped and recreated, so a view that selects from it must be dropped before the update and
  recreated after), and `sift.index_info(index)` and `sift.version()` are new. The packages' smoke test installs 1.0, updates it to the packaged
  version and runs the queries again. Existing indexes keep working: no `REINDEX`.

### Breaking / behaviour changes

- `sift.max_expansions` and `sift.prefix_cache_size` can only be set by a superuser (`PGC_SUSET`). A role that sets them in a session gets
  `permission denied to set parameter`. Set them in `postgresql.conf`, `ALTER SYSTEM`, `ALTER DATABASE ... SET` or `ALTER ROLE ... SET`.
- `sift.promote`, `sift.merge`, `sift.verify`, `sift.segments` and `sift.segment_postings` need the owner of the index's table or the `MAINTAIN`
  privilege on it (PostgreSQL 17 and 18: `GRANT MAINTAIN ON table TO role`), as `VACUUM` and `REINDEX` do. Monitoring roles that called them need the grant.
- A regular expression leaf cannot have back-references (`/(a+)\1/`); the query fails at parse time like any invalid expression. Rewrite it without one.
- `sift.score()` in `UPDATE`, `DELETE`, `MERGE` and `FOR UPDATE` fails when the statement is planned, saying so.
- `VACUUM` merges until the policy asks for no more, down to the new reloption `vacuum_segments` (default 2), instead of one step: a `VACUUM` of an index
  left in many segments takes about 4 times as long as before (0.08 s to 0.33 s at 60 segments of 50,000 documents) and queries after it are faster.
  Raise `vacuum_segments` (`ALTER INDEX ... SET`, no `REINDEX`) to keep `VACUUM` short. A segment more than 8 times the size of the ones before it is
  left alone, so an index may keep more segments than the target.
- `VACUUM` returns the free blocks at the end of the index file to the operating system (when they are 1000 blocks or a 16th of the index, and only if it
  gets `AccessExclusiveLock` on the index without waiting); a hot standby cancels the queries that hold the index when it replays the lock, like for a
  heap. `ALTER INDEX ... SET (vacuum_truncate = false)` turns it off.
- The maintenance worker stays up to `sift.maintenance_naptime` (default 2000 ms) after it started, holding a background worker slot for that time;
  leave room in `max_worker_processes`.
- A wildcard, regex or fuzzy leaf that expands to more than `sift.max_expansions` (512) terms now matches every word it matches and scores as one term
  instead of failing; `sift.max_expansions` is now the number of terms a leaf scores individually.
- `CREATE INDEX` at a small `maintenance_work_mem` writes more, smaller segments instead of growing its memory or failing (at the default budget the
  layout is the same), and a merge reads at most 16 GB of segments at a time.
- Source builds link without LTO unless `SIFT_LTO=1` (at most 4 link jobs); the deb, rpm and AUR packages set it, so packages are built with LTO as before.

### Changes

- Fix: a ranked scan with `LIMIT` plus `OFFSET` of 67,108,864 or more failed with `invalid memory alloc request size` (so did
  `LIMIT 2147483647` meaning "all"), and so did a ranked scan drained without a limit once it had more than 16.7 million matches. The
  top-k heap now grows with the hits found, in huge allocations, and counts in 64 bits: a huge limit costs what the matches take.

- Fix: the score upper bounds that prune ranked scans were derived for up to 256 scored terms, but a plan has a term for every distinct
  term of its phrases (up to 256 phrases of 128 terms). Their slack now grows with the number of terms above 256 (nothing changes below
  it: same bounds, same pruning), so a document matching thousands of terms can no longer score above its block's bound and be pruned.
  No wrong result was found at 300 terms (`rank_leaves` compares the top k with an exact reference).

- Fix: a wildcard, regex or fuzzy leaf that expands to more than `sift.max_expansions` (512) terms failed with `the leaf at position N
  expands to more than 512 terms` in execution (`*ing`, `/.*tion/` or `~2` on a natural vocabulary; `EXPLAIN` worked). Like a prefix, it
  now matches every word it matches and scores as one term (the documents with such a word, tf the number of those words); a leaf with
  512 terms or fewer is unchanged. Cost on 100,000 articles (456,027 terms): `*ing` (72,478 documents) 90-170 ms, 42 ms with the blob in
  the backend's cache; `/.*tion/` 320 ms. `sift.max_expansions` is now the number of terms a leaf scores individually, for every kind.
- Changed: `sift.max_expansions` and `sift.prefix_cache_size` are superuser settings (`PGC_SUSET`) instead of settable by any role: with
  up to 1,000,000 expansions and 1 GB of cache per backend they let any role exhaust the server's memory. Set them in `postgresql.conf`,
  `ALTER SYSTEM`, `ALTER DATABASE ... SET` or `ALTER ROLE ... SET`; a role that sets them in a session gets `permission denied to set parameter`.
- Fix: a wide prefix gathered four bytes of memory per posting of every term it expands to, without bound (`t*` on a billion postings is
  4 GB). A pass now gathers at most 128 MB, then counts the postings of every superblock and builds the superblocks in windows that fit
  it (a superblock that alone does not fit sums the term frequencies as they come, 128 kB); the result is the same blob, bit for bit. A
  prefix that fits the budget (every one before) takes the same single pass as before. The scan of the dictionary and of the postings
  of the expansion serves cancel and `statement_timeout` every 16 dictionary blocks and at every group (a buffer lock held during the scan held them off).
- Changed: a regular expression leaf cannot have back-references (`/(a+)\1/`): they make a match exponential in the length of the term.
  The query fails at parse time like any invalid expression.

- Fix: a cursor over a sparse term (fewer than 8192 postings, or inline in the dictionary) allocated the 32 kB buffer a group of a frequent
  term needs; it now allocates one the size of its rows (at most one per posting, 64 bytes each). A 200-term `OR` over six segments
  allocated 39 MB for its cursors, now 1.1 MB. Frequent terms are unchanged. The copy of a frequent term's whole blob (a term in every
  row of a billion-row index is hundreds of MB per segment and query) is not windowed yet.

- Fix: a term lookup in a dictionary whose terms share their first 8 bytes (fixed-width ids such as `ord-0000` plus digits, order
  numbers, hashes with a common prefix) read every block of the run of terms with that prefix, linearly: 625 blocks per lookup in 40,000
  ids. It finds the block by bisection (10 blocks); other dictionaries read the same blocks as before.

- Fix: every query that reads the documents of a segment (ranked scans, filters) read and summed the whole SB directory of its docmap,
  24 bytes per superblock: more than 300 kB per segment and query on a billion-row index. A backend now keeps the directories of
  segments of 128 superblocks or more (16 MB, least recently used out; a segment never changes, so its generation names the directory
  for good) and a repeated query reads none of it. Smaller segments are read each time, as before.

- Planner: the estimate of a query is computed once per statement instead of three or four times (the selectivity of the clause, the cost of each
  path, the index path). The last 8 estimates of a backend are kept under the index file, the version of its directory, the expansion cap and the
  query; a promotion, a merge, VACUUM or REINDEX changes the key. Slim tier, one backend, the same 1500 statements, user cycles per statement
  (mean +- standard error of 5 interleaved runs): count 867k +- 31k to 761k +- 15k (-12%), filter 1.86M to 1.77M (-5%), mixed 1.52M to 1.45M
  (-4%), and 846k to 811k (-4%).
- Ranker: a join (or any consumer) that keeps a small share of the rows and pulls past the limit hint makes the scan search again for 16 times
  as many hits (4 times beyond 4096) instead of 4 times, so a join that keeps a tenth of the rows searches twice instead of three times. Rows,
  scores and ties are those of the full ranking. Slim tier join class, one backend, the same 1500 statements, user cycles per query: 4.74M +- 0.02M
  to 3.90M +- 0.05M (-18%).
- Planner: a prepared statement (a generic plan, whose query the planner cannot read) now gets the filter pushdown like the literal query,
  decided when the query is known: the plan keeps the filter's bitmap as a child and the scan builds it only when the bound query matches
  enough documents for it to pay (`EXPLAIN VERBOSE` shows the number); a rare query is answered by checking the filter after the ranker.
  The plan is priced as before, so the plan cache keeps reusing it. Rows, scores and ties are unchanged. Slim tier, one backend, the same
  1500 statements, user cycles per query, prepared statements: filter class 3.23M +- 0.12M to 1.66M +- 0.06M (-49%), join class 5.93M to 4.68M,
  rare AND with a 2.3% filter 772k to 658k; with the filter keeping 0.1% of the rows the ranked or class goes from 18.7M to 2.5M and the
  filter class from 4.2M to 0.95M; at 2.3% 8.5M to 3.8M and 2.7M to 1.5M; with a filter of 10% or 50% of the rows nothing changes
  (`bench/steps/ab.sh filter_matrix`: three query classes, four filters, automatic, custom and generic plans).
- Planner: the filter pushdown is chosen for a filter on a value stored in one late stretch of the heap. The planner placed the rows of
  an equality filter on a btree column from the column's statistics, which say nothing about rows loaded by group (correlation near
  zero), and priced the scan that checks the filter after the ranker as if they were spread over the heap; it now reads where the
  entries of the key lie from the btree (a few entries, at planning time) and verifies every match before them. A filter at the start
  of the heap keeps the post-filter scan, so no plan that was fast changes. Slim places, one backend, user cycles per query: filtered
  one-letter prefix (p1) 3.52M +- 0.10M to 0.67M +- 0.01M, two-letter prefix (p2) 1.05M +- 0.03M to 0.68M +- 0.02M; the top and count workloads
  of p1 and the other article shapes are unchanged within noise. Rows, scores and ties are unchanged.
- Fix: the final aggregate of a parallel `count(*)` plan started from uninitialized costs, so the planner sometimes priced the parallel count at
  thousands more and chose a partial aggregate over a parallel scan instead (found by the sanitizer run of `parallel_scan`).
- Performance: queries over an index with vacuumed documents (segments with dirty superblocks, until a merge rewrites them) are 3 to 36
  times faster. The liveness of a page was read one document at a time, a buffer pin, lock and bit test each, which was most of the
  query; the bits of a superblock are now read once and spread over the page's offsets. 200,000 documents with a quarter deleted and
  vacuumed, ms per query (the same rows rebuilt: clean): ranked AND 1.36 to 0.26 (clean 0.26), ranked OR 2.39 to 0.70 (0.64),
  unranked filtered scan 2.11 to 0.65 (0.57), `count(*)` 1.82 to 0.05 (0.019). Clean indexes are unchanged (interleaved A/B on the same
  data, 0.93x to 1.23x with 7 to 10% error; slim bench: 0 regressions). N, df and avgdl still count vacuumed documents until a merge
.
  `bench/dirty-query.sh` measures latency and score drift against the dead fraction (deleted and vacuumed, deleted from the oldest rows,
  or rolled back).
- Fix: the memory of a merge (`sift.merge`, the maintenance worker, VACUUM's rewrite of a segment with dead documents) no longer grows with
  the size of the segments: it streams them. The documents of a segment are read a superblock at a time (a cache of the rows that
  fits `maintenance_work_mem`, in place of arrays of every key and length), a term is written from the postings of its runs without being
  collected first, and the dictionary index, and the postings of a large term (at least half of what a segment's budget holds, and the
  grouped threshold), are staged in temporary files and copied into the segment. The segments are byte for byte the ones the merge wrote
  before, and the format is unchanged. Merging 4 segments of 3M documents (690 MB) at 16MB of `maintenance_work_mem` took 140 MB of
  anonymous memory and 17.4 s, and takes 16 MB and 8.3 s now (0.4M documents: 22 to 10 MB). `scripts/merge-bytes.sh` compares the
  index files of two builds page by page, and the `mergebig` group of `scripts/perf-gate-maint.sh` measures the peak and the time.

- Fix: the memory of `CREATE INDEX` grows no more with the table at a small `maintenance_work_mem`. The runs a term in most rows makes were
  merged level after level into runs that held the term whole when written as a segment (1MB budget, a term in 9 of 10 rows: 6.4 to 31.5MB
  from 0.4M to 6.4M rows). Runs now stop merging where a segment's largest term fits the budget, and the build writes the table as more,
  smaller segments instead (about 20 for 400,000 such rows at 20,000 postings a term; at the default budget the layout is the same).

- Fix: `CREATE INDEX` no longer fails with `segment dictionary exceeds 4 GB` or `postings of one term exceed 4 GB` (the lengths and offsets
  of the format are 32 bits): the runs of a build merge, and make a segment, only while their largest term and their dictionary stay
  under those limits (estimated high, so a segment holds up to a third of them), and the build writes more segments instead.

- Fix: `CREATE INDEX` on documents of a heap page that together passed 1 GB (a few large, out-of-line values: three of 360 MB) failed with
  `string buffer exceeds maximum allowed length`: a page's documents are no longer all copied before they are analyzed, a large one is
  analyzed at once. A `maintenance_work_mem` of 4 GB or more (or a document with very many distinct terms) could fail with `inverted buffer
  pool exhausted`: the buffer is now spilled before a document that could exhaust its pool.

- Fix: a keyword value of more than 255 bytes failed an `INSERT`, a `COPY` or the end of a long `CREATE INDEX` with `a keyword value is longer
  than 255 bytes`. It is now indexed as its first 236 bytes and a hash of all of it (the same term from a build, an insert and a query),
  so two long values that share their start stay two values and `@@>` finds exactly the rows with that value. A facet over such a value
  counts it under its start and hash instead of failing.

- `CREATE INDEX` reports its progress in `pg_stat_progress_create_index`: the phases `scanning table`, `spilling runs`, `merging runs` and
  `writing segments`, and the documents analyzed and then written (`tuples_done` of `tuples_total`), parallel workers included. A parallel
  build that gets fewer workers than requested logs it at `DEBUG1`.

- Security: `sift.promote`, `sift.merge`, `sift.verify`, `sift.segments` and `sift.segment_postings` were executable by every role with `USAGE`
  on schema `sift`, which could force rewrites of any index or hold its maintenance lock. They now need the owner of the index's table or
  the `MAINTAIN` privilege on it (as `VACUUM` and `REINDEX` do); `ALTER EXTENSION pg_sift UPDATE` to 1.2 is not needed for this.
- Fix: a query with `col @@> ANY (array)` failed in `EXPLAIN` and at run time with `invalid byte sequence for encoding "UTF8": 0x00`. It now
  plans as a bitmap scan over one index scan per element (an OR), prepared or not; `ALL` and `NOT ANY` use a sequential scan as before.
- Planner: materialized views get the ranked `SiftScan` and the `SiftCount` pushdown like tables. A prepared `LIMIT $1 [OFFSET $2]` gives the
  ranked scan of a generic plan its limit hint (a broad term with `LIMIT 10` took 1.38x less time per query on 50,000 articles; custom plans
  and other shapes are unchanged). `sift.score()` in `UPDATE`, `DELETE`, `MERGE` and `FOR UPDATE` now fails when it is planned, saying so.
- Extension 1.2 (`ALTER EXTENSION pg_sift UPDATE`): `sift.segments` gains `extents` and `dead_docs`; new `sift.index_info(index)` (relation
  size, high-water mark, free and reusable blocks, directory entries, maintenance lock held) and `sift.version()` (the library reported 1.0
  for a 1.1 extension).
- Operations: promotions, merges and maintenance worker runs of at least `sift.log_min_duration` (default 1 s, `-1` off) are logged, with
  the inserts that wait for room in the directory and a maintenance worker that could not be registered; errors of the worker name the index.
  `ALTER INDEX ... SET/RESET` of an option that changes the analyzer warns that the table's queries and inserts fail until `REINDEX`.
  `sift.highlight` no longer parses its query for every row.
- Docs: [docs/SIZING.md](docs/SIZING.md) (disk, memory and time of large indexes, dead documents in BM25 statistics, `SERIALIZABLE`, row-level
  security, updates). `bench/ab-query.sh` times one statement on two builds, `bench/sql/dead_docs.sql` measures the effect of rolled-back
  loads on scores. `bench/cpu-query.sh` counts the instructions and CPU time of a backend per transaction on two builds, which a loaded host
  does not change.
- Tests: `scripts/ci.sh quick` also runs the regression tests that need no test-only function against the release library
  (`SIFT_TESTING` off).
- New: reloption `merge_fanin` (default 8, 2 to 32): the segments of one size tier that trigger a merge, so far the constant 8. It is read when
  a merge is chosen: `ALTER INDEX ... SET` applies it with no `REINDEX`, and the default is today's behavior. The other limits follow it and
  stay consistent for any value: the merge of the smallest segments starts above `greatest(32, 2 * merge_fanin)` directory entries (32 up to a fanin of 16, as
  before), a merge takes at most twice the fanin of segments, and a build-time check proves that the largest fanin still merges before inserters must make room
  (112 of 128 entries). Tests: `merge_fanin` (every fanin from 2 to 32) and the TAP test `maint_fanin` (inserts against the worker at 2 and 32).
- New: setting `sift.maintenance_cost_delay` (milliseconds, 0 to 100, default 0): the maintenance worker sleeps that long after every 16 pages (128 kB)
  its merges read or write, like `vacuum_cost_delay`. At 0 nothing changes. `VACUUM`, `sift.merge()` and the inserters' merges are not delayed.
- New: reloption `dead_rewrite_pct` (default 50, 1 to 100): the percent of dead documents above which maintenance rewrites a lone segment, so far
  the constant 50. `ALTER INDEX ... SET` applies it to the next `VACUUM` or worker step, with no `REINDEX`; 100 never rewrites. The default is today's
  behavior: against 5 (400,000 documents, 10 rounds of 6% updated or deleted rows) it took `VACUUM` 1.9x (update) and 6.9x (delete) as long, wrote 5.3x
  the WAL on deletes and left a 1.3-1.4x larger file, for faster queries only after deletes. Tests: `dead_rewrite_pct`.
- Measured, no change: `mutable_limit` stays 4096 kB. At 1024 the 16-client insert rate was 18% lower, `INSERT .. SELECT` 17% and `COPY` 47% lower, and the
  index grew 37% larger in 4.5 times the segments, for ranked queries 2 to 4 times faster; promotions and merges were 3 times shorter but 3.7 times as many
  (`scripts/perf-gate-maint.sh --cells mut`, `--cells dead`; numbers in [docs/USAGE.md](docs/USAGE.md#maintenance-and-operations)).
- Fix: the file of an index never shrank. The space `VACUUM` and the merges free at the end of the file stayed allocated (a table that kept 5%
  of 200,000 documents: 722 blocks after the bulk delete, a `VACUUM`, `sift.merge(index, true)` and another `VACUUM`, 639 of them free).
  `VACUUM` now returns the free blocks at the end of the file to the operating system, as it does for a heap: when they are 1000 blocks or a
  16th of the index, and only if it gets `AccessExclusiveLock` on the index without waiting (a query or an insert that holds the index, or the
  maintenance worker in a step, keeps it from truncating until the next `VACUUM`); the lock is released as soon as the file is cut. That
  example: 722 blocks before, 83 after. A hot standby cancels the queries that hold the index when it replays the lock, like for a heap.
  A `VACUUM` with parallel workers (several large indexes) cannot take the lock, which needs a transaction id, and leaves it to the next
  one; `VACUUM (PARALLEL 0)` and autovacuum truncate. No on-disk change. Only blocks the horizon already lets the allocator reuse are
  given back. `ALTER INDEX ... SET (vacuum_truncate = false)` turns it off for an index (no `REINDEX`).

- Performance: `CREATE INDEX` takes fewer instructions per document and posting: the checks of the memory bounds left that path (the order of the
  documents is checked once per page, the pool once per large document, a page is sorted only when its HOT chains made it unsorted), and a
  table with few runs writes no extra runs at 1MB. Runs are merged only when the run to come does not fit a segment, not before: a level that
  already fits is read by the merge that writes it, with no pass of its own (at 1 MB that pass cost 46% more temporary bytes on a table whose
  largest term just fits).
- Fix: an index with damaged offsets in a sparse term (inline keys or a LIST blob with more than 512 offsets) reports a corrupt term, as
  `sift.verify` does, instead of crashing; a damaged docmap directory no longer shares the cache entry of an intact one.
- Build: `make` links without LTO unless `SIFT_LTO=1` (at most 4 link jobs): PGXS inherits `-flto=auto` from some distributions' `pg_config`, which
  started one link job per core in every development, CI and bench build. The deb, rpm and AUR recipes set `SIFT_LTO=1`.

- Fix: promoting a mutable segment no longer takes memory in proportion to the segment. The records were all copied to memory, about six
  times the size of the segment, whatever `maintenance_work_mem` (a segment of 138 MB at `mutable_limit = 1048576`: 827 MB at a 64 MB budget, 797 MB
  at 16 MB); they are now sorted in a tuplesort that spills to disk (191 MB and 155 MB; a segment of 46 MB at a 4 MB budget: 54 MB, where it took
  268 MB). A segment that fits the budget promotes as fast as before (1.0 to 1.2 s against 1.2 to 1.7 s at 64 MB); one that does not is slower than with
  all of it in memory (3.8 s against 1.7 s at 16 MB), the price of staying within the budget.
- Fix: a long merge no longer stalls the inserts. Promotion and merge steps now claim the segments they replace instead of holding one
  exclusive lock per index, so the background worker's merge, an inserter that finds the directory nearly full (112 of 128 entries) and
  the inserters that promote a backlog of sealed segments work on different segments side by side. Before, every such inserter waited
  for the whole merge, and the throughput of 8 bulk writers equalled that of one. VACUUM, `sift.promote()` and `sift.merge()` still run
  alone. The inserter waits only when everything left to merge is being merged by others.
- New: `sift.standby_conflicts` (superuser, default off). On a hot standby without `hot_standby_feedback`, the primary logs a recovery conflict before it reuses blocks released by a merge, so a standby scan is waited for (up to `max_standby_streaming_delay`) or cancelled by the usual conflict instead of failing with "segment recycled during scan". Default behavior is unchanged.
- Fix: a promotion or merge that fails (out of disk, a corrupt segment, an error of the host) now returns the blocks it had taken to the
  free list, where it used to leave them allocated until the next orphan sweep, and the background retry doubled its delay after each
  failure (2 s up to an hour) instead of failing again every 2 s and growing the index file each time. `VACUUM` and the inserts that
  merge for room no longer fail with the step: the error is logged as a `WARNING` and counted for the delay. An explicit `sift.promote()` or
  `sift.merge()` still raises it. A background worker that cannot be registered is logged. The wrapper and the delay add nothing to the
  insert path or to a step that succeeds.
- Fix: the file of an index grew to 2.6 times its live segments while the background worker merged a backlog of segments, and kept that size.
  The worker's one transaction held back the horizon the free extents are judged by, and the free space the merges released was too
  fragmented for the long runs a merged segment asked for. The worker now runs every promotion or merge in a transaction of its own, a
  step reuses what the previous one freed (the horizon is computed anew at each allocation), and the streams of a step take shorter runs of
  free space, down to a 256th of the size they are expected to need. 60 small segments merged by the worker: file 2.6 times the live
  segments, now 1.6. An old snapshot elsewhere (an idle transaction) still keeps what is released after it from being reused.
- Fix: a merge or promotion of a segment above about 8 GB failed with `sift segment stream needs more than 1002 extents` and, with it,
  `VACUUM`'s cleanup and the inserts that merge for room, when other writers allocated between the runs of its stream. The runs of a
  stream now grow with it (up to 64 MB), 16 GB take about 300 extents, and a merge reads at most 16 GB of segments, so a segment past
  that size is left alone (the policy takes the smaller ones of its run, `sift.merge(index, true)` too). Nothing changes below 1 GB.
- `VACUUM` merges until the policy asks for no more, instead of one step: an index that a build with a small `maintenance_work_mem` left in
  60 to 128 segments (queries 2.2x slower at 11 segments than merged) needed one VACUUM per 8 segments less; now one merges down to
  `vacuum_segments` (below). A `VACUUM` of such a backlog takes about 4 times as long as the single step it did before (0.08 s to 0.33 s at 60
  segments of 50,000 documents): deliberate maintenance work, which leaves 2 segments instead of 46 and makes the queries after it faster
  (`count(*)` 1.20 ms to 0.68 ms, ranked 2.16 ms to 1.95 ms). Queries that run while `VACUUM` merges are 6 to 7% slower, from the
  extra merging itself: the same queries with the same segments and no merge running, and the same maintenance on the old build, measure the same.
- Fix: after a COPY or bulk insert with `sift.async_maintenance` on, up to three sealed segments could stay unpromoted until the next seal or a
  `VACUUM` (queries read them as mutable data: 8 to 16 ms instead of 1.3 ms): the worker was started once per 2 s and exited when it had
  nothing left, so the seals of the rest of that window had none. The worker now stays, and looks for work, until
  `sift.maintenance_naptime` (new, default 2000 ms, 200 to 60000) after it started. It holds one worker slot for that time.
- Fix: with the default `vacuum_segments = 2`, adding a small segment to a large index made every `VACUUM` rewrite the whole index (a 0.1 MB
  segment next to 11 MB: 0.8 to 0.97 s and 11 MB of WAL at each, against 5 ms with `vacuum_segments = 3`), because the two smallest segments
  were merged whatever their sizes. A segment more than 8 times the size of the ones before it now stays, so an index may keep more than
  the target when sizes differ that much (`sift.merge(index, true)` merges them). The rewrite of a mostly dead segment was also never reached at
  a large `vacuum_segments`: the merges the target discarded hid it; it is now chosen alone.
- New: reloption `vacuum_segments` (default 2, 1 to 128): the number of segments `VACUUM` leaves an index with. A higher value is shorter
  `VACUUM` and more segments to query (60 segments of 50,000 documents: 0.19, 0.15, 0.12 and 0.10 s at 2, 4, 8 and 16, `count(*)` 0.08, 0.14,
  0.22 and 0.35 ms). `ALTER INDEX ... SET` applies it with no `REINDEX`; the worker's and the inserters' merges are unchanged.

## 1.4.0

- No change to the PostgreSQL extension's behavior, on-disk format or SQL: install the new files, no `ALTER EXTENSION` and no `REINDEX`. Indexes, scores and plans are the same as 1.3.1; the benchmark queries run at the same speed.
- The engine now builds as a portable core shared with the SQLite extension and the new MariaDB extension (`src/portable.list`, one host header `src/sift_host.h`, checked by `scripts/check-portable.sh` in `make check`). The sorted-run and merge code moved from `src/sift_build.c` to `src/sift_runs.c`, the merge policy to `src/sift_merge.c`; the compiled query path is instruction for instruction the same.
- `make check` takes about 1.5 minutes on 8 cores (it took about 4): one job queue over up to 8 lanes instead of four fixed lanes, faster differential and TAP tests, same coverage.
- `scripts/build-bench.sh` compares `CREATE INDEX` time, memory and temporary files of two builds at several budgets; the runs merged into each segment are logged at `debug2`.

## 1.3.1

- Fix: `CREATE INDEX` failed with `sift parallel build worker needs more than 256 runs` (`HINT: Increase maintenance_work_mem or build
  without parallel workers`) when a parallel worker spilled more than 256 runs, i.e. on a large table with a `maintenance_work_mem` small for
  it; a build of enough runs for more than 128 segments failed with `sift index ... would need more than 128 segments`. The build now
  merges runs in passes (the last fan-in runs of a level into one of the next, a fan-in bounded by the memory budget), so any table builds
  at any `maintenance_work_mem` (down to 1MB), in parallel or not: a small budget only makes the build slower, and when it would need more
  than 128 segments the build writes fewer, larger ones. The index answers and scores as one built with more memory. Builds at the default
  or a generous budget take as long as before; at 1MB they take up to 1.5x longer (second merge pass), where they used to fail or exceed
  the budget. `sift` logs the runs merged into each segment at `debug2`, and `scripts/build-bench.sh` compares build time, memory and temporary
  files of two builds at several budgets.

## 1.3.0

- Planner: `@@>` on a key whose index has `fields` (or any analyzer other than the default) is estimated from the index like any other. It is
  the call `sift.match_idx`, which had no estimate, so the planner expected a third of the rows for every query (a term of 44 of 1M names as
  a third of them) and costed the scan and the joins above it as for a broad term. A rare term of such an index, searched among the
  attributes of a tenant with 10% of the names (`id IN (SELECT ...)` through a left join that repeats them): hash join 2.6 s, now a
  nested loop semi join under the ranked scan, 18 ms.
- Planner: a ranked scan with a `LIMIT` is costed for what it does when drained. Past its limit hint it ranks again for four times as many
  rows, to the last match, as a hash join or a sort above it makes it; its total cost was its startup plus a hundredth per row, so hash
  plans over the whole scan looked far cheaper than they are and won against the nested loop semi join that stops at the limit. 1M names,
  152k matches of `honey`, a tenant with 0.3% of the names: 190 ms, now 68 ms (`h*`: 247 to 69 ms; a rare term 9.1 to 1.6 ms); tenants
  with 2% to 50% keep their nested loop (17 and 3 ms), and one with 0.05% its hash join (about 110 ms).
- Removed: reloption `phrase_index = pairs`; phrases use word positions. `CREATE INDEX ... WITH (phrase_index = ...)` fails like any unknown
  parameter, and an index built with it is refused ("built with other analyzer options") until `ALTER INDEX ... RESET (phrase_index)` and
  `REINDEX`.
- Benchmarks: the engines are measured one at a time, each with every CPU of the host, interleaved in rounds (the CPUs are no longer shared out or capped);

## 1.2.0

- Word positions are stored by default (reloption `positions`, default on): phrase queries are decided from the index instead of
  re-tokenizing the heap text of each candidate. A phrase that every match must contain (not under `OR` or `NOT`) is verified in the
  index, and a phrase under `OR` (a CJK word becomes one) filters its candidates the same way; counts and ranked scans of such phrases
  read no heap tuple. On the 218k-article benchmark phrase queries run about 4.1x faster than with `positions = off` and 3.3x faster than pg_search,
  counts 7.3x faster. The price is a default index about 2.5x larger (52.5 MiB to 130.9 MiB on the 218k articles, pg_search: 183.6 MiB) and a
  build about 30% slower (1.7 to 2.2 s): `positions = off` restores the small index and the heap recheck of phrases. Indexes built by earlier
  versions keep working but have no positions: `REINDEX` them to get phrases decided by the index. Terms with at most two postings
  keep their positions in the dictionary entry (up to 256 bytes). Documents inserted after the build (mutable segments, and the
  segments promoted and merged from them) and phrases under `NOT` keep the heap recheck.
- On-disk format 1.1 (metapage minor version): indexes created or reindexed by this version are 1.1 and must not be opened by earlier
  releases, which write 1.0 and do not check the minor version (`REINDEX` the index there to go back). Format 1.0 indexes stay readable and work without positions until
  `REINDEX`. A build that meets a newer minor version than it knows refuses the index with "REINDEX the index".
- Counts and filters: a query with a phrase rechecks in the heap only the candidates the bitmaps and positions do not decide (an
  `OR` branch without the phrase, a `NOT` of a superset and every segment with positions are not rechecked); `count(*)` counts the
  all-visible pages of a group straight from its bitmaps; the planner no longer overcharges a pushed-down filter, so a ranked scan
  takes it when it is cheaper (a ranked `OR` with a 2% filter 6.0 -> 4.8 ms, plan choice wrong for 2 of 100 queries instead of 70);
  terms from 8K postings (was 64K) are bitmap groups with block bounds: ranked `AND` and `OR` about 20% faster and `count(*)` of
  `OR`s about 27% faster for about 10% more index (applies to new builds and `REINDEX`).
- Top-k: ranked blocks accumulate leaves by descending upper bound and score only the documents that can still reach the threshold
  (new `bm25_sel` and `prune_gt` kernels in AVX-512, AVX2 and scalar), and the bounds of dense LIST terms are read without decoding
  their postings. Scores and ties are bit-identical. Ranked `AND` and `OR` use about 27% fewer cycles, prefix top-k and boosted
  place queries 8-34% fewer.
- Fix: `sift.score()` through joins. A ranked scan under a join tree of depth two or more (a join plus a semi join, a lateral join, outer, anti and self joins, aggregates over a join) raised "must be computed by a sift scan" although the plan scanned the table with a ranked SiftScan: the joins now pass the scan's score on, also from a partitioned or inheritance parent. Regression `rank_join`.
- Prefix queries never fail on the expansion limit: a prefix with more than `sift.max_expansions` terms (`h*` or `ho*` on a large
  table without `prefix_index`) matches all the words that start with it and scores as one term, the same score `prefix_index` gives it
  (below the limit a prefix still scores as the max over its terms). `prefix_index` is now a speed and size choice, not a requirement
  (a million names: 10-15 ms for `h*` the first time in a backend, under 1 ms cached, `sift.prefix_cache_size`, against under 1 ms and
  a 2-4x larger index with it). Wildcard, regex and fuzzy leaves still raise the error above the limit. No index format change.
- PostgreSQL 17 support, with 19 (release candidate) experimental: `make` accepts 17 and newer, and the version differences live in
  `src/sift_compat.h`. PostgreSQL 17 has no `extension_control_path`, so `scripts/pgtest.sh install` copies sift into the server's own
  directories there (`SIFT_SUDO=sudo` when they are not writable) and the update-path checks of the lifecycle test are skipped. CI
  runs 17 and 18, and 19 built from git without failing the workflow.
- Fix: `name:(a b)` and `name:{a b}` lost their qualifier when the group was unboosted and the same operator as its parent (the leaves
  searched the whole text, on the index and on a seq scan alike), and `name:(NOT a)` dropped it too. Without `fields` a colon is text
  in groups as well (no more "unknown field (the index has none)"); a name an index with fields lacks lists its fields.
- Fix: with several sift indexes on one key, an index with another analyzer (one with `fields` next to a plain one) could serve `@@>`, so
  a query answered differently depending on the cheapest plan, or failed while the planner costed the index. `@@>` now means one analyzer
  (the default one when an index has it) and only indexes that answer the same serve it.
- Fix: a ranked scan with a broad match and a selective filter on a column that follows the heap order (an id range or id list at the
  end of the table) was planned as filter-after-ranker, which verifies every match before the filter's first row (tens to hundreds of
  ms against under 1 ms pushed down). The post-filter estimate now counts the matches before and after the rows the filter's btree
  quals can keep, from the column's histogram and correlation, and the bitmap's tuple count no longer includes the planner's lossy-page
  inflation. The estimate also prices the blocks the ranker scores (a broad term ties its superblock bounds, so the scan runs in heap
  order) and the descents of a `= ANY(const array)` bitmap by the leaf pages they touch rather than one random page per element, so
  1000 to 10000 scattered ids with a broad term are pushed down (418 ms against 5 ms in the report) and ids at the head of the heap
  stay on the plain scan only when it is faster.
- Fix: assertion failures on a server built with `--enable-cassert` (a stray `foreach_node(Node/Path/Plan)` cast, the GlobalVis horizon
  taken from the index instead of its heap, and a page special area read before the page layout check in `sift.verify`).
- Packages: a `.deb` (`postgresql-18-pg-sift`, PGDG on Debian 13 and Ubuntu 24.04) and an `.rpm` (`pg_sift_18`, PGDG on RHEL, Rocky and
  Alma 9 and 10), x86-64, are attached to each release with `SHA256SUMS`. `scripts/package.sh` builds them from a tag and smoke-tests
  them in containers (install, create, upgrade from 1.0, remove); the `release` workflow runs it on every `v*` tag.

## 1.1.0

- Filter pushdown: a ranked scan can take a bitmap of the other quals (btree, BRIN, GIN) as a child and score only the matches it keeps;
  `sift.filter_pushdown = off | on | force`, default `on`; EXPLAIN shows `Filter Superblocks`, `Filter Pages Exact`, `Filter Pages Lossy`.
- Keyword facets: `analyzer = keyword` indexes each whole value as one term; `sift.facets(index, facet_index, query, lim, nulls)` counts
  the matches of a query per value.
- Parallel scan: parallel-aware SiftScan and SiftCount with a shared ranked threshold, behind `sift.parallel_scan` (default off). It is
  never planned together with a pushed-down filter.
- Partitioned tables: `ORDER BY sift.score(ctid) DESC LIMIT k` through a partitioned parent ranks each partition with a ranked scan under a
  MergeAppend (scores use each partition's own statistics); `sift.max_score()` is not available through a parent.
- Fix: a crash rechecking a phrase with `count(*)` or a ranked scan on an expression index (also affects 1.0.0).
- Upgrade from 1.0: install the new files and run `ALTER EXTENSION pg_sift UPDATE`.

## 1.0.0

- First release: the `sift` index access method (`USING sift`), `text @@> text` queries (boolean, phrase, prefix, wildcard, regex, fuzzy),
  exact BM25 ranking with `sift.score()`, `count(*)` from the index, mutable segments with background maintenance, parallel `CREATE INDEX`,
  generic WAL and replica support.
