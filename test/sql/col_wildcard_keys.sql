-- A wildcard path (m.*) keeps a dictionary of at most 65,536 distinct keys per index. The keys past it, and their values, are stored as overflow and
-- read from the heap; CREATE INDEX says how many units hold one. sift.test_limits lowers the dictionary so that a few thousand documents pass it.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE FUNCTION runq(q text, agg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN agg THEN 'on' ELSE 'off' END, true);
	EXECUTE format('SELECT coalesce(jsonb_agg(to_jsonb(t) ORDER BY to_jsonb(t)::text), ''[]'')::text FROM (%s) t', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, false); b text := runq(q, true);
BEGIN
	RETURN CASE WHEN a = b THEN 'same' ELSE format('DIFF off=%s on=%s', left(a, 200), left(b, 200)) END;
END $$;

CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- 3 units of about 1,000 documents (a document is about 1.6 kB): the first 16 keys fill the dictionary of 16, the second unit brings keys it has not
-- seen, the third only those it has
CREATE TABLE jw (id int, pad text, m jsonb) WITH (autovacuum_enabled = false);
INSERT INTO jw
SELECT i, repeat(md5(i::text), 50),
       CASE WHEN i % 100 = 0 AND i BETWEEN 1200 AND 1800 THEN jsonb_build_object('k' || (i % 16), i, 'z' || i, 1)
            ELSE jsonb_build_object('k' || (i % 16), i, 'k' || ((i + 5) % 16), 2) END
FROM generate_series(1, 3000) i;

-- the default dictionary holds them all: no NOTICE
CREATE INDEX jw_all ON jw USING sift_col (id, m) WITH (paths = 'm.* int4');
DROP INDEX jw_all;

SET sift.test_limits = 'shred_keys=16';
CREATE INDEX jw_col ON jw USING sift_col (id, m) WITH (paths = 'm.* int4');
VACUUM (FREEZE, ANALYZE) jw;
SELECT sift.col_verify('jw_col', true);

-- the query that reads the keys keeps the normal plan, with the same answer; a query that does not read them is scanned
SELECT pushed('SELECT e.key, sum(e.value::int) FROM jw, jsonb_each_text(m) e GROUP BY e.key') AS keys_scanned,
	same('SELECT e.key, sum(e.value::int) FROM jw, jsonb_each_text(m) e GROUP BY e.key');
SELECT pushed('SELECT id % 7, count(*), sum(id) FROM jw GROUP BY 1') AS ids_scanned, same('SELECT id % 7, count(*), sum(id) FROM jw GROUP BY 1');

RESET sift.test_limits;
DROP TABLE jw;
DROP FUNCTION runq(text, bool), same(text), pushed(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
