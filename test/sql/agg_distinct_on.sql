-- DISTINCT ON in the aggregate scan (docs/AGG.md, "DISTINCT ON"): a group by the ON columns that keeps, for each group, the columns of the first row by the ORDER BY.
-- The ORDER BY must be a total order of the rows (it holds the columns of a unique index of the table), else the row PostgreSQL's sort picks is its choice and
-- the plan stays. Every statement is run with the pushdown and with sift.enable_agg = off: the rows must be the same, after every kind of write.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
-- the table is small: page costs that keep the sort and the btree plans from looking cheap as well
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE ev (id int PRIMARY KEY, store_id int, kind text, day date, ts timestamp, amount numeric(10, 2), note varchar(5)) WITH (autovacuum_enabled = false);
INSERT INTO ev
SELECT i, 1 + (i * 7919) % 60, (ARRAY['a', 'b', 'c', NULL])[1 + i % 4], date '2024-01-01' + i % 90, timestamp '2024-01-01' + ((i * 37) % 100000) * interval '1 second',
       CASE WHEN i % 11 = 0 THEN NULL ELSE (i % 1000) / 10.0 END, left(md5(i::text), 5)
FROM generate_series(1, 6000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, store_id, kind, day, ts, amount, note);
VACUUM (FREEZE, ANALYZE) ev;
CREATE TABLE stores (id int PRIMARY KEY, kind text);
INSERT INTO stores SELECT i, (ARRAY['outlet', 'popup', 'flagship'])[1 + i % 3] FROM generate_series(1, 60) i;
ANALYZE stores;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l LIKE '%Workers Planned: _%' AND l NOT LIKE '%Workers Planned: 0%' THEN r := r || ' with workers'; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := plan_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

-- the plan: the aggregates are the first row's columns by the ORDER BY
EXPLAIN (COSTS OFF) SELECT DISTINCT ON (store_id) store_id, id, ts, amount FROM ev ORDER BY store_id, ts DESC, id;

-- the latest and the earliest row of each store, mixed directions and NULLS placement, a text sort key, an expression of the columns, a month as the ON expression
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, id, ts, amount, kind, note FROM ev ORDER BY store_id, ts DESC, id$$);
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, id, ts, amount FROM ev ORDER BY store_id, ts, id$$);
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, id, amount, kind FROM ev ORDER BY store_id, amount DESC NULLS LAST, kind NULLS FIRST, id DESC$$);
SELECT * FROM same($$SELECT DISTINCT ON (kind) kind, id, note, amount FROM ev ORDER BY kind NULLS LAST, note COLLATE "C" DESC, id$$);
SELECT * FROM same($$SELECT DISTINCT ON (kind) kind, id, upper(note) AS n, amount * 2 AS twice FROM ev ORDER BY kind NULLS LAST, note COLLATE "C" DESC, id$$);
SELECT * FROM same($$SELECT DISTINCT ON (date_trunc('month', day)) date_trunc('month', day) AS m, id, amount FROM ev ORDER BY date_trunc('month', day), amount DESC NULLS LAST, id$$);
-- the ON columns in another order than the ORDER BY has them, one row a group, a filter that leaves nothing, a LIMIT and an OFFSET
SELECT * FROM same($$SELECT DISTINCT ON (kind, store_id) kind, store_id, id FROM ev WHERE store_id < 10 ORDER BY store_id, kind, ts, id$$);
SELECT * FROM same($$SELECT DISTINCT ON (id) id, store_id, amount FROM ev WHERE store_id < 10 ORDER BY id$$);
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, id FROM ev WHERE store_id < 0 ORDER BY store_id, id$$);
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, id, ts FROM ev ORDER BY store_id, ts DESC, id LIMIT 7 OFFSET 2$$);
-- a join that only filters, in a sub-select, in a CTE
SELECT * FROM same($$SELECT DISTINCT ON (o.store_id) o.store_id, o.id, o.ts FROM ev o JOIN stores s ON s.id = o.store_id AND s.kind = 'popup' ORDER BY o.store_id, o.ts DESC, o.id$$);
SELECT * FROM same($$SELECT kind, count(*) AS n, max(ts) AS last FROM (SELECT DISTINCT ON (store_id) store_id, id, ts, kind FROM ev ORDER BY store_id, ts DESC, id) t GROUP BY kind$$);
SELECT * FROM same($$WITH last AS (SELECT DISTINCT ON (store_id) store_id, id, ts FROM ev ORDER BY store_id, ts DESC, id) SELECT s.kind, count(*) AS n, max(l.ts) AS last FROM last l JOIN stores s ON s.id = l.store_id GROUP BY s.kind$$);

