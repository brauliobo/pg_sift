# Which `sift_col` indexes to create

How to choose the columns of the `sift_col` indexes of an application's tables, what a wider index buys and costs, and which btrees to keep.

Every figure on this page is measured on the store dataset of the benchmark ([bench/sql/store/gen.sql](../bench/sql/store/gen.sql): 1,000,000 orders of nine columns, a 104.2 MB heap) with its primary key and two btrees (`customer_id`, `created_at`), release 4.2.x, PostgreSQL 18.6, one host, warm cache, `EXPLAIN (ANALYZE)` minimum of five runs (writes: minimum of three, a checkpoint before each). The statements are those of [bench/sql/store/queries](../bench/sql/store/queries) plus the ones written out below; the LEFT JOIN section has a generator of its own. Absolute times depend on the host; read the ratios.

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
- **The second order is grouped by the column the table is read by.** The build groups the date, timestamp and integer arrays by an integer of 100 or more values (3 rows a value at the least) that is the first or second key column of a non-unique b-tree of the table whose other key column is a date or timestamp of the index (a partial b-tree counts; a unique one, a b-tree on the column alone and an `INCLUDE` column do not), else by the foreign key or `*_id` column of most values; the b-tree wins over the entity. The b-tree is the table's own statement of the shape `WHERE col = x ORDER BY ts LIMIT n`, which the sift index replaces, and without the second order that statement reads the units (5,000,000 rows, 100 values, index `id, g, ts, amount`, one backend, no parallel workers, cold: a b-tree on `(amount, ts)` 0.33M instructions and 24 blocks; sift without the group 4.69M and 1,491; with it 0.73M and 70; the index 37.2 to 54.5 MB (+47%), the build 24.8 to 28.7 G instructions (+16%); 1,000,000 rows 7.22M and 526 to 0.50M and 33). A b-tree on the measure alone is not a signal: the units read 4.69M instructions and 1,491 blocks against its 3.48M and 1,933, so the index stays 17 MB smaller. A measure or a code with no such b-tree is not chosen (3.5 bytes a row at 100 values): name it with `order_array_group = 'auto: col'` and decide again: `ALTER INDEX orders_col SET (order_array_group = 'auto: amount'); REINDEX INDEX orders_col;` (the weighted formula still weighs it, and refuses it at 150,000 rows, where the statement reads 1.6M instructions). The NOTICE of `CREATE INDEX` and `sift.index_choices` say which one chose the group (`second order by amount: b-tree orders_amount_ts_idx`, `entity` or `hint`). An index of an earlier release keeps its arrays until `REINDEX`, which decides again; what the build wrote is the hint of the next `REINDEX`, so the group stays after the b-tree is dropped for the sift index (`ALTER INDEX .. RESET (order_array_group)` decides from the table again). Do not write `order_array_group = 'amount'`: that groups every array by the column and keeps no plain order, so `ORDER BY ts LIMIT`, `OFFSET`, a range and `IN` of another column read the units (1,000,000 rows: 0.46M to 6.5M, 0.44M to 14.8M, 0.80M to 9.7M and 1.81M to 11.2M instructions).

## What writes cost

100,000 rows against the 1,000,000-row table with its three btrees, the first set having no `sift_col` index (time and WAL, minimum of three runs):

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

## When to add a btree

The lean index adds 1.5 times the time and 1.14 times the WAL to an insert, and 1.1 to 1.2 times the WAL to an update. The update of a column the index does not hold (`title` against the lean index) takes the time of the table alone and writes 13 MB more WAL; the one that changes an indexed column also takes 1.4 times as long. Size and WAL grow with the number of columns, so each column is worth the queries that use it.

## When to keep a btree

- **Keep** the btrees of point lookups: the primary key and the foreign keys a query looks rows up by (`orders (customer_id)`). `SELECT * FROM orders WHERE id = 424242` takes 0.044 ms through the primary key and 0.307 ms with the `sift_col` index alone; the 18 orders of one customer take 0.10 ms through the btree and 1.6 ms through the `sift_col` index. See [COL-SCAN.md](COL-SCAN.md#replacing-a-btree) for what a btree can and cannot be replaced by.
- **Do not create** a btree for a column the `sift_col` index holds to speed up a count, a group-by or a filter: the aggregate scan answers it from the index. The three btrees of the first set above left all seven queries on a sequential or bitmap scan.
- **Drop** an older `sift_col` index whose columns the new one holds, once the queries run on the new one.

## Known limit: a LEFT JOIN from a parent to its children when both tables are indexed

For `parent LEFT JOIN child ... GROUP BY` with an aggregate over the child's columns, the scan starts from the table on the left of the join. With a `sift_col` index on the child alone, the scan reads the child and takes the parent as its lookup table; with an index on **both** tables the planner does not try the child as the scan, and the statement runs as the normal plan or as a scan of the parent that loads the child as rows. Measured on 100,000 parents and 1,000,000 children, one backend without workers, minimum of five:

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
| `count(c.id)` per parent, ten largest | 1,030 ms | 75 ms, scan | 491 ms, scan of the parent |
| `count(c.id)` per `p.grp` | 648 ms | 197 ms, scan | 631 ms, normal plan |
| `sum(c.amount)` per `p.grp`, with `c.kind = 'a' AND c.at >= ..` in the `ON` | 214 ms | 30 ms, scan | 191 ms, normal plan |
| `count(DISTINCT c.kind)` per `p.grp` | 893 ms | 87 ms, scan | 591 ms, scan of the parent |
| `sum(c.amount)` per `p.state` with `WHERE p.rank < 5` | 131 ms | 159 ms, scan | 125 ms, normal plan |
| `WHERE EXISTS (SELECT 1 FROM child c WHERE c.parent_id = p.id AND c.amount > 990)` per `p.grp` | 87 ms | 92 ms | 88 ms |

- **Advice.** Index the child. If the parent's columns are only filters and group keys, they can stay out of a parent index; add one when other statements need it, and check the statements above with `EXPLAIN`.
- **A filter of the parent** (the fifth row) is the one shape where the scan of the child is slower than the normal plan (159 against 131 ms): check it with `EXPLAIN (ANALYZE)` and `SET sift.enable_agg = off`.
- **`EXISTS`, `NOT EXISTS` and `IN` from the parent to a child with conditions** keep the normal plan in every set (87 to 92 ms).

## What an index of this kind does not answer yet

A text group key or filter on the columns of a joined parent (`GROUP BY customers.country`) loads the parent rows one by one, so the join may be faster in the normal plan; `EXPLAIN (VERBOSE)` says why a table is read from the heap (`Lookup Heap Reason`). [AGG.md](AGG.md) lists the joins the scan answers and the ones it leaves to the normal plan.

Check any index set with `EXPLAIN (ANALYZE, BUFFERS)` of the queries that matter: `Heap Fetches: 0` and the `Custom Scan (SiftAgg)` node say the index alone answered. [TUNING.md](TUNING.md) lists the options an index takes and [SIZING.md](SIZING.md) the disk and memory a large one needs.
