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

## What writes cost

100,000 rows against the 1,000,000-row table with its three btrees, the first set having no `sift_col` index (time and WAL, minimum of three runs):

| statement | btrees only | lean | wide |
|---|---|---|---|
| insert 100,000 rows | 614 ms, 67.4 MB | 943 ms, 77.0 MB | 982 ms, 79.1 MB |
| update `amount` of 100,000 rows | 2,822 ms, 97.8 MB | 3,886 ms, 110.9 MB | 3,745 ms, 117.2 MB |
| update `title` of 100,000 rows | 2,438 ms, 105.1 MB | 2,466 ms, 117.9 MB | 3,166 ms, 122.7 MB |
| delete 100,000 rows | 61 ms, 19.1 MB | 171 ms, 22.5 MB | 136 ms, 22.5 MB |

The lean index adds 1.5 times the time and 1.14 times the WAL to an insert, and 1.1 to 1.2 times the WAL to an update. The update of a column the index does not hold (`title` against the lean index) takes the time of the table alone and writes 13 MB more WAL; the one that changes an indexed column also takes 1.4 times as long. Size and WAL grow with the number of columns, so each column is worth the queries that use it.

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
