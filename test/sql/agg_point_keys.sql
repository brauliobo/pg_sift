-- Custom Scan (SiftAgg) asks the point directory (point_columns: the rows of the segment; point_tid_columns: heap addresses) for the rows of a short IN list, an
-- equality, a short range and the keys of an inner or semi lookup join: a unit that holds none is not read, and one that does goes on as those rows alone
-- ("Units From Point Directory" of EXPLAIN ANALYZE). The directory lists the rows whose fingerprint matches, so every conjunct still runs on them: the rows are
-- those of the normal plan over NULLs, duplicates, text, DML in and out of the transaction, VACUUM, the write buffer, a merge and parallel workers.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE src (id int, k int, s text, d date, v numeric(10, 2), pad text DEFAULT repeat('x', 700));
ALTER TABLE src ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO src (id, k, s, d, v)
SELECT i, CASE WHEN i % 53 = 0 THEN NULL ELSE (i * 7919) % 9000 END, CASE WHEN i % 47 = 0 THEN NULL ELSE 's' || (i * 31) % 6000 END,
	   date '2024-01-01' + i % 500, (i % 1000) / 10.0
FROM generate_series(1, 30000) i;
CREATE TABLE dim (k int, grp int);
INSERT INTO dim SELECT g * 37 % 9000, g % 4 FROM generate_series(1, 400) g;
INSERT INTO dim VALUES (NULL, 1), (7919, 2), (7919, 3), (123456, 1);

-- the same rows with a directory of heap addresses, one of rows, and none
CREATE TABLE pk_t AS TABLE src;
CREATE TABLE pk_r AS TABLE src;
CREATE TABLE pk_n AS TABLE src;
CREATE INDEX pk_t_col ON pk_t USING sift_col (k, s, d, v) WITH (point_tid_columns = 'k, s');
CREATE INDEX pk_r_col ON pk_r USING sift_col (k, s, d, v) WITH (point_columns = 'k, s');
CREATE INDEX pk_n_col ON pk_n USING sift_col (k, s, d, v);
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) pk_t, pk_r, pk_n, dim;

-- the plan: whether the pushed aggregate read the directory, and how many rows it left
CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN r := 'SiftAgg'; END IF;
		IF l LIKE '%from point directory%' THEN r := r || ' directory'; END IF;
	END LOOP;
	RETURN r;
END $$;

-- the rows of the statement as pushed, and of the normal plan
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

-- every statement on each table: an equality, IN lists (NULL among them, a key the table lacks), a short range, a text key, a key and another conjunct
CREATE FUNCTION all_same(t text) RETURNS TABLE (n int, plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE q text[] := ARRAY[
	'SELECT count(*), sum(v) FROM %1$s WHERE k = 7919',
	'SELECT count(*), sum(v), min(d) FROM %1$s WHERE k IN (1, 2, 3, 4000, 8999, 123456, 7919)',
	'SELECT count(*) FROM %1$s WHERE k IN (5, NULL, 6)',
	'SELECT count(*), sum(v) FROM %1$s WHERE k BETWEEN 100 AND 130',
	'SELECT count(*), sum(v) FROM %1$s WHERE s IN (''s1'', ''s2'', ''s300'', ''nosuch'')',
	'SELECT d, count(*), sum(v) FROM %1$s WHERE k IN (10, 20, 30, 40, 50, 60, 70, 80) AND d >= date ''2024-06-01'' GROUP BY d ORDER BY d',
	'SELECT count(*), sum(v) FROM %1$s WHERE k IN (SELECT k FROM dim WHERE grp = 1)',
	'SELECT dim.grp, count(*), sum(v) FROM %1$s JOIN dim ON dim.k = %1$s.k WHERE dim.k < 3000 GROUP BY 1 ORDER BY 1',
	'SELECT count(*) FROM %1$s WHERE EXISTS (SELECT 1 FROM dim WHERE dim.k = %1$s.k AND dim.grp = 2)',
	'SELECT count(*) FROM %1$s WHERE k NOT IN (1, 2, 3)'];
BEGIN
	FOR n IN 1..array_length(q, 1) LOOP
		RETURN QUERY SELECT n, s.plan, s.result_rows, s.vs_normal_plan FROM same(format(q[n], t)) s;
	END LOOP;
END $$;

SET sift.agg_force = on;
SELECT 'tids' AS dir, * FROM all_same('pk_t');
SELECT 'rows' AS dir, * FROM all_same('pk_r');
SELECT 'none' AS dir, * FROM all_same('pk_n');

-- the directory cuts the rows a unit decodes: the counters name the units and the rows
SELECT plan_of('SELECT count(*) FROM pk_t WHERE k IN (1, 2, 3, 7919)') AS tids, plan_of('SELECT count(*) FROM pk_r WHERE k IN (1, 2, 3, 7919)') AS rows,
	   plan_of('SELECT count(*) FROM pk_n WHERE k IN (1, 2, 3, 7919)') AS none;

-- a list of more keys than the units are many (and than the set a zone test carries) asks the directory as well
CREATE TABLE pk_l AS SELECT i AS id, (i * 7919) % 100000 AS k, i % 97 AS g FROM generate_series(1, 100000) i;
CREATE INDEX pk_l_col ON pk_l USING sift_col (k, g) WITH (point_tid_columns = 'k');
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, ANALYZE) pk_l;
SELECT * FROM same('SELECT count(*), sum(g) FROM pk_l WHERE k IN (' || (SELECT string_agg((i * 331)::text, ',') FROM generate_series(1, 300) i) || ')');
DROP TABLE pk_l;