-- with workers: the best rows of the workers are compared with the leader's
RESET seq_page_cost; RESET random_page_cost;
SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET min_parallel_index_scan_size = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;
SELECT plan_of($$SELECT DISTINCT ON (store_id) store_id, id, ts, amount, kind FROM ev ORDER BY store_id, ts DESC, id$$);
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, id, ts, amount, kind FROM ev ORDER BY store_id, ts DESC, id$$);
SELECT * FROM same($$SELECT DISTINCT ON (kind) kind, id, amount, note FROM ev ORDER BY kind NULLS FIRST, amount DESC NULLS LAST, note, id$$);
RESET parallel_setup_cost; RESET parallel_tuple_cost; RESET min_parallel_table_scan_size; RESET min_parallel_index_scan_size;
SET max_parallel_workers_per_gather = 0;

-- the normal plan stays: no unique column in the ORDER BY (a tie would be PostgreSQL's sort's choice; a select list of the sort columns only gives the same rows),
-- a volatile function in the ORDER BY, more sort columns than the scan keeps. A volatile function of the select list is evaluated by PostgreSQL after the rows are picked.
SELECT plan_of($$SELECT DISTINCT ON (store_id) store_id, id, ts FROM ev ORDER BY store_id, ts DESC$$) AS no_tie_break;
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, ts FROM ev ORDER BY store_id, ts DESC$$);
SELECT plan_of($$SELECT DISTINCT ON (store_id) store_id, id FROM ev WHERE store_id < 30 ORDER BY store_id, 0 * random(), id$$) AS volatile_sort;
SELECT plan_of($$SELECT DISTINCT ON (store_id) store_id, id, ts FROM ev ORDER BY store_id, ts DESC, kind, day, note, amount, id$$) AS six_sort_columns;
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, id, 0 * random() AS z FROM ev ORDER BY store_id, ts DESC, id$$);

CREATE TABLE tn (id int, g int, v int, w int, u int) WITH (autovacuum_enabled = false);
INSERT INTO tn SELECT i, i % 20, (i * 31) % 97, i % 13, CASE WHEN i = 1 THEN NULL ELSE i END FROM generate_series(1, 4000) i;
CREATE UNIQUE INDEX tn_id ON tn (id);
CREATE INDEX tn_col ON tn USING sift_col (id, g, v, w, u);
VACUUM (FREEZE, ANALYZE) tn;
SELECT plan_of($$SELECT DISTINCT ON (g) g, id, v FROM tn ORDER BY g, v DESC, id$$) AS nullable_unique;
SELECT * FROM same($$SELECT DISTINCT ON (g) g, id, v FROM tn ORDER BY g, v DESC, id$$);
ALTER TABLE tn ALTER COLUMN id SET NOT NULL;
SELECT plan_of($$SELECT DISTINCT ON (g) g, id, v FROM tn ORDER BY g, v DESC, id$$) AS not_null_unique;
SELECT * FROM same($$SELECT DISTINCT ON (g) g, id, v FROM tn ORDER BY g, v DESC, id$$);
-- a plan prepared with the unique index is replanned when the index is dropped
PREPARE p AS SELECT DISTINCT ON (g) g, id, v, w FROM tn ORDER BY g, v DESC, id;
EXECUTE p;
DROP INDEX tn_id;
EXPLAIN (COSTS OFF) EXECUTE p;
CREATE UNIQUE INDEX tn_id_part ON tn (id) WHERE v >= 0;
SELECT plan_of($$SELECT DISTINCT ON (g) g, id, v FROM tn ORDER BY g, v DESC, id$$) AS partial_unique;
DROP INDEX tn_id_part;
-- a unique index over several columns, all of which the sort must hold (and which are NOT NULL), and one that takes NULLs as equal
CREATE UNIQUE INDEX tn_gwi ON tn (g, w, id);
SELECT plan_of($$SELECT DISTINCT ON (g) g, id, v FROM tn ORDER BY g, w, id$$) AS composite_nullable;
ALTER TABLE tn ALTER COLUMN g SET NOT NULL, ALTER COLUMN w SET NOT NULL, ALTER COLUMN id SET NOT NULL;
SELECT * FROM same($$SELECT DISTINCT ON (g) g, id, v FROM tn ORDER BY g, w, id$$);
SELECT plan_of($$SELECT DISTINCT ON (g) g, id, v FROM tn ORDER BY g, w DESC$$) AS composite_without_id;
DROP INDEX tn_gwi;
SELECT plan_of($$SELECT DISTINCT ON (g) g, u, v FROM tn ORDER BY g, u$$) AS nullable_u;
CREATE UNIQUE INDEX tn_u ON tn (u) NULLS NOT DISTINCT;
SELECT * FROM same($$SELECT DISTINCT ON (g) g, u, v FROM tn ORDER BY g, u NULLS FIRST$$);
DROP INDEX tn_u;
DEALLOCATE p;

