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

A request often carries a key that lives above the fact table: a tenant, a group, a plan. The facts reach it through a *scope relation*, a table of unique `(member key, scope key)` pairs (the ids a tenant sees, the ids of a group). An index keyed by the member answers such a request by expanding the pairs and testing every fact row against the set of ids, so its cost follows the table, not the tenant. The answer is the *scope index*, a `sift_col` index that keeps the rows replicated by scope key inside the table's own index, so the cost follows the rows of the key (EXPERIMENTAL in 5.0, no second table and no triggers on the facts; [COL-SCOPE](COL-SCOPE.md) has the recipe, the shapes the planner answers, what it costs and its edges).

A copy of the fact table for each scope key of its member, kept by triggers and read by a plain query on a table (`CREATE TABLE AS` of the join), is not shipped: the benchmark of [COL-SCOPE](COL-SCOPE.md#against-tuned-stock-postgresql) builds one by its own SQL (`denorm_flat`, `denorm_part`) and finds it 13 to 28 times slower than the scope index to read for the tenants of 6% or more, 3 to 4 times dearer to write, and faster only for the latest 20 rows of a key and a selective filter. A table of that shape, with the key as the first column of its `sift_col` index, takes `key_order` and `key_cut` by itself.

## Keeping the pairs from a view

The scope relation is often not a table anyone writes but the answer of a view: the ids a tenant sees are the ids it is assigned directly, plus the ids of the groups it is assigned (`assignments` and `group_members`), minus the rows soft deleted (`deleted_at`) and the assignments that have expired (`expires_at`). Reading it through the view expands the assignments and the group membership for every request. A table of the pairs, kept equal to the view, makes that expansion one index range, and a scope index on it ([COL-SCOPE](COL-SCOPE.md)) is kept equal to the join by the same transaction.

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
SELECT * FROM sift.scope_expire_pairs('scope_pairs');   -- from a scheduler, for the pairs that expired with no write
```

`sift.scope_follow_pairs` makes the pair table equal to the view (the first column of the view is the member, the second the scope key; the pair table has columns of those names) and puts statement triggers with transition tables on the source table: after an insert, update, delete or truncate of its rows, what the old and new rows name is recomputed from the view: the ids of `source_column` (`'member'`), its keys (`'key'`: every pair of the key) or, with `'pair'`, the (id, key) pairs of the rows that have both columns of the view under their names (`source_column` is the key; a row that names only its key or only its id refreshes that key or that id), in the transaction of the change, with `sift.scope_refresh_pairs`. That function reads the pairs the table has for those ids, compares them with the view's in one statement and inserts the missing pairs and deletes the extra ones: the pair table is equal to the view when the statement ends. An assignment that names a group is covered because the recompute is by key: the key's pairs are read again from the whole view. A direct assignment is refreshed by its pair, which reads the view for that one id and key: a soft delete of an assignment of an id its group also gives changes nothing and costs the read of that one pair, and so does every pair given both directly and through a group (the view's `UNION` and the function's `DISTINCT` make it one pair); `'key'` recomputes all the pairs of the tenant for the same edit (the next section). A pair mode is exact where a row contributes only to the pairs of its own member and key, or to those of its key when it names no member, which is what an assignments table is. `sift.scope_refresh_pairs(pairs, view, 'key', ARRAY[3, 7])` names ids or keys by hand (`'pair'` takes the ids and, as a fifth argument, the keys, element by element) and a call without ids recomputes everything; `sift.scope_unfollow_pairs` drops the triggers (do it before dropping a table or the view).

What a recipe needs to hold:

- **Exactness.** The pairs equal the view as of the transaction of the last change of a source table, under READ COMMITTED. A refresh takes a lock of the pair table for the transaction, so a second one waits for the commit of the first and reads what it wrote: two sessions that change different source tables at once (an assignment of a group here, a new id in that group there) cannot each miss the pair that depends on both. The writers of the source tables are serialized on that lock until they commit, and the roles that write them need the privileges of the pair table (the triggers run as the writer). A table the triggers do not hear (a logical replication target, `COPY` with triggers off, `ALTER TABLE .. DISABLE TRIGGER`) leaves the pairs behind: `sift.scope_refresh_pairs` without ids puts them right, and `(SELECT * FROM visible_dim_a EXCEPT SELECT * FROM scope_pairs) UNION ALL (SELECT * FROM scope_pairs EXCEPT SELECT * FROM visible_dim_a)` lists the difference. `sift.scope_follow_pairs` refuses a logical replication target.
- **A group assignment names its whole key.** A soft delete, a delete or an update of an assignment that names a group (`group_id`, no `dim_a_id`) refreshes every pair of its tenant, because the view, not the row, says which ids a group gives: 164 M instructions and 18 to 21 ms for a tenant of 11,884 pairs (the key mode's price for any assignment edit; by pair, a direct assignment costs 6.6 M). When that matters, change the pairs by member and not by group: give the ids as direct assignments (refreshed by pair) or edit `group_members`, which is refreshed by member (5.3 M for one id, 14.7 M for 100).
- **Only columns of the followed table name ids or keys.** A table whose change reaches ids it has no column for (a `groups` table whose `deleted_at` hides all its group's ids) is not followed: express the change in the table that names them, or refresh the keys that see the group by hand.
- **Indexes.** The refresh reads the source tables by the id or key it is given: `assignments (tenant_id)`, `assignments (group_id)`, `assignments (dim_a_id)`, `group_members (dim_a_id)` and the primary key of `group_members` by group, and the pair table by key (its primary key) and by id (the index above).
- **Expiry by date with no write.** A pair that ends because `expires_at` passes does not change any row, so no trigger hears it: the pair table is the view as of the last change, and the view's `now()` is the time of the transaction that evaluated it. `sift.scope_expire_pairs(pairs, overlap)` applies the expiries: for each followed table that names an `expires` column, it refreshes the ids or keys of the rows whose date lies between the last call minus `overlap` (default one hour) and now, returns the pairs it inserted and deleted, and remembers the time. Call it from a scheduler at the granularity the application accepts a stale pair (a minute, an hour); it reads the rows that expired in that window (index the `expires` column). The overlap covers a transaction that evaluated the view before a sweep and committed after it, so it is an upper bound of the duration of a write transaction of the source; a longer one can leave a pair that expired meanwhile until the next write of its id or key. Nothing time dependent is kept in a scope index on the pairs: a pair that expired and is not yet swept is the only stale thing, and `sift.col_scope_verify` compares the index with the pair table, not with the clock.

### Soft deleted memberships and grants

A bridge table that soft deletes (`memberships(group_id, dim_a_id, deleted_at)`, unique over all rows) and grants that reach an entity directly or through a group (`grants(id, account_id, dim_a_id, group_id,
deleted_at, expired_at)`, a `CHECK` that one of the two is NULL) are kept as two views of their active rows and two pair tables that follow them. A group is a scope of the memberships alone; an account is the
union of its direct grants and of the memberships of the groups it holds, active rows only (`deleted_at IS NULL`, `expired_at IS NULL OR expired_at > now()`):

```sql
CREATE VIEW active_memberships AS SELECT dim_a_id, group_id FROM memberships WHERE deleted_at IS NULL;
CREATE VIEW visible_dim_a AS
	SELECT g.dim_a_id, g.account_id FROM grants g WHERE g.dim_a_id IS NOT NULL AND g.deleted_at IS NULL AND (g.expired_at IS NULL OR g.expired_at > now())
	UNION
	SELECT m.dim_a_id, g.account_id FROM grants g JOIN memberships m ON m.group_id = g.group_id AND m.deleted_at IS NULL
	WHERE g.group_id IS NOT NULL AND g.deleted_at IS NULL AND (g.expired_at IS NULL OR g.expired_at > now());
CREATE TABLE group_pairs   (dim_a_id int NOT NULL, group_id   int NOT NULL, PRIMARY KEY (group_id, dim_a_id));   CREATE INDEX ON group_pairs (dim_a_id);
CREATE TABLE account_pairs (dim_a_id int NOT NULL, account_id int NOT NULL, PRIMARY KEY (account_id, dim_a_id)); CREATE INDEX ON account_pairs (dim_a_id);
SELECT * FROM sift.scope_follow_pairs('group_pairs',   'active_memberships', 'memberships', 'pair',   'group_id');
SELECT * FROM sift.scope_follow_pairs('account_pairs', 'visible_dim_a',      'grants',      'pair',   'account_id', expires => 'expired_at');
SELECT * FROM sift.scope_follow_pairs('account_pairs', 'visible_dim_a',      'memberships', 'member', 'dim_a_id');
```

The indexes the refresh reads are `memberships (dim_a_id) WHERE deleted_at IS NULL`, `grants (account_id)`, `grants (group_id) WHERE group_id IS NOT NULL`, `grants (dim_a_id) WHERE dim_a_id IS NOT NULL` and,
for the sweep, `grants (expired_at) WHERE expired_at IS NOT NULL`, and a btree on `facts (dim_a_id)`. Run `sift.scope_expire_pairs('account_pairs')` from a scheduler. The group alone needs no
table when the scope index is enough ([COL-SCOPE](COL-SCOPE.md#pairs-that-are-soft-deleted-a-partial-unique-btree): the partial unique btree of the memberships is the pairs; the account, the union of its direct grants and of the grants of its groups, is [a scope of two paths](#grants-to-a-member-or-to-a-group-a-scope-index-of-two-paths)).

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

Edits of `assignments` and `group_members` on a 500,000-fact fixture of 20,000 ids and three tenants that see 100, 1,000 and 11,884 pairs (a group is 75 ids; direct assignments fill the rest); one statement in a transaction that is rolled back, after a checkpoint, median of 3 runs, instructions of the backend (no parallel worker takes part in a write), the statement's milliseconds inside the backend and the WAL. `'key'` is the tenant followed by key (all its pairs are recomputed), `'pair'` by pair. The key mode of a group assignment and the member mode of `group_members` are the same in both columns.

| edit | tenant of | `'key'`: M instr, ms | `'pair'`: M instr, ms |
|---|---|---|---|
| insert a direct assignment (+1 pair) | 100 | 6.5, 2.1 | 6.4, 3.1 |
| insert a direct assignment (+1 pair) | 1,000 | 18.1, 4.7 | 6.6, 3.2 |
| insert a direct assignment (+1 pair) | 11,884 | 163.8, 24.8 | 6.6, 2.3 |
| soft delete of an assignment of an id its group gives (no pair change) | 100 | 6.6, 2.0 | 6.5, 2.8 |
| soft delete of an assignment of an id its group gives (no pair change) | 1,000 | 18.2, 7.5 | 6.6, 2.9 |
| soft delete of an assignment of an id its group gives (no pair change) | 11,884 | 163.8, 27.4 | 6.6, 2.6 |
| update of the key column of a direct assignment (-1 +1 pair) | 100 | 6.8, 2.0 | 6.9, 3.1 |
| update of the key column of a direct assignment (-1 +1 pair) | 1,000 | 18.4, 7.5 | 7.0, 3.3 |
| update of the key column of a direct assignment (-1 +1 pair) | 11,884 | 164.2, 20.1 | 7.0, 2.3 |
| soft delete of a group assignment (its 75 ids) | 11,884 | 163.8, 21.1 | 164.1, 17.9 |
| a group of the tenant gains 1 / 100 / 1,000 ids | 11,884 | 5.3 / 14.7 / 100.3 | 5.3 / 14.7 / 100.4 |
| a group of the tenant loses 1 / 100 / 1,000 ids | 11,884 | 5.8 / 9.4 / 46.3 | 5.8 / 9.4 / 46.3 |

A direct assignment costs the plan of one pair (about 6.5 M instructions, 2 to 3 ms, whatever the size of the tenant) instead of the read of the tenant's 11,884 pairs (164 M, 20 to 27 ms), and an edit that changes no pair writes no WAL. The soft delete of a group assignment still reads all the pairs of its key: the row names the group, and which ids it contributes is the view's to say.

### What one edit costs

The same fixture. Each row is one statement in a transaction that is rolled back, after a checkpoint (so the WAL includes the full-page image of the first change of each page), median of 3 or 5 runs, instructions of the backend, the milliseconds of the statement inside the backend (the host ran at 15 to 30% CPU pressure), WAL records plus full-page images. *Source alone* is the statement with no trigger; *pairs followed* adds the refresh of the pair table.

| edit | source alone: M instr, ms, WAL | pairs followed: M instr, ms, WAL |
|---|---|---|
| an id moves between groups (1 row) | 0.4, 1.1, 0.02 MB | 4.6, 4.2, 0.04 MB |
| a group gains 1 id | 0.2, 0.6, 0.02 MB | 4.3, 3.8, 0.04 MB |
| a group gains 1,000 ids | 25.2, 10.7, 0.32 MB | 125.8, 35.9, 0.69 MB |
| a group loses 1,000 ids (delete) | 15.4, 3.8, 0.09 MB | 80.6, 19.0, 0.18 MB |
| 1,000 ids soft deleted | 50.9, 16.5, 0.47 MB | 122.9, 22.2, 0.56 MB |
| a tenant gains a group of 51 ids | 0.3, 2.1, 0.02 MB | 9.7, 27.1, 0.38 MB |
| a tenant gains a group of 1,051 ids | 0.3, 1.3, 0.02 MB | 51.9, 16.2, 0.74 MB |
| a tenant loses that group (delete) | 0.3, 0.6, 0.01 MB | 17.7, 7.4, 0.11 MB |
| that assignment soft deleted | 0.4, 1.0, 0.01 MB | 17.8, 10.1, 0.11 MB |
| ... and restored | 0.3, 1.6, 0.01 MB | 52.8, 18.6, 0.74 MB |

The refresh of the pair table costs a plan of its statement (about 4 M instructions and 4 ms for one id; the statement is planned on each call) and a read of the ids' or keys' pairs and of the view for them, so it follows the size of the edit and, for a key, the size of the key: 1,000 ids cost 80 to 125 M instructions and 0.2 to 0.7 MB of WAL. An edit that changes the ids of a tenant in bulk is the one to batch in one statement.

## Grants to a member or to a group: a scope index of two paths

The shape of [the soft deleted memberships and grants](#soft-deleted-memberships-and-grants), read through the scope index and without a pair table: an account sees an entity when it has a grant on the
entity or on a group the entity belongs to. The three tables are the application's, with the soft delete it already has, and the scope index declares the union of the two paths
([COL-SCOPE](COL-SCOPE.md#pairs-reached-by-two-paths-a-direct-grant-and-a-group-grant) has the grammar and the rules):

```sql
CREATE TABLE grants (id bigint PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz);   -- one of dim_a_id and group_id is set
CREATE TABLE memberships (id bigint PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz);
-- the three btrees that define the rows of each path: unique over the active rows
CREATE UNIQUE INDEX grants_direct      ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX grants_group       ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;
CREATE UNIQUE INDEX memberships_active ON memberships (group_id, dim_a_id) WHERE deleted_at IS NULL;
-- the three journals, each keyed by what its path depends on: the member, the group, the member again
CREATE INDEX grants_by_member      ON grants      USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group       ON grants      USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_member ON memberships USING sift_col (dim_a_id, group_id)  WITH (track_changes = 'ctid');
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, kind, value_num)
  WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