-- DML: the unit a changed row lives in has a dead row, the new versions are in the write buffer; inside the transaction and after it
BEGIN;
UPDATE pk_t SET k = k + 1 WHERE id % 97 = 0;
DELETE FROM pk_t WHERE id % 89 = 0;
INSERT INTO pk_t SELECT id + 100000, k, s, d, v, pad FROM pk_t WHERE id % 211 = 0;
SELECT 'tids, in transaction' AS dir, * FROM all_same('pk_t');
COMMIT;
UPDATE pk_r SET k = k + 1 WHERE id % 97 = 0;
DELETE FROM pk_r WHERE id % 89 = 0;
INSERT INTO pk_r SELECT id + 100000, k, s, d, v, pad FROM pk_r WHERE id % 211 = 0;
SELECT 'tids, committed' AS dir, * FROM all_same('pk_t');
SELECT 'rows, committed' AS dir, * FROM all_same('pk_r');

-- a rolled back change leaves dead versions on pages that are not all-visible
BEGIN;
UPDATE pk_t SET k = k + 2 WHERE id % 31 = 0;
ROLLBACK;
SELECT 'tids, rolled back' AS dir, * FROM all_same('pk_t');

VACUUM pk_t, pk_r;
SELECT 'tids, vacuumed' AS dir, * FROM all_same('pk_t');
SELECT 'rows, vacuumed' AS dir, * FROM all_same('pk_r');

SELECT sift.col_promote('pk_t_col'::regclass) >= 0 AS promoted, sift.col_promote('pk_r_col'::regclass) >= 0 AS promoted_rows;
SELECT 'tids, promoted' AS dir, * FROM all_same('pk_t');
SELECT sift.col_merge('pk_t_col'::regclass, true) >= 0 AS merged, sift.col_merge('pk_r_col'::regclass, true) >= 0 AS merged_rows;
SELECT sift.col_verify('pk_t_col', true), sift.col_verify('pk_r_col', true);
SELECT 'tids, merged' AS dir, * FROM all_same('pk_t');
SELECT 'rows, merged' AS dir, * FROM all_same('pk_r');

-- parallel workers each begin their own range of units
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET max_parallel_workers_per_gather = 2;
SELECT 'tids, parallel' AS dir, * FROM all_same('pk_t');

RESET ALL;
DROP TABLE src, dim, pk_t, pk_r, pk_n;
DROP FUNCTION all_same(text), same(text), plan_of(text);
DROP EXTENSION pg_sift;
