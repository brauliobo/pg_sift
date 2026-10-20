-- SiftAgg after the rows that hold a value the index cannot store are repaired, when the VACUUM that follows leaves the index alone: with dead
-- rows on under 2% of the heap pages PostgreSQL skips the vacuuming of the indexes (ambulkdelete), so the index never learns that the rows are
-- dead. The VACUUM of the index (amvacuumcleanup) finds the dead rows that hold the values and the pushdown is back; a unit that still has a live
-- row with such a value keeps its flag. Plain VACUUM throughout (no INDEX_CLEANUP on). Every query runs with and without the pushdown.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE v_t (id int PRIMARY KEY, doc jsonb, touched int DEFAULT 0) WITH (autovacuum_enabled = false, fillfactor = 70);
-- about 800 heap pages; a JSON string in the float8 path of the rows 100 to 103 (one page) and of the row 29000 (another superblock)
INSERT INTO v_t (id, doc)
SELECT i, jsonb_build_object('lines', (SELECT jsonb_agg(jsonb_build_object('k', (array['a', 'b', 'c'])[1 + (i + j) % 3],
                                                                          'n', CASE WHEN i IN (100, 101, 102, 103, 29000) THEN to_jsonb((j % 10)::text) ELSE to_jsonb(j % 10) END))
                                       FROM generate_series(1, 1 + i % 4) j))
FROM generate_series(1, 30000) i;
CREATE INDEX v_t_col ON v_t USING sift_col (id, doc) WITH (paths = 'doc.lines[]{k text, n float8}');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) v_t;

CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = on;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_on;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0;
END $$;

-- the strings read as numbers, as the normal plan reads them
CREATE FUNCTION unstring(doc jsonb) RETURNS jsonb LANGUAGE sql AS $$
	SELECT jsonb_set(doc, '{lines}', (SELECT jsonb_agg(CASE WHEN jsonb_typeof(l -> 'n') = 'string' THEN jsonb_set(l, '{n}', to_jsonb((l ->> 'n')::numeric)) ELSE l END ORDER BY o)
	                                  FROM jsonb_array_elements(doc -> 'lines') WITH ORDINALITY x(l, o)))
$$;

CREATE FUNCTION agg_q(sel text) RETURNS text LANGUAGE sql AS $$
	SELECT 'SELECT ' || sel || ' FROM v_t t, jsonb_to_recordset(t.doc -> ''lines'') AS l(k text, n float8) GROUP BY l.k'
$$;

SELECT count(*) AS string_values, count(DISTINCT t.id) AS string_rows
FROM v_t t, jsonb_array_elements(t.doc -> 'lines') l WHERE jsonb_typeof(l -> 'n') = 'string';
SELECT pushed(agg_q('l.k, avg(l.n)')) AS float_pushed, same(agg_q('l.k, avg(l.n)')) AS float_same;

-- a live row that holds a string, updated without touching an indexed column (a HOT update: the TID of the index is now a redirect to the new
-- version): the VACUUM does not take it for a dead row
BEGIN;
UPDATE v_t SET touched = 1 WHERE id = 29000;
SELECT pg_stat_get_xact_tuples_hot_updated('v_t'::regclass) AS hot_updates;
COMMIT;
VACUUM (DISABLE_PAGE_SKIPPING) v_t;
SELECT pushed(agg_q('l.k, avg(l.n)')) AS hot_row_pushed, same(agg_q('l.k, avg(l.n)')) AS hot_row_same;
-- and the row is still in the index
SELECT pushed('SELECT count(*), max(id) FROM v_t') AS count_pushed, same('SELECT count(*), max(id) FROM v_t') AS count_same;

-- repair the rows of the first superblock: the one of the second still holds a string in a live row
UPDATE v_t SET doc = unstring(doc) WHERE id BETWEEN 100 AND 103;
VACUUM (DISABLE_PAGE_SKIPPING) v_t;
SELECT pushed(agg_q('l.k, avg(l.n)')) AS live_row_pushed, same(agg_q('l.k, avg(l.n)')) AS live_row_same;

-- repair the last one: only dead rows hold a string, and the VACUUM of the index lets the scan read the leaf again
UPDATE v_t SET doc = unstring(doc) WHERE id = 29000;
VACUUM (DISABLE_PAGE_SKIPPING) v_t;
SELECT count(*) AS string_values FROM v_t t, jsonb_array_elements(t.doc -> 'lines') l WHERE jsonb_typeof(l -> 'n') = 'string';
SELECT pushed(agg_q('l.k, avg(l.n)')) AS repaired_pushed, same(agg_q('l.k, avg(l.n)')) AS repaired_same;
SELECT pushed(agg_q('l.k, count(*)')) AS key_pushed, same(agg_q('l.k, count(*)')) AS key_same;

-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift;
