-- The same cache for a dimension that is the parent of a connection of Format 1 (the fact table's index is Format 2.0, the parent's index and its child are Format 1): the journal is asked through the
-- same route, and every write or DDL of the parent makes the next statement read it again. Testing build only: only it can make a Format 1 index.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_hashagg = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET client_min_messages = warning;
SET sift.test_unified_agg = on;

SET sift.test_create_format1 = on;
CREATE TABLE dim (k int PRIMARY KEY, a int, b int, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 7, i % 3, i % 5 = 0 FROM generate_series(0, 699) i;
CREATE TABLE kid (id int PRIMARY KEY, k int) WITH (autovacuum_enabled = false);
INSERT INTO kid SELECT i, i % 700 FROM generate_series(1, 500) i;
CREATE INDEX dim_col ON dim USING sift_col (k, a, b, flag);
CREATE INDEX kid_col ON kid USING sift_col (id, k) WITH (connect = 'k -> dim(k)');
SET sift.test_create_format1 = off;
CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 211 = 0 THEN NULL ELSE i % 750 END, i % 11 FROM generate_series(1, 3000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, v);
VACUUM (FREEZE, ANALYZE) ev, dim, kid;
SELECT relation::text, format_major FROM sift.status() WHERE relation::text IN ('dim_col', 'kid_col', 'ev_col') ORDER BY 1;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT dim.a, count(*), sum(ev.v) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT dim.a, dim.b, count(*) FROM ev LEFT JOIN dim ON dim.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2$q$);
CREATE FUNCTION cache(sql text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || sql LOOP
		IF l LIKE '%Dimension Cache:%' THEN RETURN trim(split_part(l, ':', 2)); END IF;
	END LOOP;
	RETURN 'none';
END $$;
CREATE FUNCTION engaged(sql text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l LIKE '%SiftAggV6%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
-- the first statement after a change says what it did with the dimensions, the second what the next one does
CREATE FUNCTION report(label text, upto int DEFAULT 1) RETURNS TABLE (stage text, shape int, first_statement text, same_answers bool, next_statement text) LANGUAGE plpgsql AS $$
DECLARE s record; f text; ok bool; BEGIN
	FOR s IN SELECT * FROM shapes WHERE n <= upto ORDER BY n LOOP
		f := cache(s.sql);
		ok := sift_helpers.answer(s.sql, true) IS NOT DISTINCT FROM sift_helpers.answer(s.sql, false);
		RETURN QUERY SELECT label, s.n, f, ok, cache(s.sql);
	END LOOP;
END $$;
SELECT n, engaged(sql) AS engine FROM shapes ORDER BY n;
SELECT * FROM report('built', 2);
UPDATE dim SET a = 6, flag = NULL WHERE k % 31 = 0;
SELECT * FROM report('payload update');
UPDATE dim SET k = k + 100000 WHERE k % 97 = 0;
SELECT * FROM report('key update');
DELETE FROM dim WHERE k % 53 = 0;
SELECT * FROM report('delete');
INSERT INTO dim SELECT i, 1, 1, true FROM generate_series(700, 749) i;
SELECT * FROM report('insert');
BEGIN;
UPDATE dim SET b = 2 WHERE k % 17 = 0;
SELECT * FROM report('in the transaction', 2);
ROLLBACK;
SELECT * FROM report('after the abort');
UPDATE kid SET k = k + 1 WHERE id % 7 = 0;
SELECT * FROM report('child write');
ALTER TABLE dim ADD COLUMN extra int DEFAULT 5;
SELECT * FROM report('add column');
REINDEX INDEX dim_col;
SELECT * FROM report('reindex');
TRUNCATE dim;
SELECT * FROM report('truncate', 2);
INSERT INTO dim SELECT i, i % 7, i % 3, i % 5 = 0 FROM generate_series(0, 299) i;
SELECT * FROM report('refilled');

DROP FUNCTION report(text, int), engaged(text), cache(text);
DROP TABLE shapes, ev, kid, dim;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