```

`CREATE INDEX` says which btree of each path it recorded (one `NOTICE` each) and how many entries the scope holds for the rows. A member that an account reaches directly and through its
groups is one pair, so a fact is in the units of the account once; the entries are the distinct pairs, within `sift.scope_max_multiplicity`.

**Verify.**

```sql
-- the distinct pairs the scope stands for, as the join says them (a member reached twice is one pair)
SELECT count(*) FROM (
  SELECT dim_a_id, tenant_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL
  UNION
  SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id
   WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL) p;
SELECT sift.col_verify('facts_col', true);                                 -- empty
SELECT format, count(*) FROM sift.col_segments('facts_col') WHERE kind = 'immutable' GROUP BY format;   -- 49
```

and `EXPLAIN` of a read of the scope has `Scope Units` and no `Scope Fallback` once the journals vouch. A scope whose reads say `Scope Fallback: no journal` is missing one of the three journal indexes
(or has one that a row can be inserted outside of: a partial journal); `unfilled` is the `REINDEX` of the scope index after the journal that was made later (or the rewrite the maintenance makes); `other lineage` is a journal that was rebuilt (`REINDEX` of the scope index).

**Monitor.** `sift.col_maintenance_due('facts_col', false)` says `stale connection` when so many members of the pairs or of the groups changed that a rewrite is cheaper than the patches
(`sift.col_connect_refresh_pct`, the groups counted by their members) or when a thousand reads of the patches cost more than the rewrite; `Scope Changed Keys` and `Scope Patched Rows` in `EXPLAIN (ANALYZE)` are the work of one read and `too many changed keys` is a read that
fell to the entity rows (the reader's cap on the changed members). The journals pay a 32-byte record for each row of the three tables an `UPDATE` or `DELETE` ends or an `INSERT` adds; a statement that changes a column no
index holds (a HOT update) writes none. A time dependent grant (`expires_at`) is a column the application or a scheduled `UPDATE` sets into the predicate (`WHERE deleted_at IS NULL AND NOT expired`): `now()` is refused in an index predicate.

**What it measures.** 100,000 facts, 2,000 entities, 200 groups, 20 accounts, 3,321 pairs, a read of one account (instructions:u of the backend, millions): 6.02 cold and 1.34 warm with nothing changed since the
build, 12.5 and 7.8 with one group (10 members) changed, 26.5 and 21.8 with 100 groups (1,090 members, 135 patched), where the join on the `UNION` takes 68.9 and 63.6.

**Dump and restore.** The text of the scope is the record: `pg_dump` writes the declaration and the restore chooses the three btrees again by the rule (the only partial unique btree on the two
columns, else the lowest oid of the plain ones). A rule that is settled by the oids of the btrees (two partial ones on a relation, or a partial and a plain one) can choose another one after a restore than
before it: name the btree with `using` where the choice must not depend on them (`grants(group_id, tenant_id) using grants_group`).

**Two paths or a pair table.** The pair table of the sections above is the right choice for arbitrary pair queries (a table anyone can join or filter), a condition on a third table (a group
that is soft deleted itself, a tenant that is suspended: the view says it, the scope index cannot), a pair that is a view, or a time dependent pair swept by `sift.scope_expire_pairs`. The two paths are the right choice for the grants and memberships shape: the incremental maintenance is exact for every edit of the three tables (a grant, a membership, a
member moved, a group emptied), there is no pair table to size, to vacuum and to keep by triggers, and an edit of one group costs the members of that group at the next read (6.5 M
instructions for 10 members, above) and not a recompute of the account's pairs (164 M for a tenant of 11,884 pairs by key, a different fixture, in [the edit table](#what-an-assignment-edit-costs-by-pair-and-by-key)). The planner reads the join forms of the scope index of one path and of two from the units ([COL-SCOPE](COL-SCOPE.md#what-the-planner-recognises)).

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
