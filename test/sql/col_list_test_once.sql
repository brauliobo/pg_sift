-- A list of numbers is one test for every segment of an index and every begin of a scan (sift_col scan prune.c, num_test): an index of several segments that overlap
-- in the key (runs of a build, each holding rows of every key) answers a long IN list, a NOT IN list and a list with a range as the heap does.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.test_col_segment_kb = 3000;

CREATE TABLE lt (id int, tenant_id int, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO lt SELECT i, (i::bigint * 7919 % 400)::int, i % 100, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 120000) i;
VACUUM (FREEZE, ANALYZE) lt;
CREATE INDEX lt_idx ON lt USING sift_col (id, tenant_id, amount, pad) WITH (key_cut = 'tenant_id');
RESET sift.test_col_segment_kb;
SELECT count(*) > 3 AS several_segments FROM sift.col_segments('lt_idx') WHERE kind = 'immutable';

CREATE TABLE lt_keys AS SELECT g * 3 AS k FROM generate_series(1, 120) g;
CREATE FUNCTION lt_same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; SET LOCAL sift.enable_agg = on;
	EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO a;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on;
	EXECUTE format('SELECT string_agg(x::text, E''\n'') FROM (%s) x', q) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;

CREATE FUNCTION lt_pushed(q text) RETURNS bool LANGUAGE plpgsql SET sift.enable_agg = on SET enable_seqscan = off AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'SiftAgg' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SELECT lt_pushed('SELECT count(*), sum(amount) FROM lt WHERE tenant_id IN (SELECT k FROM lt_keys)') AS answered_by_the_index;

SELECT q, lt_same(q) AS same_rows FROM (VALUES
	('SELECT count(*), sum(amount) FROM lt WHERE tenant_id IN (SELECT k FROM lt_keys)'),
	('SELECT count(*), sum(amount) FROM lt WHERE tenant_id NOT IN (SELECT k FROM lt_keys)'),
	('SELECT count(*), sum(id) FROM lt WHERE tenant_id IN (SELECT k FROM lt_keys) AND amount BETWEEN 10 AND 40'),
	('SELECT tenant_id % 7, count(*) FROM lt WHERE tenant_id = ANY (ARRAY(SELECT k FROM lt_keys)) GROUP BY 1 ORDER BY 1')
) t(q);

DROP FUNCTION lt_same(text), lt_pushed(text);
DROP TABLE lt, lt_keys;
DROP EXTENSION pg_sift;
