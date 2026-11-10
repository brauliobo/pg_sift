# Which `sift_col` indexes to create

How to choose the columns of the `sift_col` indexes of an application's tables, what a wider index buys and costs, and which btrees to keep.

Every figure on this page is measured on the store dataset of the benchmark ([bench/sql/store/gen.sql](../bench/sql/store/gen.sql): 1,000,000 orders of nine columns, a 104.2 MB heap) with its primary key and two btrees (`customer_id`, `created_at`), releases 4.2.x and 4.3, PostgreSQL 18.6, one host, warm cache, `EXPLAIN (ANALYZE)` minimum of five runs (writes: minimum of three, a checkpoint before each). The statements are those of [bench/sql/store/queries](../bench/sql/store/queries) plus the ones written out below; the LEFT JOIN section has a generator of its own. Absolute times depend on the host; read the ratios.

## The rule

One `sift_col` index per table, with a column for every column a query filters, groups or joins by, or aggregates. An aggregate query reads the index alone; a column the index does not hold sends the scan back to the heap or to the normal plan. Both sides of a join need an index that holds the join key and the columns the query reads from that side.

```sql
-- lean: the columns the filters and group-bys of the reports use
CREATE INDEX orders_col ON orders USING sift_col (id, customer_id, country, category, status, amount, quantity, created_at);
```

## Lean or wide

| index set | size | queries answered by the scan (of 7) | sum of the 7 queries |
|---|---|---|---|
| btrees only (`id`, `customer_id`, `created_at`) | 56.4 MB | 0 | 1,254 ms |
| lean: the eight columns above, plus the btrees | 32.0 MB (31% of the heap) | 6 | 62 ms |
| wide: lean plus `title` | 38.6 MB (37% of the heap) | 7 | 30 ms |

| query | btrees only | lean | wide |
|---|---|---|---|
| `count(DISTINCT customer_id)` per month | 787 ms | 11.3 ms | 11.5 ms |
| `sum(amount)` per month | 222 ms | 1.1 ms | 1.1 ms |
| `count(*)` per country | 54 ms | 0.5 ms | 0.5 ms |
| ten largest categories by revenue | 72 ms | 0.5 ms | 0.5 ms |
| average of the shipped orders of one country in a year | 39 ms | 5.5 ms | 4.4 ms |
| `count(*)` of the big orders of the last month | 43 ms | 4.0 ms | 3.5 ms |
| ten first orders by `title` of a country and a status | 37 ms | 39 ms (bitmap scan) | 8.3 ms |

- **Start lean.** Every aggregate and filtered count above went from 39-787 ms to 0.5-11 ms with eight columns.
- **Go wide only for what lean lacks:** an `ORDER BY title LIMIT`, name prefixes, the timestamp or sort columns a list query tests. `title` takes 6.6 MB more and moves one query from 39 to 8.3 ms.

## The 32-column cap

PostgreSQL stops an index at 32 columns, and an expression is a column of the index too (`ERROR: cannot use more than 32 columns in an index` for 33 plain columns, for 30 plus three tests like `(c1 > 5)`, and for 31 plus two). Count them before adding one: drop a column no query uses.

## Columns a list query returns

A query that lists ids under a filter of a text column is an `Index Only Scan` only when the column is in `return_columns` (`ALTER INDEX orders_col SET (return_columns = 'country, category, status')`, no REINDEX): `SELECT id FROM orders WHERE country = 'DE' AND category = 'Toys' AND status = 'shipped'` takes 6.4 ms as a bitmap heap scan and 4.1 ms as an index-only scan. The option is opt-in; name only the columns the list queries filter by. Counts and group-bys need nothing: the aggregate scan reads the index alone. See [COL-SCAN.md](COL-SCAN.md).

## Order arrays: the id, and a second order by a column

The build decides two order structures from the statistics of the table; both are visible in `sift.index_choices('index')` and named in the NOTICE of CREATE INDEX.

- **A serial id is an order array, not a point directory.** A nearly unique integer that follows the heap order gets its array whole (0.4 bytes a row), and no point directory (3.3). 5,000,000 rows, `id, g, ts, amount`: 63.3 MB to 49.1 MB; an equality on the id 0.46M instructions and 5 cold blocks with the directory, 0.51M and 2 with the array (a b-tree 0.12M). A list of many ids is the cost: the array decodes a chunk of its segment for each key, about 55,000 instructions a key where the directory takes 12,000 and a b-tree 14,000 (`id IN` of 64 keys, 1,000,000 rows: 3.7M against 1.0M and 1.0M instructions, in 94 cold blocks against 155 and 139; 16 keys 1.07M against 0.40M). If the application's reads are long id lists, ask for the directory: `CREATE INDEX .. WITH (point_tid_columns = 'id')`. A table with a primary key keeps the directory of the key for the writes that supersede rows, whatever this says.
- **An index that exists keeps its directory until its choices are made again.** A merge decides them again for the segments it writes (the options show `order_array_columns = 'auto: id, ts'` and the directory gone afterwards), a REINDEX for all of them; the old and the new segments answer the same queries meanwhile (1,000,000 rows of a tip-built index plus 100,000 rows: 9.0 MB, 11.3 MB after two promotions, 6.4 MB after REINDEX).
- **The second order is grouped by the column the table is read by.** The build groups the date, timestamp and integer arrays by an integer of 100 or more values (3 rows a value at the least) that is the first or second key column of a non-unique b-tree of the table whose other key column is a date or timestamp of the index (a partial b-tree counts; a unique one, a b-tree on the column alone and an `INCLUDE` column do not), else by the foreign key or `*_id` column of most values; the b-tree wins over the entity. The b-tree is the table's own statement of the shape `WHERE col = x ORDER BY ts LIMIT n`, which the sift index replaces, and without the second order that statement reads the units (5,000,000 rows, 100 values, index `id, g, ts, amount`, one backend, no parallel workers, cold: a b-tree on `(amount, ts)` 0.33M instructions and 24 blocks; sift without the group 4.69M and 1,491; with it 0.89M and 44; the index 31.6 to 37.9 MB (+20%: the group names its entries by their position in the plain order, a chunk of them where the positions are close, and it was 50.4 MB, +60%, with their keys and heap addresses), the build 31.8 to 34.3 G instructions against the same group by keys and addresses (+7.7%); at 1,000 values +14% (+31%), at 93,000 values +65% (65.0 MB for 39.5 MB without the group; it was 102.7 MB, +160%, before the table of groups of 1,024 groups or more was packed to the 35 bits of a group, which cut the build to no more instructions and the statement from 8.2M instructions and 807 blocks to 0.91M and 214); 1,000,000 rows 7.22M and 526 to 0.50M and 33). A b-tree on the measure alone is not a signal: the units read 4.69M instructions and 1,491 blocks against its 3.48M and 1,933, so the index stays 17 MB smaller. A measure or a code with no such b-tree is not chosen (3.5 bytes a row at 100 values): name it with `order_array_group = 'auto: col'` and decide again: `ALTER INDEX orders_col SET (order_array_group = 'auto: amount'); REINDEX INDEX orders_col;` (the weighted formula still weighs it, and refuses it at 150,000 rows, where the statement reads 1.6M instructions). The NOTICE of `CREATE INDEX` and `sift.index_choices` say which one chose the group (`second order by amount: b-tree orders_amount_ts_idx`, `entity` or `hint`). An index of an earlier release keeps its arrays until `REINDEX`, which decides again; what the build wrote is the hint of the next `REINDEX`, so the group stays after the b-tree is dropped for the sift index (`ALTER INDEX .. RESET (order_array_group)` decides from the table again). Do not write `order_array_group = 'amount'`: that groups every array by the column and keeps no plain order, so `ORDER BY ts LIMIT`, `OFFSET`, a range and `IN` of another column read the units (1,000,000 rows: 0.46M to 6.5M, 0.44M to 14.8M, 0.80M to 9.7M and 1.81M to 11.2M instructions).

## Loading in key order, and `key_cut`

When the heap follows a time or key column, the index needs less for that column and a window of it reads far less. Measured on 1,000,000 rows and an index of four columns (an id, a 50-value group, a timestamp, an integer), the same rows loaded in id order (timestamp scattered) and loaded in timestamp order; instructions of one backend, minimum of five, blocks read cold:

| query | timestamp scattered | loaded in timestamp order |
|---|---|---|
| aggregate over a 3 % window of the timestamp | 12.4 M, 318 blocks | 0.77 M, 9 blocks |
| list of two group values, `ORDER BY ts DESC LIMIT 20` | 2.02 M, 480 blocks | 0.78 M, 7 blocks |
| `ORDER BY ts DESC LIMIT 20` | 0.375 M | 0.391 M |

- **Sort the whole segment, not each batch.** The order array of the column falls 75 % (2.15 to 0.54 MB) and its per-unit runs 98 % only when the heap is sorted across a segment (about 150,000 rows); sorted blocks of 2,000 rows grow the array 13 %, blocks of 150,000 rows shrink it 32 %. Sorting each `INSERT` by the column changes nothing (WAL, pages written, size and reads within 3 %): a row keeps its heap address. `CLUSTER` or a sorted load of the table does it (a `sift_table` is rewritten in the order of the btree `CLUSTER` follows, see [COL-TAM.md](COL-TAM.md)); these figures are from a `sift_col` index of a heap table.
- **The other columns pay.** A column that followed the old order loses its runs: here the group and the integer followed the id, and the index grew 58 % (8.7 to 13.8 MB); with only the timestamp ordered it shrank 28 % (7.5 to 5.4 MB). Sort by the column the queries window or order by, and only when the others do not follow the old order.
- **`key_cut` for key-shaped columns** cuts the units by the key at build, promotion and merge, so each row is written once in its unit: 96,000 inserted rows and 9,600 updates wrote 36 % fewer pages and 24 % less WAL, the file grew 45 % less and the segments fell from 11 to 6, and a window aggregate dropped from 12.4 M to 0.61 M instructions. The trade-off is `ORDER BY .. LIMIT` of another column: the key-cut index has no order array for it, so `ORDER BY ts DESC LIMIT 20` costs 0.70 M instead of 0.375 M on a fresh index and each ordered overlay of an index that took writes adds 0.35 to 0.6 M (an overlay that keeps an order array adds 12 to 15 k). Leave the cut automatic, and set `key_cut = 'off'` on an index whose queries also order or range by another column.

