-- A sift_table answers aggregates through SiftAggV6 under a Gather (workers steal the granules of its parts with the table's own versions), a partitioned table of sift_tables, or of sift_tables and heaps
-- with a sift_col index, is one SiftAggV6 over its partitions, and so is the parallel one that steals granules across all of them. Every answer is the heap twin's, also with rows, deletes and updates
-- of the open transaction (and of a savepoint it rolled back) that the workers see through the transaction's own versions.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
SET max_parallel_workers = 8;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 4;
SET seq_page_cost = 10000;
SET cpu_tuple_cost = 100;
SET random_page_cost = 10000;

CREATE TABLE ts (id int, a int, b int, v int, n numeric(12,2), t text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE th (LIKE ts) WITH (autovacuum_enabled = false);
INSERT INTO ts SELECT g, g % 7, g % 101, g % 1000, (g % 997) / 8.0, CASE WHEN g % 13 = 0 THEN NULL ELSE 'k' || g % 11 END FROM generate_series(1, 150000) g;
INSERT INTO th SELECT * FROM ts;
ANALYZE ts;
ANALYZE th;

CREATE FUNCTION same_rows_serial(a text, b text) RETURNS boolean LANGUAGE sql SET max_parallel_workers_per_gather = 0 AS $$ SELECT sift_helpers.same_rows(a, b) $$;

-- the query over the sift relation @T has the twin's rows, as it runs with workers and as it runs with none
CREATE FUNCTION parity(q text, sift_rel text, twin_rel text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE par boolean; ser boolean;
BEGIN
	par := sift_helpers.same_rows(replace(q, '@T', sift_rel), replace(q, '@T', twin_rel));
	ser := same_rows_serial(replace(q, '@T', sift_rel), replace(q, '@T', twin_rel));
	RETURN par AND ser;
END $$;

CREATE FUNCTION plan_has(q text, node text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP
		IF line LIKE '%' || node || '%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- the most workers a run of the query launched
CREATE FUNCTION launched(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE line text; n int := 0;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		IF line LIKE '%Workers Launched: %' THEN n := greatest(n, substring(line from 'Launched: (\d+)')::int); END IF;
	END LOOP;
	RETURN n;
END $$;

-- one table
SELECT plan_has('SELECT a, sum(v) FROM ts GROUP BY a', 'Custom Scan (SiftAggV6)') AS table_served;
SELECT plan_has('SELECT a, sum(v) FROM ts GROUP BY a', 'Workers Planned') AS table_parallel;
SELECT launched('SELECT a, sum(v) FROM ts GROUP BY a') > 0 AS table_workers_launched;
SELECT parity('SELECT a, b, count(*), sum(v), min(n), max(n), avg(v) FROM @T GROUP BY a, b', 'ts', 'th') AS table_groups;
SELECT parity('SELECT t, count(*), sum(n) FROM @T WHERE v > 100 GROUP BY t', 'ts', 'th') AS table_text_keys_and_quals;
SELECT parity('SELECT count(*), sum(v), count(DISTINCT b) FROM @T', 'ts', 'th') AS table_no_keys;
SELECT parity('SELECT a, count(DISTINCT b) FROM @T GROUP BY a', 'ts', 'th') AS table_distinct;

-- deletes and updates (column deltas) before the query
DELETE FROM ts WHERE id % 10 = 0;
DELETE FROM th WHERE id % 10 = 0;
UPDATE ts SET v = v + 5, n = n + 0.5 WHERE id % 3 = 0;
UPDATE th SET v = v + 5, n = n + 0.5 WHERE id % 3 = 0;
SELECT launched('SELECT a, sum(v) FROM ts GROUP BY a') > 0 AS deltas_workers_launched;
SELECT parity('SELECT a, b, count(*), sum(v), sum(n) FROM @T GROUP BY a, b', 'ts', 'th') AS after_deletes_and_updates;

-- the open transaction's own rows, deletes and updates, and a savepoint it rolls back
BEGIN;
INSERT INTO ts SELECT g, g % 7, g % 101, 5, 1.25, 'new' FROM generate_series(150001, 190000) g;
INSERT INTO th SELECT g, g % 7, g % 101, 5, 1.25, 'new' FROM generate_series(150001, 190000) g;
SELECT parity('SELECT a, count(*), sum(v), sum(n) FROM @T GROUP BY a', 'ts', 'th') AS own_inserts;
DELETE FROM ts WHERE id <= 7;
DELETE FROM th WHERE id <= 7;
UPDATE ts SET v = 100000 WHERE id BETWEEN 100 AND 103;
UPDATE th SET v = 100000 WHERE id BETWEEN 100 AND 103;
SELECT launched('SELECT a, sum(v) FROM ts GROUP BY a') > 0 AS own_writes_workers_launched;
SELECT parity('SELECT a, count(*), sum(v), sum(n) FROM @T GROUP BY a', 'ts', 'th') AS own_deletes_and_updates;
SAVEPOINT s1;
INSERT INTO ts SELECT g, g % 7, g % 101, 7, 2.5, 'sub' FROM generate_series(190001, 200000) g;
INSERT INTO th SELECT g, g % 7, g % 101, 7, 2.5, 'sub' FROM generate_series(190001, 200000) g;
DELETE FROM ts WHERE id % 7 = 0;
DELETE FROM th WHERE id % 7 = 0;
UPDATE ts SET v = v + 2 WHERE id % 5 = 0;
UPDATE th SET v = v + 2 WHERE id % 5 = 0;
SELECT parity('SELECT a, count(*), sum(v), sum(n) FROM @T GROUP BY a', 'ts', 'th') AS inside_savepoint;
ROLLBACK TO s1;
DELETE FROM th WHERE id > 190000;
SELECT parity('SELECT a, count(*), sum(v), sum(n) FROM @T GROUP BY a', 'ts', 'th') AS after_rollback_to;
ROLLBACK;
DELETE FROM th WHERE id > 150000;
SELECT parity('SELECT a, count(*), sum(v), sum(n) FROM @T GROUP BY a', 'ts', 'th') AS after_rollback;

-- partitions that are all sift_tables, one ten times the size of the others
CREATE TABLE pp (id int, a int, b int, v int, n numeric(12,2)) PARTITION BY RANGE (id);
CREATE TABLE pp1 PARTITION OF pp FOR VALUES FROM (0) TO (10000) USING sift_table;
CREATE TABLE pp2 PARTITION OF pp FOR VALUES FROM (10000) TO (20000) USING sift_table;
CREATE TABLE pp3 PARTITION OF pp FOR VALUES FROM (20000) TO (30000) USING sift_table;
CREATE TABLE pp4 PARTITION OF pp FOR VALUES FROM (30000) TO (150000) USING sift_table;
CREATE TABLE ph (LIKE pp);
INSERT INTO pp SELECT g, g % 7, g % 101, g % 1000, (g % 997) / 8.0 FROM generate_series(1, 139999) g;
INSERT INTO ph SELECT * FROM pp;
ANALYZE pp;
ANALYZE ph;
SELECT plan_has('SELECT a, sum(v) FROM pp GROUP BY a', 'Custom Scan (SiftAggV6)') AS partitioned_served;
SELECT plan_has('SELECT a, sum(v) FROM pp GROUP BY a', 'Partitions: 4') AS partitioned_reads_all_partitions;
SELECT plan_has('SELECT a, sum(v) FROM pp GROUP BY a', 'Workers Planned') AS partitioned_parallel;
SELECT launched('SELECT a, sum(v) FROM pp GROUP BY a') > 0 AS partitioned_workers_launched;
SELECT parity('SELECT a, b, count(*), sum(v), sum(n) FROM @T GROUP BY a, b', 'pp', 'ph') AS partitioned_groups;
SELECT parity('SELECT a, count(*), sum(v) FROM @T WHERE id >= 25000 AND id < 35000 GROUP BY a', 'pp', 'ph') AS partitioned_pruned;
SELECT plan_has('SELECT a, sum(v) FROM pp WHERE id >= 25000 AND id < 35000 GROUP BY a', 'Partitions: 2') AS pruned_partitions;
SELECT parity('SELECT count(*), sum(v), count(DISTINCT b) FROM @T', 'pp', 'ph') AS partitioned_no_keys;
DELETE FROM pp WHERE id % 9 = 0;
DELETE FROM ph WHERE id % 9 = 0;
UPDATE pp SET v = v + 3 WHERE id % 4 = 0;
UPDATE ph SET v = v + 3 WHERE id % 4 = 0;
SELECT parity('SELECT a, b, count(*), sum(v) FROM @T GROUP BY a, b', 'pp', 'ph') AS partitioned_after_deletes_and_updates;
BEGIN;
INSERT INTO pp SELECT g, g % 7, g % 101, 9, 1.5 FROM generate_series(140000, 149999) g;
INSERT INTO ph SELECT g, g % 7, g % 101, 9, 1.5 FROM generate_series(140000, 149999) g;
DELETE FROM pp WHERE id BETWEEN 5 AND 15;
DELETE FROM ph WHERE id BETWEEN 5 AND 15;
SELECT parity('SELECT a, count(*), sum(v) FROM @T GROUP BY a', 'pp', 'ph') AS partitioned_own_writes;
ROLLBACK;
DELETE FROM ph WHERE id >= 140000;
SELECT parity('SELECT a, count(*), sum(v) FROM @T GROUP BY a', 'pp', 'ph') AS partitioned_after_rollback;

-- sift_table partitions beside heap partitions with a sift_col index
CREATE TABLE pm (id int, a int, b int, v int, n numeric(12,2)) PARTITION BY RANGE (id);
CREATE TABLE pm1 PARTITION OF pm FOR VALUES FROM (0) TO (40000) USING sift_table;
CREATE TABLE pm2 PARTITION OF pm FOR VALUES FROM (40000) TO (80000);
CREATE TABLE pm3 PARTITION OF pm FOR VALUES FROM (80000) TO (120000) USING sift_table;
CREATE TABLE pm4 PARTITION OF pm FOR VALUES FROM (120000) TO (140000);
CREATE INDEX pm2_sift ON pm2 USING sift_col (id, a, b, v, n);
CREATE INDEX pm4_sift ON pm4 USING sift_col (id, a, b, v, n);
INSERT INTO pm SELECT * FROM ph WHERE id < 140000;
ANALYZE pm;
CREATE TABLE pmh (LIKE pm);
INSERT INTO pmh SELECT * FROM pm;
SELECT plan_has('SELECT a, sum(v) FROM pm GROUP BY a', 'Custom Scan (SiftAggV6)') AS mixed_served;
SELECT plan_has('SELECT a, sum(v) FROM pm GROUP BY a', 'Partitions: 4') AS mixed_reads_all_partitions;
SELECT plan_has('SELECT a, sum(v) FROM pm GROUP BY a', 'Workers Planned') AS mixed_parallel;
SELECT launched('SELECT a, sum(v) FROM pm GROUP BY a') > 0 AS mixed_workers_launched;
SELECT parity('SELECT a, b, count(*), sum(v), sum(n) FROM @T GROUP BY a, b', 'pm', 'pmh') AS mixed_groups;
SELECT parity('SELECT a, count(*), sum(v) FROM @T WHERE v > 500 AND b < 50 GROUP BY a', 'pm', 'pmh') AS mixed_quals;
UPDATE pm SET v = v + 1 WHERE id % 5 = 0;
UPDATE pmh SET v = v + 1 WHERE id % 5 = 0;
DELETE FROM pm WHERE id % 11 = 0;
DELETE FROM pmh WHERE id % 11 = 0;
SELECT parity('SELECT a, count(*), sum(v) FROM @T GROUP BY a', 'pm', 'pmh') AS mixed_after_deletes_and_updates;
BEGIN;
INSERT INTO pm SELECT g, g % 7, g % 101, 4, 1.5 FROM generate_series(30000, 139999, 7) g;
INSERT INTO pmh SELECT g, g % 7, g % 101, 4, 1.5 FROM generate_series(30000, 139999, 7) g;
DELETE FROM pm WHERE id BETWEEN 50000 AND 50100 OR id BETWEEN 90000 AND 90100;
DELETE FROM pmh WHERE id BETWEEN 50000 AND 50100 OR id BETWEEN 90000 AND 90100;
SELECT parity('SELECT a, count(*), sum(v) FROM @T GROUP BY a', 'pm', 'pmh') AS mixed_own_writes;
ROLLBACK;

DROP TABLE ts, th, pp, ph, pm, pmh;
DROP FUNCTION parity(text, text, text), same_rows_serial(text, text), plan_has(text, text), launched(text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