-- groups beyond the memory a hash aggregate gets (the best row of each is kept): the normal plan stays
SET work_mem = '64kB'; SET hash_mem_multiplier = 1;
SELECT plan_of($$SELECT DISTINCT ON (id) id, store_id, ts, amount, kind, note, day FROM ev ORDER BY id$$) AS group_a_row;
SELECT plan_of($$SELECT DISTINCT ON (store_id, kind) store_id, kind, id, ts, amount FROM ev ORDER BY store_id, kind, ts DESC, id$$) AS few_hundred_groups;
SELECT plan_of($$SELECT DISTINCT ON (kind) kind, id, ts, amount FROM ev ORDER BY kind, ts DESC, id$$) AS four_groups;
RESET work_mem; RESET hash_mem_multiplier;

-- a partitioned table: the unique index holds the partition key, so it is unique across the partitions
CREATE TABLE tp (id int NOT NULL, d date NOT NULL, g int, v int, PRIMARY KEY (id, d)) PARTITION BY RANGE (d);
CREATE TABLE tp1 PARTITION OF tp FOR VALUES FROM ('2024-01-01') TO ('2024-02-01');
CREATE TABLE tp2 PARTITION OF tp FOR VALUES FROM ('2024-02-01') TO ('2024-03-01');
INSERT INTO tp SELECT i, date '2024-01-01' + i % 58, i % 9, (i * 7) % 100 FROM generate_series(1, 5000) i;
CREATE INDEX tp_col ON tp USING sift_col (id, d, g, v);
VACUUM (FREEZE, ANALYZE) tp;
SELECT * FROM same($$SELECT DISTINCT ON (g) g, id, d, v FROM tp ORDER BY g, v DESC, d, id$$);
SELECT * FROM same($$SELECT DISTINCT ON (g) g, id, d, v FROM tp WHERE d >= '2024-02-01' ORDER BY g, v DESC, d, id$$);
SELECT plan_of($$SELECT DISTINCT ON (g) g, id, d, v FROM tp ORDER BY g, v DESC, id$$) AS partial_key;

-- a sift_table
CREATE TABLE tt (id int PRIMARY KEY, g int, v int, s text) USING sift_table;
INSERT INTO tt SELECT i, i % 11, (i * 13) % 101, 'k' || i % 17 FROM generate_series(1, 3000) i;
SELECT * FROM same($$SELECT DISTINCT ON (g) g, id, v, s FROM tt ORDER BY g, v DESC NULLS LAST, s, id$$);

-- after writes: inserts, updates, deletes, a rolled-back write, and a vacuum
INSERT INTO ev SELECT 6000 + i, 1 + i % 60, 'z', date '2024-06-01', timestamp '2030-01-01' + i * interval '1 second', i, 'new' FROM generate_series(1, 300) i;
UPDATE ev SET ts = ts + interval '5 years' WHERE id % 97 = 0;
DELETE FROM ev WHERE id % 53 = 0;
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, id, ts, amount, kind, note FROM ev ORDER BY store_id, ts DESC, id$$);
BEGIN; DELETE FROM ev WHERE store_id < 20; ROLLBACK;
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, id, ts, amount, kind, note FROM ev ORDER BY store_id, ts DESC, id$$);
VACUUM ev;
SELECT * FROM same($$SELECT DISTINCT ON (store_id) store_id, id, ts, amount, kind, note FROM ev ORDER BY store_id, ts DESC, id$$);
-- the rows of the transaction itself
BEGIN;
UPDATE ev SET ts = timestamp '2040-01-01' WHERE id = 7;
SELECT store_id, id, ts FROM (SELECT DISTINCT ON (store_id) store_id, id, ts FROM ev ORDER BY store_id, ts DESC, id) t WHERE id = 7;
ROLLBACK;

DROP FUNCTION same(text);
DROP FUNCTION plan_of(text);
DROP TABLE tt, tp, tn, stores, ev;
DROP EXTENSION pg_sift;