## Several indexes over the same columns

A second index over columns another index already lists stores their leaves again. What it adds depends on the layout, the key order, the cut, the roll-ups and the order arrays that the build makes of the rows (`key_order`, `key_cut`, `rollup_columns`, `order_array_columns`, `order_array_group`, `order_layout`) and on the predicate.

The figures below are the instructions of one backend (`perf stat`, warm, no workers) and the buffers of `EXPLAIN (ANALYZE, BUFFERS)`, on a generated table `events (id, dim_a_id, tenant_id, created_at, kind, status, value_num, qty, label, country, score)` of 800,000 rows (200,000 in the second column of each table), `track_changes = off`, one index at a time; sizes are the bytes of the segments (`sift.col_index_info`). They are not the store dataset of the page above.

**The same layout: one wider index.** A narrow index `(dim_a_id, created_at, kind, value_num)` and a wide one with five more columns, both `key_order = 'dim_a_id, created_at'`, hold the same order arrays, runs and key lists: the narrow one is only a smaller copy of the wide one.

| | 800,000 rows | 200,000 rows |
|---|---|---|
| narrow + wide, two indexes | 32.4 MB | 8.0 MB |
| the wide index alone | 18.5 MB (-43%) | 4.6 MB (-43%) |
| an aggregate by `dim_a_id` on the narrow / the wide index | 0.61 M, 18 blocks / 0.64 M, 19 | 0.56 M, 19 / 0.58 M, 19 |
| the same over a month of `created_at` | 1.09 M, 34 / 1.13 M, 36 | 1.01 M, 34 / 1.04 M, 34 |

The queries of the narrow index cost the same on the wide one, so drop the narrow one. `CREATE INDEX` and `REINDEX` say so in a NOTICE when another `sift_col` index of the table has the same predicate and layout and lists some of the same columns; it is advice, the index is built as written, and `SET client_min_messages = warning` hides it. Two indexes whose automatic choices differ (the build gave each its own `rollup_columns`) have different layouts and are not named: write the options of one as the other has them, or keep both.

**Different layouts, one index.** Two keys read in different ways (the rows of one `dim_a_id`, the rows of one `tenant_id`) want two key orders; a second index gives each its own cut, and the units of the one that is not cut by the key are read whole. The order arrays hold a second order over the same leaves, so a second key's top-N needs no second cut:

```sql
CREATE INDEX events_col ON events USING sift_col (dim_a_id, tenant_id, created_at, kind, status, value_num, qty)
  WITH (key_order = 'dim_a_id, created_at', order_array_group = 'tenant_id');
```

| | 800,000 rows | 200,000 rows |
|---|---|---|
| one index cut by `dim_a_id` + one cut by `tenant_id` | 13.2 + 11.8 = 25.0 MB | 3.4 + 3.0 = 6.4 MB |
| one index cut by `dim_a_id`, grouped by `tenant_id` | 12.0 MB (-52%) | 2.9 MB (-54%) |

| query (instructions, buffers) | cut by `dim_a_id` | cut by `tenant_id` | one index, grouped by `tenant_id` |
|---|---|---|---|
| `dim_a_id = X`, group by `kind` | 0.63 M, 20 | 10.12 M, 610 | 0.63 M, 20 |
| `dim_a_id` range and a month | 1.12 M, 36 | 24.36 M, 1131 | 1.12 M, 37 |
| `tenant_id = Y`, group by `kind` | 8.25 M, 509 | 1.24 M, 19 | 8.25 M, 509 |
| `tenant_id = Y` and a month | 10.02 M, 1028 | 1.09 M, 34 | 10.02 M, 1027 |
| `tenant_id = Y`, a filter, group by `qty` | 8.24 M, 524 | 1.15 M, 19 | 8.24 M, 524 |
| `tenant_id = Y ORDER BY created_at DESC LIMIT 20` | 17.10 M, 624 | 0.60 M, 48 | 0.61 M, 51 |
| group by `kind`, no condition | 33.17 M, 397 | 33.13 M, 392 | 33.17 M, 397 |

- **The top-N and the list of the second key are as cheap as on its own index** (0.61 M against 0.60 M instructions), and everything of the first key is unchanged.
- **The aggregates of the second key are dearer, and grow with the rows.** The scan reads every unit that holds a row of the tenant: 6.7 to 9.2 times the instructions of an index cut by the tenant at 800,000 rows (2.7 to 3.1 times at 200,000), where that index reads 19 buffers at both sizes. Use the one index when the second key is read for its latest rows, its top-N or a rare aggregate; keep two indexes when its aggregates are frequent or the table is large.
- The columns of the second key are in the index once, for both.

## Named projections: the columns of an index, defined once

An index is what it stores: columns, derived expressions, a filter (a partial index), the roll-ups and a few options. When several indexes of a table repeat that list, a change reaches some of them and not the others. A projection is that list under a name, kept in a table of the extension (`sift.projections`, dumped with the database), and `sift.projection_index` makes the index from it, or finds that an index already holds it and builds none.

```sql
SELECT sift.projection_create('event_dims', 'events', ARRAY['dim_a_id', 'tenant_id', 'created_at', 'kind', 'status', '(qty / 10)', 'value_num'],
                              rollups => 'dim_a_id, kind');
SELECT sift.projection_create('event_kinds', 'events', ARRAY['dim_a_id', 'kind', '(qty / 10)']);
SELECT sift.projection_index('events_col', ARRAY['event_dims'], '{"key_order": "dim_a_id, created_at"}');   -- builds events_col
SELECT sift.projection_index('events_kinds', ARRAY['event_kinds'], '{"key_order": "dim_a_id, created_at"}'); -- builds nothing: events_col holds it
SELECT * FROM sift.projection_status;
```

`sift.projection_create(name, table, columns, rollups, filter, options)` checks the definition on an empty copy of the table (a column that does not exist, an expression that is not immutable, a bad filter or roll-up is refused there). `sift.projection_index(index, projections, layout)` takes one projection or several of one table, with the same filter, roll-ups and options (the index holds the union of their columns, once each), and the layout of this index: the options that cut and order its rows (`key_order`, `key_cut`, `order_array_group`, ...) and the others that belong to one index (`track_changes`). It returns the index it made or found. The index lists the names in its option `projection`, which `pg_dump` writes with the index; `sift.projection_index_sql` returns the statement instead, for `CREATE INDEX CONCURRENTLY`.

**One index serves a projection** when it is a `sift_col` index of the table that is valid, lists every column and expression of the projection, keeps every row of its filter (the filter implies the predicate of the index) and either keeps no other row or lists the columns the filter reads, rolls up the same columns and names the measures the projection names, has the options the projection sets, and cuts and orders its rows as the layout says. Nothing is stored for the second projection: the planner reads the index for its queries as it does for any index that holds the columns, so the answers and the cost are those of an index of its own.

| 1,000,000 rows, 12 columns | an index of its own | served by the wide index |
|---|---|---|
| size of the second index | 11.1 MB | none (the pair 30.4 MB, the wide one alone 19.3 MB: -36%) |
| build of the second index | 6,613 M instructions | none (the call that finds the wide index 10.7 M) |
| one entity, group by `qty / 10` | 1.67 M instructions, 302 buffer hits (no SiftAgg plan) | 0.70 M, 25 |
| an entity range | 3.98 M, 57 | 4.01 M, 60 |
| one tenant | 10.23 M, 622 | 10.25 M, 616 |
| no condition | 41.36 M, 487 | 41.38 M, 476 |

**Different layouts share the declaration, not the data.** An index cut by another key reads its own queries 8 to 19 times cheaper than a shared store would (the table of the previous section): `sift.projection_index` with another `key_order` builds a second index from the same definition, and the definition is the one place to change.

**Replacing and dropping.** A projection that an index lists cannot be deleted, renamed or change its table, columns or filter (`sift.projections` has a trigger, so a direct `DELETE` or `UPDATE` is refused too): define the changed one under another name, make its indexes with `sift.projection_index_sql`, then drop the old indexes and the old definition (or `ALTER INDEX .. RESET (projection)` first). New roll-ups and options (`sift.projection_replace`) are set on the indexes that list only that projection, and a NOTICE says to `REINDEX` them; an index that lists other projections too is left as it is.

**What the status says.** `sift.projection_status` lists each projection with the indexes that list it: `ok`, or `broken` with the reason: the index lost a column, its predicate no longer covers the filter, the roll-up or an option changed, the index is not valid, a name that nobody defined, or no index at all. A broken projection changes no answer: the queries take the plan they would without the index, with the same rows (the aggregate scan proves what it reads against the index it reads).

**Dump, restore and pg_upgrade.** The definitions are extension configuration, dumped as data, and every index keeps its names in its options. A restore (a plain dump through psql, `pg_restore`, serial or `-j`) creates the indexes with or without the definitions there yet and a `pg_upgrade` creates them before the data is back: the names are checked by the status and never by `CREATE INDEX`, so none of them fails, and the status is `ok` once the data of the definitions is restored. A restore of 1,100,000 rows with two indexes of one definition took 4.9 s with the projections and 4.7 s without (the index builds are the time); the definitions are 873 bytes of the dump.

**A value derived once.** An expression in the key list (`(qty / 10)`) is computed again in every index that lists it: 185 instructions a row for an integer, 920 for a text value, in each build. A stored generated column computes it once, at the write, and every index lists the column: `ALTER TABLE events ADD COLUMN bucket int GENERATED ALWAYS AS (qty / 10) STORED`, then `... (dim_a_id, bucket)`. The aggregate scan reads the column as any other leaf (group by, filter, sums) and an index-only scan returns it, as the same query on an expression leaf; queries must name the column, a repeated expression does not use it. It pays when many indexes or a b-tree or the queries share the value: 100,000 inserts cost 1,800 more instructions a row for an integer column (+14%, heap +8.4% at 1,000,000 rows) and 3,000 for a text one (+22%, WAL +4.6%, heap +13.7%). For one or two indexes the expression leaf is cheaper.

## What writes cost

100,000 rows against the 1,000,000-row table with its three btrees, the first set having no `sift_col` index (time and WAL, minimum of three runs):

| statement | btrees only | lean | wide |
|---|---|---|---|
| insert 100,000 rows | 614 ms, 67.4 MB | 943 ms, 77.0 MB | 982 ms, 79.1 MB |
| update `amount` of 100,000 rows | 2,822 ms, 97.8 MB | 3,886 ms, 110.9 MB | 3,745 ms, 117.2 MB |
| update `title` of 100,000 rows | 2,438 ms, 105.1 MB | 2,466 ms, 117.9 MB | 3,166 ms, 122.7 MB |
| delete 100,000 rows | 61 ms, 19.1 MB | 171 ms, 22.5 MB | 136 ms, 22.5 MB |

The lean index adds 1.5 times the time and 1.14 times the WAL to an insert, and 1.1 to 1.2 times the WAL to an update. The update of a column the index does not hold (`title` against the lean index) takes the time of the table alone and writes 13 MB more WAL; the one that changes an indexed column also takes 1.4 times as long. Size and WAL grow with the number of columns, so each column is worth the queries that use it.

## When to keep a btree

- **Keep** the btrees of point lookups: the primary key and the foreign keys a query looks rows up by (`orders (customer_id)`). `SELECT * FROM orders WHERE id = 424242` takes 0.044 ms through the primary key and 0.307 ms with the `sift_col` index alone; the 18 orders of one customer take 0.10 ms through the btree and 1.6 ms through the `sift_col` index. See [COL-SCAN.md](COL-SCAN.md#replacing-a-btree) for what a btree can and cannot be replaced by.
- **Do not create** a btree for a column the `sift_col` index holds to speed up a count, a group-by or a filter: the aggregate scan answers it from the index. The three btrees of the first set above left all seven queries on a sequential or bitmap scan.
- **Drop** an older `sift_col` index whose columns the new one holds, once the queries run on the new one.

## A LEFT JOIN from a parent to its children

For `parent LEFT JOIN child ... GROUP BY` with an aggregate over the child's columns, the scan reads the child and takes the parent as its lookup table, whether the parent has a `sift_col` index or not (4.2.x did not try the child as the scan when both tables were indexed: 491, 631, 191, 591 and 125 ms for the first five statements below). Measured on 100,000 parents and 1,000,000 children, release 4.3, one backend without workers, minimum of five (normal plan: three):

```sql
CREATE TABLE parent (id int PRIMARY KEY, grp int, state text, rank int);
INSERT INTO parent SELECT g, g % 40, (ARRAY['ca','ny','tx','wa','fl'])[1 + g % 5], g % 100 FROM generate_series(1, 100000) g;
CREATE TABLE child (id int PRIMARY KEY, parent_id int, kind text, at timestamp, amount numeric(8,2));
INSERT INTO child SELECT g, 1 + (g::bigint * 7919) % 100000, (ARRAY['a','b','c','d'])[1 + g % 4],
       timestamp '2024-01-01' + (g % 365) * interval '1 day', (g % 1000)::numeric FROM generate_series(1, 1000000) g;
CREATE INDEX child_parent_idx ON child (parent_id);
CREATE INDEX child_col ON child USING sift_col (id, parent_id, kind, at, amount);   -- the child alone
CREATE INDEX parent_col ON parent USING sift_col (id, grp, state, rank);            -- and the parent
```

| statement (`parent p LEFT JOIN child c ON c.parent_id = p.id` unless said) | normal plan | child indexed alone | both indexed |
|---|---|---|---|
| `count(c.id)` per parent, ten largest | 548 ms | 84 ms | 60 ms |
| `count(c.id)` per `p.grp` | 316 ms | 50 ms | 50 ms |
| `sum(c.amount)` per `p.grp`, with `c.kind = 'a' AND c.at >= ..` in the `ON` | 117 ms | 17 ms | 14 ms |
| `count(DISTINCT c.kind)` per `p.grp` | 734 ms | 42 ms | 43 ms |
| `sum(c.amount)` per `p.state` with `WHERE p.rank < 5` | 69 ms | 43 ms | 53 ms |
| `WHERE EXISTS (SELECT 1 FROM child c WHERE c.parent_id = p.id AND c.amount > 990)` per `p.grp` | 58 ms | 57 ms | 72 ms |

- **Advice.** Index the child. The parent needs an index only for the statements of its own that read its columns; check the statements above with `EXPLAIN`.
- **A filter of the parent** (the fifth row) is the shape closest to the normal plan: check it with `EXPLAIN (ANALYZE)` and `SET sift.enable_agg = off`.
- **`EXISTS` and `NOT EXISTS` from the parent to a child with conditions** keep the normal plan in every set (the differences of the last row are the noise of the host).

## Keys above the fact table

A request often carries a key that lives above the fact table: a tenant, a group, a plan. The facts reach it through a *scope relation*, a table of unique `(member key, scope key)` pairs (the ids a tenant sees, the ids of a group). An index keyed by the member answers such a request by expanding the pairs and testing every fact row against the set of ids, so its cost follows the table, not the tenant. A *scope copy* is a copy of the fact table that holds each fact once for every scope key of its member, with a `sift_col` index keyed by the scope key: the cost then follows the rows of the key. The same request can also be answered by a *scope index*, a `sift_col` index that keeps the rows replicated by scope key inside the table's own index
(EXPERIMENTAL in 4.4, no second table and no triggers on the facts; [COL-SCOPE](COL-SCOPE.md#scope-index-or-scope-copy) says when to use which, with the sizes and the build WAL of both on one fixture).

```sql
-- facts(id primary key, dim_a_id, created_at, kind, amount); scope_pairs(dim_a_id, tenant_id) with a unique index on both
SELECT sift.scope_create('facts_by_tenant', 'facts', 'dim_a_id', 'scope_pairs', 'tenant_id');
SELECT kind, count(*), sum(amount) FROM facts_by_tenant WHERE tenant_id = 38 GROUP BY kind;
SELECT * FROM sift.scope_verify('facts_by_tenant');   -- no rows: the copy equals the join
SELECT sift.scope_drop('facts_by_tenant');
```

`sift.scope_create` creates the copy (the scope key first, then the carried columns: those of the table's `sift_col` index, all of them when it has none), the `sift_col` index with `key_order = 'tenant_id, created_at'` (the first date or timestamp column; `order_column` names another), a btree on the copy's primary key (a change of a fact finds all its copies by it; a pair that goes is found by the `sift_col` index on the scope key and the foreign key), and the triggers that keep the copy exact in the transaction of each change: statement triggers with transition tables on the fact table (insert, update, delete, truncate) and on the scope relation (the same). The fact table needs a primary key, and a btree on the foreign key keeps a membership edit from scanning it. `partition => true` makes the copy a LIST partition per scope key. A prepared statement or a driver's Bind (`WHERE tenant_id = $1`) keeps the scan on either form: the partitioned copy reads the one partition the parameter selects when the statement starts and its generic plan is priced for one partition of the average size, so PostgreSQL takes it from the sixth execution as it does for the single copy.

### Columns and options of the copy

- **Columns.** When the table has a `sift_col` index, the copy carries its columns (plus the primary key and the foreign key); `columns => ARRAY[..]` replaces them. A table without one is copied whole, and a NOTICE says how many columns and the size of the copy's table (a column nobody reads costs its bytes in every copy of a row).
- **Options.** The copy inherits the options of that index that describe columns: `paths`, `compression`, `preagg`, `rollup`, `rollup_dims`, `rollup_leaves`, `rollup_columns`, `rollup_on_demand`, `calendar_columns`, `cluster_advice`, `cube_minmax`, `dict_lookup`, `array_lengths`, `hll_columns`, `return_columns`, `superunit`, `write_buffer_limit`, `dead_rewrite_pct` and `vacuum_truncate`. It does not inherit what is keyed on the table's own key: `key_cut`, `key_cut_rows`, `key_columns`, `key_table`, `key_order`, `order_columns`, `order_layout`, `order_array_columns`, `order_array_group`, `point_columns`, `point_tid_columns`, `count_columns`, `track_changes` and `connect`; the build derives those again for the scope key. A value the build chose from the documents (`paths = 'auto: ..'`) is not inherited either: the copy reads the documents again.
- **`options`.** A reloption list added to the copy's `CREATE INDEX .. WITH (..)`: `options => 'paths = ''metrics_a.* float8, notes.k int4'''`. An option it names replaces the inherited one; `key_order` replaces the scope key and order column the copy sets. A jsonb column whose documents have no leaf (`'[]'`) needs `paths` (or is left out with `columns`); a map read with `jsonb_each` (`metrics_a.* float8`) is served only when the index declares the wildcard, so declare it on the table's index and the copy takes it. An inherited option that names a column `columns` left out fails at `CREATE INDEX`: name the option in `options` too.

### Vacuum the copy

`sift.scope_create` analyzes the copy and its scope relation (a pair table that was only filled has no statistics until autovacuum, and the statements of the triggers are then planned for far more rows than they get; `sift.scope_refresh_pairs` and `sift.scope_follow_pairs` analyze the pair table after a full refresh, and the probe of the copy that the insert makes is an index probe of the maintenance index whatever the statistics say; `analyzed` of `sift.scope_status` is false while the copy or its scope relation has none: run `ANALYZE` on them) but cannot vacuum the copy: `VACUUM` cannot run inside a function, and nothing else sets the visibility map of a table written by `INSERT`. Run `VACUUM <copy>` right after it (the NOTICE names the command). Until then a read checks every row it keeps against the heap: a statement that reads 66,000 rows of a 200,000-row copy takes 20.1 M instructions, 5.7 ms and 859 buffer hits before the `VACUUM` and 1.98 M, 1.0 ms and 49 after (PostgreSQL 18.6, one backend, warm). `SELECT * FROM sift.scope_status` lists each copy's pages and all-visible pages and whether it was vacuumed; autovacuum vacuums a table that was inserted into, after its naptime.

### The contract of a read

- A copy is read **one key at a time**: `WHERE tenant_id = $1`. A fact is in the copy once for each key of its member (ids in no key are in none).
- `WHERE tenant_id IN (1, 2)` counts a fact of a member in both keys twice (the rows, counts and averages are wrong by the share of such facts). The exact read over several keys counts a fact under the **first requested key that holds its member**; `sift.scope_read_keys(copy, keys)` returns that condition (`alias` names the copy in your query), so nobody writes it by hand:

```sql
SELECT sift.scope_read_keys('facts_by_tenant', ARRAY[38, 1]);
-- (public.facts_by_tenant.tenant_id = 38) OR (public.facts_by_tenant.tenant_id = 1 AND NOT EXISTS
--   (SELECT 1 FROM public.scope_pairs scope_p WHERE scope_p.dim_a_id = public.facts_by_tenant.dim_a_id AND scope_p.tenant_id = ANY ('{38}'::bigint[])))
SELECT kind, count(*), sum(amount) FROM facts_by_tenant WHERE <that condition> GROUP BY kind;   -- the same answer as the member path, in any key order
```

  The member path (`dim_a_id IN (SELECT dim_a_id FROM scope_pairs WHERE tenant_id IN (1, 2))`) and a `DISTINCT` on the primary key are the other exact forms. A key listed twice counts once; no key reads no row.
- `GROUP BY tenant_id` counts a fact in each of its keys.
- A fact delete or update changes all of its copies; a pair that appears, goes or moves changes the rows of its member in the keys concerned.

### Exactness

The copy equals the join while its triggers are enabled and the tables are not written by a logical-replication apply worker (`sift.scope_create` refuses a subscription target: the apply worker fires no statement trigger). `ALTER TABLE .. DISABLE TRIGGER` and `COPY` with triggers off leave the copy behind; `sift.scope_verify(copy)` lists the rows that are `missing` from the copy, are `extra` in it, `differs` in a carried column or are a `duplicate` (a fact twice in a key). `sift.scope_verify(copy, keys)` checks only those keys and also the contract of a read over them: the facts `sift.scope_read_keys` reads, each once, are the facts of the members of the keys (`multi_key_missing`, `multi_key_extra`). `sift.scope_replicas(copy)` is the histogram of the copies each fact has (`copies_per_fact`, `facts`; 0 for a fact that reaches no key), the multiplicity the cap below limits. The roles that write the fact table or the scope relation need the privileges of the copy (the triggers run as the writer).

A transaction that changes the facts does not see the uncommitted pairs of another one, and the other way round, so the triggers of the two tables exclude each other: the writers of the facts share a lock of the copy until they commit, a writer of the pairs takes it alone. A change of the pairs waits for the open transactions that wrote the facts, and the facts wait while a transaction that changed the pairs is open; keep such transactions short. Two transactions that each change the facts and then the pairs (a statement on each table, or a data-modifying CTE) can deadlock, each waiting for the other's shared hold of the lock: PostgreSQL ends exactly one with a deadlock error (`40P01`), the other commits and the copy is exact (`test/isolation/specs/col_scope_copy_deadlock.spec`); retry the one that failed, and change the pairs before the facts in every transaction that changes both to avoid it. A `REPEATABLE READ` or `SERIALIZABLE` transaction is refused (`scope copies need READ COMMITTED writers`, SQLSTATE 0A000, the statement changes nothing): its triggers would read the snapshot of the transaction, which does not see the pairs another transaction committed after it began, and leave the copy inexact. Change the tables of a copy in `READ COMMITTED`, or drop the copy with `sift.scope_drop`. A subscription created after the copy leaves it behind the same way a subscription target would (the apply worker fires no trigger; `CREATE SUBSCRIPTION` has no hook to refuse it): `sift.scope_verify` names the cause with a first row `logical replication target` whose `row_key` has the table, next to the `missing`, `extra` or `differs` rows the writes of the worker left. Rebuild the copy with `sift.scope_drop` and `sift.scope_create` once the subscription is gone.

### What the copy inherits

The copy carries the columns of the table's `sift_col` index (or the `columns` asked for, and always the primary key and the foreign key) and inherits the options of that index that describe them: `paths`, `compression`, `preagg`, `rollup_columns`, `rollup_leaves`, `calendar_columns`, `hll_columns`, `return_columns`, and those that name no column (`cube_minmax`, `superunit`, `dict_lookup`, `array_lengths`). Each option that names columns is restricted to the carried ones: an entry of `paths` (`doc.a float8`, `extra[]{k text}`), of `compression` (`kind=zstd:3`) or of a list of columns that names a column the copy leaves out goes, the measures of a `cube` or of `rollup_columns` that are not carried go, and a rollup or a cube dimension whose columns are not all carried goes whole. An option with nothing left is not inherited and a NOTICE lists it, so the copy chooses it again from its own columns (`auto`). Words (`auto`, `off`, `all`) stay as they are. The keys of the table's own key (`key_order`, `key_columns`, `order_*`, `point_*`, `count_columns`, `track_changes`, `connect`) are derived again for the scope key, and an option given in `options` replaces the inherited one.

### The multiplicity cap

A fact is stored once per key of its member, so the copy grows with its *multiplicity*: the rows of the copy over the rows of the table that reach a key (a fact whose member is in 3 keys has 3 rows). It is a property of the data of the keys, and `sift.scope_create` measures it before it builds (`sift.scope_replicas(copy)` is the histogram), says it in a NOTICE with the share of those rows that have 2 or more copies, the size of the copy with its indexes and its growth over the table and its indexes, and **refuses a scope relation above `max_copies`, 1.8 copies per fact by default**, with the size and growth it estimated (the row width of the table for each copy row, and 0.4 of that again for the `sift_col` index and the maintenance btree: 0.34 to 0.44 from 2 to 6 copies per fact below). A copy is the form for a multiplicity close to 1; above it, the join answers (the read through the entity path), and the refusal points to the derived-key scope index (the `scope` option of the `sift_col` index, docs/COL-SCOPE.md, which has its own multiplicity rule) and to the count and sum cube recipe of this page. The refusal is a default, not a ceiling: `sift.scope_create(.., max_copies => 6)` builds the copy anyway, and the NOTICE prints the multiplicity, the share of rows in 2 or more keys, the most keys of one member, the size and the growth that were built. A `max_copies` below 1 or NULL is an error.

Where 1.8 comes from. The program's formula weighs the benefit `B` (the mean over the nine read classes of `log2(S) / log2(1.05)`, `S` = join instructions over copy instructions) against the cost `C = G/3 + W/5 + 8` (`G`: the growth of the table and its indexes in %, `W`: the extra instructions of inserting 10,000 facts in %, 8 for the code and the one new concept). The same 2,000,000 facts, 20,000 ids and 60 tenants of skewed size with pairs of 1.14, 2, 3 and 6 copies per fact (10%, 50%, 75% and 100% of the facts' members in 2 or more keys, at most 6; PostgreSQL 18.6, one backend, serial; answers equal in every read, the copy exact by `sift.scope_verify` and with no duplicate row):

| copies per fact | copy rows | heap, `sift_col`, maintenance btree (MB) | growth over table + indexes | build (s, WAL MB) | insert 10,000 facts: M instructions, ms, WAL MB (records / images) | 100 members gain / lose a key: M instructions, ms, WAL MB | B | C | B - C |
|---|---|---|---|---|---|---|---|---|---|
| 1.14 | 1,812,118 | 139, 30, 39 | +85% | 4.9, 248 | 887 (658 without), 185, 25.8 (4.7 / 20.9) | 621, 178, 38.5 / 543, 114, 50.1 | 61.2 | 43.4 | +17.8 |
| 1.98 | 3,149,800 | 241, 51, 55 | +143% | 8.3, 432 | 1071 (664 without), 196, 27.2 (6.1 / 20.9) | 648, 159, 48.0 / 706, 160, 58.6 | 64.3 | 67.8 | -3.5 |
| 3.01 | 4,795,463 | 367, 76, 70 | +211% | 11.3, 629 | 1298 (648 without), 259, 29.1 (7.7 / 21.1) | 634, 286, 55.3 / 686, 177, 64.9 | 63.2 | 98.3 | -35.2 |
| 6.00 | 9,560,268 | 732, 144, 107 | +404% | 20.4, 1190 | 1948 (645 without), 365, 34.3 (12.4 / 21.7) | 686, 306, 63.8 / 1421, 614, 71.3 | 55.4 | 183.1 | -127.7 |

`B` does not grow with the multiplicity (55 to 64: a bigger tenant holds more rows in both paths) while `G` and `W` do, so `B - C` is +17.8 at 1.14 copies per fact, -3.5 at 2 (the sign changes at 1.84 by linear interpolation), -35 at 3 and -128 at 6. `B` does not depend on the rows of the table at a fixed share of the tenants either (2 copies per fact, the same read classes: 56.4 at 250,000 facts, 49.9 at 500,000, 58.2 at 1,000,000 and 64.3 at 2,000,000); that spread of 7 moves the sign change by 0.3 copies per fact, so the cap is one constant. By the formula a copy is a win up to 1.8 copies per fact and a loss above it, so 1.8 is the default of `max_copies`. Above it the formula turns negative, and building the copy is the caller's decision: a copy that is read far more than it is written, or a scope of a few large members that reach 2 or 3 keys, can still be worth its size. Expect the copy rows and the growth of the table above (+143% at 2 copies per fact, +211% at 3, +404% at 6 of the table and its indexes), 1.6, 2.0 and 3.0 times the instructions of inserting 10,000 facts without the copy at 2, 3 and 6 copies per fact, and the build time in proportion to the copy rows; the refusal and the NOTICE give the figures of your own table. Free space is not checked.

### What it measures

The reads of the table above, absolute numbers: the join is the fact index and `dim_a_id IN (SELECT .. FROM scope_pairs WHERE tenant_id = $1)`; a copy is read `WHERE tenant_id = $1`. The tenants are the biggest, the 30th and the 55th by their ids (2,382, 216 and 159 ids at 1.14 copies per fact; 3,716, 385 and 279 at 2; 5,308, 584 and 429 at 3; 9,338, 1,211 and 890 at 6). Each cell is warm M instructions, ms and buffer hits of the join, then of the copy; after the semicolon the blocks read from storage by the first run after a restart of the cluster (the join, then the copy). Instructions are those of the backend (median of 3); the milliseconds are one host's wall clock and noisy.

| statement | 1.14 copies per fact | 1.98 copies per fact | 3.01 copies per fact | 6.00 copies per fact |
|---|---|---|---|---|
| big, distinct month | 339.6, 70.1, 7,408 -> 58.30, 15.60, 2,773; 6,627 -> 3,050 | 904.9, 152.2, 36,897 -> 90.94, 16.83, 4,335; 7,175 -> 4,678 | 979.2, 201.3, 38,322 -> 131.20, 37.49, 6,452; 8,252 -> 6,316 | 1056.1, 146.6, 39,327 -> 206.41, 54.43, 6,686; 9,212 -> 6,878 |
| big, latest20 | 41.1, 15.2, 2,312 -> 0.65, 0.44, 59; 2,402 -> 354 | 39.9, 12.4, 1,884 -> 0.71, 0.32, 64; 1,707 -> 469 | 37.2, 16.6, 1,687 -> 0.82, 0.49, 80; 1,588 -> 112 | 48.9, 140.9, 2,097 -> 1.15, 0.39, 117; 2,305 -> 519 |
| big, sum kind | 324.1, 61.0, 7,528 -> 40.73, 17.17, 2,634; 6,561 -> 2,906 | 433.6, 127.3, 8,705 -> 63.40, 18.13, 4,149; 7,124 -> 4,495 | 544.4, 121.2, 10,189 -> 91.11, 36.59, 6,197; 8,296 -> 5,982 | 675.9, 117.5, 11,189 -> 132.45, 37.41, 6,190; 9,301 -> 6,463 |
| mid, distinct month | 98.2, 23.6, 3,458 -> 6.50, 1.78, 386; 3,609 -> 681 | 840.6, 123.5, 34,288 -> 8.60, 2.67, 484; 5,159 -> 878 | 897.6, 325.8, 35,637 -> 12.71, 5.84, 906; 6,538 -> 875 | 934.1, 75.4, 36,916 -> 43.96, 5.22, 1,447; 8,548 -> 1,744 |
| mid, latest20 | 25.1, 5.7, 1,794 -> 0.64, 0.25, 57; 1,844 -> 350 | 37.8, 10.8, 2,270 -> 0.68, 0.40, 60; 1,848 -> 465 | 36.8, 20.6, 2,248 -> 0.78, 0.59, 77; 1,894 -> 110 | 46.7, 10.9, 3,128 -> 0.96, 0.29, 105; 3,006 -> 516 |
| mid, sum kind | 96.0, 15.2, 3,523 -> 4.10, 1.36, 341; 3,746 -> 641 | 174.2, 28.5, 5,321 -> 4.95, 1.91, 452; 5,369 -> 851 | 240.2, 70.8, 6,985 -> 7.60, 3.27, 836; 7,021 -> 791 | 316.8, 46.0, 8,809 -> 34.22, 4.94, 1,351; 8,690 -> 1,648 |
| small, distinct month | 75.8, 17.5, 2,713 -> 5.64, 1.88, 318; 2,865 -> 616 | 148.4, 31.5, 4,556 -> 20.27, 4.70, 817; 4,394 -> 1,203 | 218.8, 69.9, 6,349 -> 21.82, 5.77, 979; 6,212 -> 981 | 292.2, 41.9, 8,146 -> 39.15, 8.58, 1,289; 8,218 -> 1,572 |
| small, latest20 | 26.6, 7.5, 1,774 -> 0.62, 0.78, 53; 1,715 -> 350 | 30.8, 7.8, 1,900 -> 0.76, 0.31, 103; 1,549 -> 493 | 39.5, 16.5, 2,387 -> 0.91, 0.60, 111; 1,841 -> 128 | 47.1, 9.9, 3,184 -> 0.95, 0.28, 105; 3,047 -> 517 |
| small, sum kind | 73.9, 19.1, 2,764 -> 3.75, 1.52, 300; 2,958 -> 603 | 145.8, 32.7, 4,645 -> 17.42, 55.29, 753; 4,665 -> 1,143 | 215.3, 55.6, 6,457 -> 17.91, 5.65, 941; 6,467 -> 940 | 285.7, 42.0, 8,303 -> 30.87, 6.08, 1,192; 8,349 -> 1,491 |

What it costs is in the cost table above: the copy of the 1.14 table is 139 MB of heap (76.5 B for each of its 1,812,118 rows), a 30 MB `sift_col` index (16.4 B a row) and a 39 MB btree (21.6 B a row), 207 MB beside 243 MB of table and indexes (+85%); inserting 10,000 facts takes 887 M instructions against 658 M (+35%) and 25.8 MB of WAL against 23.9 MB; deleting 1,000 facts 1,069 M against 1,024 M and updating 1,000 1,195 M against 1,084 M; a fact table without the copy is not slowed on reads.

Updating facts costs the rows that changed, found by a set difference of the transition tables: linear under any planner estimate. The statement it replaced joined the two transition tables and could flip to a nested loop with a join filter, quadratic (59.6 G instructions at n = 4,000 facts with hash and merge joins off). The difference reads and compares every carried column of the rows an update touches, so a wide column costs on every update of a fact, and the update rewrites each of the fact's copies. Updating n facts of a 100,000-fact table with 1.6 copies per fact, M instructions (PostgreSQL 18.6, one backend, median of 3):

| copy | n | previous statement | set difference | over |
|---|---|---|---|---|
| wide carried json (~1.5 KB documents) | 1,000 / 4,000 / 10,000 | 161.5 / 815.6 / 2,527 | 242.6 / 1,149.2 / 3,278 | +50% / +41% / +30% |
| narrow rows | 1,000 / 4,000 / 10,000 | | | +1% to +4% |

When the report does not read a wide json column, leave it out of the copy with the `columns` list of `sift.scope_create`: a narrower copy keeps the update cost near the narrow row.

### The maintenance btree

A change of a fact finds its copies by the primary key, and an insert checks that a change of the other table in the same statement has not made the copy already (a statement that changes a pair and a fact of its member). The alternatives measured on the 1,812,118 rows of the 1.14 table, and the ones that kept their rank at 2, 3 and 6 copies per fact (WAL after a checkpoint):

| design | btree MB at 1.14 / 2 / 3 / 6 copies per fact | bytes per copy row | insert 10,000 facts, M instructions at 1.14 / 2 / 3 / 6 | WAL MB (full-page images) at 1.14 |
|---|---|---|---|---|
| unique `(dim_a_id, id, tenant_id)`, `ON CONFLICT DO NOTHING` | 73.5 | 40.6 | 1,007.5 | 57.8 (51.4) |
| unique `(id, tenant_id)`, `ON CONFLICT DO NOTHING` | 57.1 / 99.3 / 151.2 / 301.4 | 31.5 | 950 / 1,173 / 1,439 / 2,230 | 26.2 (20.2) |
| **`(id)`, a probe of the btree for each row (`LATERAL ... LIMIT 1`)** | **39.1 / 55.3 / 69.9 / 107.4** | **21.6 / 17.6 / 14.6 / 11.2** | **887 / 1,071 / 1,298 / 1,948** | **25.8 (20.9)** |
| `(id)`, `NOT EXISTS` (an anti join) | 39.1 / 55.3 / 69.9 / 107.4 | 21.6 / 17.6 / 14.6 / 11.2 | 857 / 4,785 / 6,916 / 15,578 | 21.5 (16.5) |
| the table without a copy | | | 658 | 23.9 (20.9) |

The foreign key first spreads the rows of a new fact over random pages (51 MB of full-page images after a checkpoint); the primary key appends them at the right edge. The plain btree is deduplicated (the copies of a fact share one entry), so it shrinks per copy row as the multiplicity grows, where the unique one stays at 31.5 B. The `NOT EXISTS` form lets PostgreSQL choose an anti join, and from 2 copies per fact it hashes the whole copy for 10,000 facts (4,785 M instructions and 1,171 ms against 1,071 M for the probe, 62,872 buffer hits); the lateral probe is an index probe whatever the statistics. Deleting 1,000 facts (999 to 1,069 M instructions at 1.14 across the designs) and updating 1,000 (1,166 to 1,219 M) cost the same in all of them. The price of the lean design: a pair edit writes more full-page images (100 members gaining a key: 38.5 MB of WAL against 11.8 MB with the three-column unique btree), and a pair that goes takes 2x the instructions (the `sift_col` index finds its rows instead of the btree: 543 M against 270 M). By the formula (`G/3 + W/5`, the cost table above) the lean design costs 35 at 1.14 copies per fact (+85% / 3 + 35% / 5) against 46 for the first (+99% / 3 + 65% / 5).

### When not to use it

A fact that reaches many keys (above 1.8 copies per fact on average: `sift.scope_create` refuses it unless `max_copies` is raised, and the formula says the copy then costs more than it gains: read it through the join, the `scope` option of the `sift_col` index or the count and sum cube recipe there), a scope key of text or uuid (use an integer key), a scope that changes by the expiry of a date with no write when it is not kept from a view (see below: `sift.scope_expire_pairs`), a restricted user inside a tenant (filter the user's ids inside the tenant's rows: the copy gains little when the user's set is a small part of it), and tables small enough that the join answers in a few milliseconds.

## Keeping the pairs from a view

The scope relation is often not a table anyone writes but the answer of a view: the ids a tenant sees are the ids it is assigned directly, plus the ids of the groups it is assigned (`assignments` and `group_members`), minus the rows soft deleted (`deleted_at`) and the assignments that have expired (`expires_at`). Reading it through the view expands the assignments and the group membership for every request. A table of the pairs, kept equal to the view, makes that expansion one index range, and a scope copy built on it (above) is kept equal to the join by the same transaction.

```sql
CREATE VIEW visible_dim_a AS   -- (dim_a_id, tenant_id): the member first, the scope key second
	SELECT a.dim_a_id, a.tenant_id FROM assignments a WHERE a.dim_a_id IS NOT NULL AND a.deleted_at IS NULL AND (a.expires_at IS NULL OR a.expires_at > now())
	UNION
	SELECT m.dim_a_id, a.tenant_id FROM assignments a JOIN group_members m ON m.group_id = a.group_id AND m.deleted_at IS NULL
	WHERE a.group_id IS NOT NULL AND a.deleted_at IS NULL AND (a.expires_at IS NULL OR a.expires_at > now());
CREATE TABLE scope_pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (tenant_id, dim_a_id));
CREATE INDEX ON scope_pairs (dim_a_id);
-- a change of a row of assignments names its pair (id, tenant), or its tenant when it names a group; a change of a row of group_members names an id (the pairs of that id)
SELECT * FROM sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'assignments', 'pair', 'tenant_id', expires => 'expires_at');
SELECT * FROM sift.scope_follow_pairs('scope_pairs', 'visible_dim_a', 'group_members', 'member', 'dim_a_id');
SELECT sift.scope_create('facts_by_tenant', 'facts', 'dim_a_id', 'scope_pairs', 'tenant_id');   -- optional: the copy on the pairs
SELECT * FROM sift.scope_expire_pairs('scope_pairs');   -- from a scheduler, for the pairs that expired with no write
```

`sift.scope_follow_pairs` makes the pair table equal to the view (the first column of the view is the member, the second the scope key; the pair table has columns of those names) and puts statement triggers with transition tables on the source table: after an insert, update, delete or truncate of its rows, what the old and new rows name is recomputed from the view: the ids of `source_column` (`'member'`), its keys (`'key'`: every pair of the key) or, with `'pair'`, the (id, key) pairs of the rows that have both columns of the view under their names (`source_column` is the key; a row that names only its key or only its id refreshes that key or that id), in the transaction of the change, with `sift.scope_refresh_pairs`. That function reads the pairs the table has for those ids, compares them with the view's in one statement and inserts the missing pairs and deletes the extra ones: the pair table and a copy on it (its own triggers fire on the difference) are equal to the view when the statement ends. An assignment that names a group is covered because the recompute is by key: the key's pairs are read again from the whole view. A direct assignment is refreshed by its pair, which reads the view for that one id and key: a soft delete of an assignment of an id its group also gives changes nothing and costs the read of that one pair, and so does every pair given both directly and through a group (the view's `UNION` and the function's `DISTINCT` make it one pair); `'key'` recomputes all the pairs of the tenant for the same edit (the next section). A pair mode is exact where a row contributes only to the pairs of its own member and key, or to those of its key when it names no member, which is what an assignments table is. `sift.scope_refresh_pairs(pairs, view, 'key', ARRAY[3, 7])` names ids or keys by hand (`'pair'` takes the ids and, as a fifth argument, the keys, element by element) and a call without ids recomputes everything; `sift.scope_unfollow_pairs` drops the triggers (do it before dropping a table or the view).

What a recipe needs to hold:

- **Exactness.** The pairs equal the view as of the transaction of the last change of a source table, under READ COMMITTED. A refresh takes a lock of the pair table for the transaction, so a second one waits for the commit of the first and reads what it wrote: two sessions that change different source tables at once (an assignment of a group here, a new id in that group there) cannot each miss the pair that depends on both. The writers of the source tables are serialized on that lock until they commit, and the roles that write them need the privileges of the pair table (the triggers run as the writer). A table the triggers do not hear (a logical replication target, `COPY` with triggers off, `ALTER TABLE .. DISABLE TRIGGER`) leaves the pairs behind: `sift.scope_refresh_pairs` without ids puts them right, and `(SELECT * FROM visible_dim_a EXCEPT SELECT * FROM scope_pairs) UNION ALL (SELECT * FROM scope_pairs EXCEPT SELECT * FROM visible_dim_a)` lists the difference. `sift.scope_follow_pairs` refuses a logical replication target.
- **A group assignment names its whole key.** A soft delete, a delete or an update of an assignment that names a group (`group_id`, no `dim_a_id`) refreshes every pair of its tenant, because the view, not the row, says which ids a group gives: 164 M instructions and 18 to 21 ms for a tenant of 11,884 pairs (the key mode's price for any assignment edit; by pair, a direct assignment costs 6.6 M). When that matters, change the pairs by member and not by group: give the ids as direct assignments (refreshed by pair) or edit `group_members`, which is refreshed by member (5.3 M for one id, 14.7 M for 100).
- **Only columns of the followed table name ids or keys.** A table whose change reaches ids it has no column for (a `groups` table whose `deleted_at` hides all its group's ids) is not followed: express the change in the table that names them, or refresh the keys that see the group by hand.
- **Indexes.** The refresh reads the source tables by the id or key it is given: `assignments (tenant_id)`, `assignments (group_id)`, `assignments (dim_a_id)`, `group_members (dim_a_id)` and the primary key of `group_members` by group, and the pair table by key (its primary key) and by id (the index above). A scope copy on the pairs also reads the fact table by its foreign key (`facts (dim_a_id)`, as a pair that comes in already did): the pairs that go are looked up there first, one probe each, because a pair whose id has no fact has no copy row and the copy is not read for it.
- **Expiry by date with no write.** A pair that ends because `expires_at` passes does not change any row, so no trigger hears it: the pair table is the view as of the last change, and the view's `now()` is the time of the transaction that evaluated it. `sift.scope_expire_pairs(pairs, overlap)` applies the expiries: for each followed table that names an `expires` column, it refreshes the ids or keys of the rows whose date lies between the last call minus `overlap` (default one hour) and now, returns the pairs it inserted and deleted, and remembers the time. Call it from a scheduler at the granularity the application accepts a stale pair (a minute, an hour); it reads the rows that expired in that window (index the `expires` column). The overlap covers a transaction that evaluated the view before a sweep and committed after it, so it is an upper bound of the duration of a write transaction of the source; a longer one can leave a pair that expired meanwhile until the next write of its id or key. Nothing time dependent is kept in the copy: a pair that expired and is not yet swept is the only stale thing, and `sift.scope_verify` compares the copy with the pair table, not with the clock.

### What the pair table alone measures

2,000,000 facts, 20,000 ids, 60 tenants of skewed size, 18,035 memberships in 267 groups of about 68 ids, 12,840 assignments (5% soft deleted, 2% expiring), 21,214 pairs for 15,163 visible ids (scratch measurement, PostgreSQL 18.6, one backend, serial, `work_mem = 256MB`, instructions of the backend, median of 5 warm runs; cold: the cluster restarted before the statement, empty shared buffers, warm page cache; answers equal in every cell). The pair table is 0.75 MB of heap and a 0.67 MB primary key, 0.46 MB for the index by id. "Expansion" counts and sums the ids of one tenant; "sum by kind" and "latest 20 facts" read the facts of those ids by the fact index and `dim_a_id IN (SELECT dim_a_id FROM <view or pairs> WHERE tenant_id = $1)`.

| statement | tenant of | view: M instr, ms, hits (cold ms, blocks read) | pairs: M instr, ms, hits (cold ms, blocks read) |
|---|---|---|---|
| expansion | 2,032 ids | 13.29, 4.70, 1,034 (14.1, 176) | 2.11, 0.50, 8 (0.83, 8) |
| expansion | 207 ids | 2.51, 2.66, 133 (4.6, 129) | 0.41, 0.12, 4 (0.18, 4) |
| expansion | 91 ids | 1.85, 1.87, 51 (3.2, 49) | 0.31, 0.11, 3 (0.15, 3) |
| sum by kind | 2,032 ids | 85.03, 28.3, 2,373 (51.4, 1,446) | 73.41, 25.6, 1,347 (51.0, 1,277) |
| sum by kind | 207 ids | 59.82, 27.3, 1,472 (67.3, 1,394) | 58.30, 22.7, 1,343 (55.5, 1,266) |
| sum by kind | 91 ids | 58.28, 28.0, 1,390 (46.8, 1,314) | 57.36, 33.5, 1,342 (49.1, 1,267) |
| latest 20 facts | 2,032 ids | 15.63, 5.83, 1,068 (9.7, 199) | 4.03, 3.80, 42 (3.5, 24) |
| latest 20 facts | 207 ids | 3.81, 2.48, 170 (13.4, 147) | 2.30, 1.43, 41 (3.1, 27) |
| latest 20 facts | 91 ids | 3.19, 1.24, 88 (7.2, 73) | 2.29, 0.82, 40 (3.0, 26) |

The pair table cuts the expansion about 6 times in instructions and by 94 to 99% in buffer hits and in blocks read cold; a request that reads the facts of the ids gains what the expansion cost (2,032 ids: 85.0 to 73.4 M instructions, 2,373 to 1,347 hits; a latest-20 read 15.6 to 4.0 M, 1,068 to 42 hits) and nothing more, because the fact read dominates. The pairs column is the plan with `sift.agg_probe_rewrite = off`: with the setting on, the planner prices a probe of the primary key of `scope_pairs` for each id the scan meets (20,000 probes) below the one hashed set of the tenant's ids and chooses it, and the sum by kind then takes 282, 266 and 266 M instructions and 22,100 hits for the three tenants against the 1,350 of the set (the view, a `UNION`, cannot be probed and is planned as the set). That is the price of the probe plan, not of the pair table: a pair table of a small tenant in a large id range, read by a `sift_col` aggregate, is planned wrongly until the probe's price follows the tenant's size; until then `SET sift.agg_probe_rewrite = off` for the statement gives the set plan. The latest-20 reads were planned as the set in both columns.

### What an assignment edit costs by pair and by key

Edits of `assignments` and `group_members` on a 500,000-fact fixture of 20,000 ids and three tenants that see 100, 1,000 and 11,884 pairs (a group is 75 ids; direct assignments fill the rest); one statement in a transaction that is rolled back, after a checkpoint, median of 3 runs, instructions of the backend (no parallel worker takes part in a write), the statement's milliseconds inside the backend and the WAL. `'key'` is the tenant followed by key (all its pairs are recomputed), `'pair'` by pair; a copy is the scope copy of the facts on the pairs. The key mode of a group assignment and the member mode of `group_members` are the same in both columns.

| edit | tenant of | `'key'`: M instr, ms | `'pair'`: M instr, ms | `'pair'` with a copy: M instr, ms, WAL |
|---|---|---|---|---|
| insert a direct assignment (+1 pair) | 100 | 6.5, 2.1 | 6.4, 3.1 | 9.0, 4.2, 0.16 MB |
| insert a direct assignment (+1 pair) | 1,000 | 18.1, 4.7 | 6.6, 3.2 | 9.2, 9.6, 0.16 MB |
| insert a direct assignment (+1 pair) | 11,884 | 163.8, 24.8 | 6.6, 2.3 | 9.2, 6.3, 0.17 MB |
| soft delete of an assignment of an id its group gives (no pair change) | 100 | 6.6, 2.0 | 6.5, 2.8 | 8.3, 4.6, 0.00 MB |
| soft delete of an assignment of an id its group gives (no pair change) | 1,000 | 18.2, 7.5 | 6.6, 2.9 | 8.4, 2.9, 0.00 MB |
| soft delete of an assignment of an id its group gives (no pair change) | 11,884 | 163.8, 27.4 | 6.6, 2.6 | 8.4, 4.2, 0.00 MB |
| update of the key column of a direct assignment (-1 +1 pair) | 100 | 6.8, 2.0 | 6.9, 3.1 | 9.6, 4.7, 0.16 MB |
| update of the key column of a direct assignment (-1 +1 pair) | 1,000 | 18.4, 7.5 | 7.0, 3.3 | 9.7, 4.0, 0.17 MB |
| update of the key column of a direct assignment (-1 +1 pair) | 11,884 | 164.2, 20.1 | 7.0, 2.3 | 9.8, 4.6, 0.17 MB |
| soft delete of a group assignment (its 75 ids) | 11,884 | 163.8, 21.1 | 164.1, 17.9 | 630.2, 93.2, 11.03 MB |
| a group of the tenant gains 1 / 100 / 1,000 ids | 11,884 | 5.3 / 14.7 / 100.3 | 5.3 / 14.7 / 100.4 | 7.1 / 17.3 / 110.0 |
| a group of the tenant loses 1 / 100 / 1,000 ids | 11,884 | 5.8 / 9.4 / 46.3 | 5.8 / 9.4 / 46.3 | 7.8 / 432.7 / 914.7 |

A direct assignment costs the plan of one pair (about 6.5 M instructions, 2 to 3 ms, whatever the size of the tenant) instead of the read of the tenant's 11,884 pairs (164 M, 20 to 27 ms), and an edit that changes no pair writes no WAL. The soft delete of a group assignment still reads all the pairs of its key: the row names the group, and which ids it contributes is the view's to say. A copy on the pairs deletes the facts of each pair that goes at about 4 M instructions a pair for a group that loses ids, which the copy's own maintenance owns, not the refresh.

### What the pairs that go cost with a copy

A scope copy on the pairs deletes the copy's rows of each pair that goes. The statement used to join the transition table with the copy, which PostgreSQL plans for the number of pairs and not for the facts they reach: for pairs whose ids have no fact it read the whole copy from about 400 of them (1,000 pairs: 1,082 M instructions, 10,000: 1,152 M, for no row deleted). The pairs that go whose id has a fact are now looked up first (one probe of `facts (dim_a_id)` each, 8,000 instructions) and only those are deleted, by a semi join of the copy with their arrays, so that the pairs are the hashed side. 500,000 facts over 20,000 ids (25 facts for each), pair table of 30,000 pairs with the ids above 20,000 in no fact, the copy of 570,010 rows (1.14 copies per fact) or 875,106 (1.75); one statement in a transaction rolled back after a checkpoint, median of 3, instructions of the backend (no worker takes part in a write); before: M instructions, after: M instructions.

| pairs that go | ids with facts, 1.14 copies | ids with facts, 1.75 | ids without facts, 1.14 | ids without facts, 1.75 |
|---|---|---|---|---|
| 1 | 2.0, 3.3 | 2.0, 3.3 | 0.9, 1.1 | 0.9, 1.1 |
| 100 | 111.1, 113.7 | 111.5, 113.8 | 4.6, 2.3 | 5.2, 2.4 |
| 400 | 1,221.8, 384.8 | 377.1, 384.5 | 1,069.5, 5.9 | 17.0, 6.1 |
| 600 | 1,251.1, 526.3 | 1,807.1, 526.4 | 1,072.4, 8.2 | 1,628.6, 8.6 |
| 1,000 | 1,296.9, 793.8 | 1,854.3, 793.5 | 1,081.5, 12.9 | 1,638.9, 13.5 |
| 10,000 | 2,095.8, 1,634.8 | 2,659.5, 1,903.5 | 1,152.0, 118.6 | 1,714.5, 124.3 |

The WAL is the same before and after (a delete writes what it deletes); the buffers rise with the lookup and the plan's probes (1,000 pairs with facts: 32,127 to 65,766) and fall where the whole copy was read for nothing (1,000 pairs without facts: 7,171 to 5,043). A pair that moves to another key (an `UPDATE` of the pair table: the delete, then the insert of the new key) costs 2,563 M instructions (1.14 copies) for 1,000 ids with facts and 1,135 M for 1,000 without before, 2,141 M and 59 M after; for 10,000 ids with facts 11,921 M and 11,221 M, and without 2,216 M and 1,065 M, of which the insert of the new key is most. The delete of a few pairs costs the lookup in addition (1 pair: 2.0 to 3.3 M instructions, 0.3 ms). The lookup reads the fact table by its foreign key: without the index it reads the fact table for each statement.

### What one edit costs

The same fixture. Each row is one statement in a transaction that is rolled back, after a checkpoint (so the WAL includes the full-page image of the first change of each page), median of 3 or 5 runs, instructions of the backend, the milliseconds of the statement inside the backend (the host ran at 15 to 30% CPU pressure), WAL records plus full-page images. *Source alone* is the statement with no trigger; *pairs followed* adds the refresh of the pair table; *with a copy* adds the copy on the pairs (2,112,841 rows, 154 MB of heap, a 33 MB `sift_col` index, a 42 MB btree on the primary key, measured on the tip that keeps the pairs' deletes in the `sift_col` index), which holds one row of the fact table for each fact of an id and each key of it: the figures of that column follow the 100 facts an id has on average (the 1,000 ids of the group edits hold 73,664 facts, 4,637 for a group of 51).

| edit | source alone: M instr, ms, WAL | pairs followed: M instr, ms, WAL | with a copy: M instr, ms, WAL |
|---|---|---|---|
| an id moves between groups (1 row) | 0.4, 1.1, 0.02 MB | 4.6, 4.2, 0.04 MB | 43.6, 33, 6.0 MB |
| a group gains 1 id | 0.2, 0.6, 0.02 MB | 4.3, 3.8, 0.04 MB | 9.5, 8.6, 0.53 MB |
| a group gains 1,000 ids | 25.2, 10.7, 0.32 MB | 125.8, 35.9, 0.69 MB | 4,526, 1,977, 67.2 MB |
| a group loses 1,000 ids (delete) | 15.4, 3.8, 0.09 MB | 80.6, 19.0, 0.18 MB | 1,008, 131, 9.1 MB |
| 1,000 ids soft deleted | 50.9, 16.5, 0.47 MB | 122.9, 22.2, 0.56 MB | 1,042, 139, 9.4 MB |
| a tenant gains a group of 51 ids | 0.3, 2.1, 0.02 MB | 9.7, 27.1, 0.38 MB | 377, 430, 29.1 MB |
| a tenant gains a group of 1,051 ids | 0.3, 1.3, 0.02 MB | 51.9, 16.2, 0.74 MB | 5,777, 2,397, 73.4 MB |
| a tenant loses that group (delete) | 0.3, 0.6, 0.01 MB | 17.7, 7.4, 0.11 MB | 4,578, 585, 10.5 MB |
| that assignment soft deleted | 0.4, 1.0, 0.01 MB | 17.8, 10.1, 0.11 MB | 4,578, 561, 10.5 MB |
| ... and restored | 0.3, 1.6, 0.01 MB | 52.8, 18.6, 0.74 MB | 6,016, 2,704, 72.3 MB |

The refresh of the pair table costs a plan of its statement (about 4 M instructions and 4 ms for one id; the statement is planned on each call) and a read of the ids' or keys' pairs and of the view for them, so it follows the size of the edit and, for a key, the size of the key: 1,000 ids cost 80 to 125 M instructions and 0.2 to 0.7 MB of WAL. The copy is what the edit costs once the facts of the ids are rewritten: about 60,000 instructions and 240 B of WAL records for each copy row inserted (900 B with the full-page images of the first change of each page), 12,600 instructions and 50 B of records for each row deleted by the id that leaves and 62,000 instructions for each row deleted by the key that leaves (a pair that goes is found by the `sift_col` index of the copy; the full-page images of a checkpoint's first touch are 5 to 28 MB of the WAL columns of the rolled-back runs). An edit that changes the ids of a tenant in bulk is the one to batch in one statement; a copy on the pairs is worth it where the reads it serves (above) outnumber such edits.

## Totals per key and month: a cube table

When a key above the fact table (a tenant, a group) is only asked for totals (count and sum by kind, by month, in a month, filtered on a declared dimension), a *cube table* answers it without reading a fact row: one row per `(scope key, bucket, dimension)` holding the count and the sums, a `sift_col` index on it, and statement triggers that fold each change of the facts or of the pairs into it as a delta. It is a recipe, not an index kind: nothing in the engine knows about it, and it is the right form only when the reads below are all the key is asked for.

```sql
-- facts(id, dim_a_id, created_at, kind, amount, quantity); scope_pairs(dim_a_id, tenant_id) unique
CREATE TABLE facts_cube AS
SELECT p.tenant_id, date_trunc('month', f.created_at) AS bucket, f.kind,
       count(*) AS cnt, sum(f.amount) AS sum_amount, sum(f.quantity)::bigint AS sum_quantity
FROM facts f JOIN scope_pairs p USING (dim_a_id) GROUP BY 1, 2, 3;
ALTER TABLE facts_cube ADD PRIMARY KEY (tenant_id, bucket, kind);
CREATE INDEX facts_cube_col ON facts_cube USING sift_col (tenant_id, bucket, kind, cnt, sum_amount, sum_quantity);

CREATE FUNCTION facts_cube_fold() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	CREATE TEMP TABLE IF NOT EXISTS facts_cube_delta (tenant_id int, bucket timestamptz, kind smallint, c bigint, a numeric, q bigint)
		ON COMMIT DELETE ROWS;
	IF TG_OP IN ('DELETE', 'UPDATE') THEN   -- the old rows leave the cells of their tenants
		INSERT INTO facts_cube_delta SELECT p.tenant_id, date_trunc('month', o.created_at), o.kind, -count(*), -sum(o.amount), -sum(o.quantity)
		FROM oldt o JOIN scope_pairs p USING (dim_a_id) GROUP BY 1, 2, 3;
	END IF;
	IF TG_OP IN ('INSERT', 'UPDATE') THEN   -- the new rows enter them
		INSERT INTO facts_cube_delta SELECT p.tenant_id, date_trunc('month', n.created_at), n.kind, count(*), sum(n.amount), sum(n.quantity)
		FROM newt n JOIN scope_pairs p USING (dim_a_id) GROUP BY 1, 2, 3;
	END IF;
	INSERT INTO facts_cube SELECT tenant_id, bucket, kind, sum(c), sum(a), sum(q) FROM facts_cube_delta GROUP BY 1, 2, 3
	ON CONFLICT (tenant_id, bucket, kind) DO UPDATE SET cnt = facts_cube.cnt + EXCLUDED.cnt,
		sum_amount = facts_cube.sum_amount + EXCLUDED.sum_amount, sum_quantity = facts_cube.sum_quantity + EXCLUDED.sum_quantity;
	DELETE FROM facts_cube WHERE cnt = 0;
	TRUNCATE facts_cube_delta;
	RETURN NULL;
END $$;
CREATE TRIGGER facts_cube_ins AFTER INSERT ON facts REFERENCING NEW TABLE AS newt FOR EACH STATEMENT EXECUTE FUNCTION facts_cube_fold();
CREATE TRIGGER facts_cube_del AFTER DELETE ON facts REFERENCING OLD TABLE AS oldt FOR EACH STATEMENT EXECUTE FUNCTION facts_cube_fold();
CREATE TRIGGER facts_cube_upd AFTER UPDATE ON facts REFERENCING OLD TABLE AS oldt NEW TABLE AS newt FOR EACH STATEMENT EXECUTE FUNCTION facts_cube_fold();

SELECT kind, sum(cnt), sum(sum_amount) FROM facts_cube WHERE tenant_id = 38 GROUP BY kind;
```

A change of the pairs folds the same way with the signs of the pair that comes or goes, taking the facts of its member from `facts` instead of from a transition table (three more triggers on `scope_pairs`, `INSERT` with `+` and `DELETE` with `-`). The cube is exact while the triggers are on: compare it with the aggregation it replaces (`SELECT * FROM facts_cube EXCEPT SELECT .. FROM facts JOIN scope_pairs ..`, and the same the other way round) after a bulk load that disabled them.

### What it answers, and what it does not

- **Answers:** `count`, `sum` and (with the average as `sum / count`) `avg` of the declared measures, grouped by the key, by the bucket and by the declared dimension columns, on a window that is a whole number of buckets. Each added dimension multiplies the cells by its cardinality, so declare only low-cardinality ones.
- **Does not answer:** `count(DISTINCT)`, a top N or a latest N by a column that is not a dimension, a filter on a measure (`amount > 500`), a window that cuts a bucket (June 10 to 20 on a month cube; a week cube answers a week, not a month), and any statement that needs a row.
- **`min` and `max` are not invertible.** A delete or an update that removes the minimum of a cell must read the facts of the cell again, so the cube keeps a cell's `min` and `max` only with a recompute of the cells whose extreme leaves: 10,000 facts deleted or updated cost 12 to 16 G instructions (below) against 1 to 2 G for count and sum. Insert alone folds `min` and `max` for free (`least`, `greatest`). Keep them only for a table that is appended to, or leave them out.
- **A pair that comes or goes** reads the facts of its member (about 4 M instructions per pair added or removed, 100 pairs: 0.4 G).

### What it measures

Fixture: 500,000 and 2,000,000 facts of 20,000 members, 60 tenants of skewed size, pairs of 2.7 and 5.4 per member, 10 kinds and 12 months (PostgreSQL 18.6, one backend, serial; every answer equals the join's). The tenants are the ones nearest 3% and 6% of the table.

| | 500,000 facts, 2.7 | 500,000, 5.4 | 2,000,000, 2.7 | 2,000,000, 5.4 |
|---|---|---|---|---|
| table's `sift_col` index | 12.4 MB | 12.4 MB | 53 MB | 53 MB |
| scope index (replicated): growth, build | +254% (+31 MB), 8.9 s | +489% (+60 MB), 17.7 s | +255% (+136 MB), 27.7 s | +482% (+256 MB), 36.9 s |
| month cube: 7,200 cells, `sift_col` index + heap, build | 0.15 + 1.0 MB, 0.2 s | 0.15 + 1.0 MB, 0.2 s | 0.13 + 1.0 MB, 0.8 s | 0.13 + 1.0 MB, 0.8 s |
| week cube: 31,800 cells | 0.64 + 3.7 MB | 0.63 + 3.7 MB | 0.60 + 3.7 MB | 0.59 + 3.7 MB |
| day cube: about 215,000 cells | 4.9 + 21 MB | 5.2 + 22 MB | 5.2 + 22 MB | 5.1 + 22 MB |

The cube follows its cells (keys x buckets x dimensions): the same size at 2.7 and 5.4 pairs per member and at 500,000 and 2,000,000 facts. Reads of a tenant of 3%, warm M instructions / blocks read from storage by a cold first run:

| statement | join, 500,000 | scope index | month cube | join, 2,000,000 | scope index | month cube |
|---|---|---|---|---|---|---|
| `kind, count, sum` | 8.4 / 365 | 0.85 / 29 | 0.80 / 8 | 29.8 / 1,464 | 1.3 / 97 | 0.84 / 8 |
| the same in one month | 3.4 / 81 | 1.3 / 61 | 0.83 / 8 | 7.2 / 240 | 2.6 / 205 | 0.88 / 8 |
| `count, min, max, sum` | 9.1 / 378 | 0.82 / 25 | 1.1 / 15 | 32.5 / 1,545 | 1.1 / 80 | 1.2 / 14 |
| `count, sum WHERE kind = 3` | 7.8 / 366 | 0.87 / 39 | 0.66 / 8 | 27.2 / 1,452 | 1.4 / 138 | 0.70 / 8 |
| `GROUP BY` month | 13.5 / 684 | 2.7 / 39 | 1.3 / 8 | 48.1 / 2,573 | 7.2 / 129 | 1.4 / 8 |

The cube reads the same 0.7 to 1.4 M instructions and 8 to 15 blocks at both table sizes and both multiplicities. The scope index reads 1 to 7 M instructions at 2,000,000 facts, and its month-window reads grow with the pairs per member (up to 393 blocks at 5.4). A week cube answers the week-aligned reads in 1.0 to 3.3 M instructions and a day cube costs 13 to 34 M for a read over every day: the month is the granularity that pays.

What a change costs, 10,000 facts (or 100 pairs) on 500,000 facts at 2.7 pairs per member, M instructions of the backend / WAL MB:

| statement | table's index only | scope index | cube, count and sum | cube, with `min` and `max` |
|---|---|---|---|---|
| insert 10,000 facts | 538 / 2.4 | 539 / 2.4 | 1,075 / 4.7 | 1,049 / 4.8 |
| update 10,000 facts | 1,021 / 3.1 | 1,022 / 3.1 | 1,731 / 5.2 | 12,024 / 7.2 |
| delete 10,000 facts | 493 / 0.92 | 493 / 0.92 | 988 / 2.9 | 12,714 / 2.7 |
| add 100 pairs | 7 / 0.03 | 8 / 0.04 | 393 / 0.11 | 404 / 0.10 |
| remove 100 pairs | 2.5 / 0.006 | 3.1 / 0.013 | 382 / 0.04 | 3,063 / 0.04 |
| promotion of the index | 74 / 0.31 | 470 / 1.11 | | |

At 5.4 pairs per member the count and sum cube costs 1,257 / 4.9 (insert), 2,081 / 5.5 (update) and 1,191 / 3.2 (delete), and the `min` and `max` cube 16.3 G instructions for the update and the delete. After every step the cube equals a fresh aggregation in all cells. The cube roughly doubles the instructions of a fact insert or delete and adds 0.7 G to an update; the scope index costs nothing more than the index of the table, except in its promotion (6 times), and answers the statements the cube cannot.

Choose the scope index when the key is asked for rows, distinct counts, top N, latest N or filters on measures; the cube when the key is only asked for totals on whole months, the facts are inserted far more than they are updated or deleted, and the multiplicity makes the scope index large.

## What an index of this kind does not answer yet

A text group key or filter on the columns of a joined parent (`GROUP BY customers.country`) loads the parent rows one by one, so the join may be faster in the normal plan; `EXPLAIN (VERBOSE)` says why a table is read from the heap (`Lookup Heap Reason`). [AGG.md](AGG.md) lists the joins the scan answers and the ones it leaves to the normal plan.

Check any index set with `EXPLAIN (ANALYZE, BUFFERS)` of the queries that matter: `Heap Fetches: 0` and the `Custom Scan (SiftAgg)` node say the index alone answered. [TUNING.md](TUNING.md) lists the options an index takes and [SIZING.md](SIZING.md) the disk and memory a large one needs.
