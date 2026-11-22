-- The 6.0 Vulkan path under churn: a table that has had deletes, updates and inserts since its index was built must give, with sift.gpu = on, the rows the CPU gives and the rows
-- a heap without the index gives. The parts the deletes touch are read by the device with their dead rows masked, the parts the writes left unsettled by the CPU, and the groups of both
-- are one answer. The test prints only whether the three agree, so the output is the same on a build without the backend, on one without a device, and on one with a device.
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
SET client_min_messages = error;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

-- a table with the index and a twin heap that gets every statement the table gets; the columns hold 2^n values drawn evenly, which no codec shrinks, so every chunk is one the device decodes
CREATE TABLE ch (id int, g int, h int, w int, q int, k int) WITH (autovacuum_enabled = false);
CREATE TABLE chh (LIKE ch) WITH (autovacuum_enabled = false);
INSERT INTO ch
SELECT i, hashint4(i) & 31, hashint4(i + 3) & 1023, ((i::bigint * 7919) % 100003 - 50000)::int, hashint4(i + 7) & 1023, hashint4(i + 9) & 7
FROM generate_series(1, 300000) i;
INSERT INTO chh SELECT * FROM ch;
CREATE INDEX ch_col ON ch USING sift_col (id, g, h, w, q, k);
VACUUM (FREEZE, ANALYZE) ch;
VACUUM (FREEZE, ANALYZE) chh;

CREATE FUNCTION dml(stmt text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE replace(stmt, '@', 'ch');
	EXECUTE replace(stmt, '@', 'chh');
END $$;

CREATE FUNCTION rows_of(q text, rel text, gpu text) RETURNS SETOF text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('SET sift.gpu = %L', gpu);
	EXECUTE 'SET sift.gpu_min_rows = 1';
	RETURN QUERY EXECUTE format('SELECT x::text FROM (%s) x', format(q, rel));
	RESET sift.gpu;
	RESET sift.gpu_min_rows;
END $$;

CREATE TEMP TABLE r (side text, r text);

CREATE FUNCTION agree(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	TRUNCATE r;
	INSERT INTO r SELECT 'gpu', x FROM rows_of(q, 'ch', 'on') x;
	INSERT INTO r SELECT 'cpu', x FROM rows_of(q, 'ch', 'off') x;
	INSERT INTO r SELECT 'heap', x FROM rows_of(q, 'chh', 'off') x;
	SELECT count(*) INTO d FROM (
		(SELECT r FROM r WHERE side = 'gpu' EXCEPT ALL SELECT r FROM r WHERE side = 'heap') UNION ALL
		(SELECT r FROM r WHERE side = 'heap' EXCEPT ALL SELECT r FROM r WHERE side = 'gpu') UNION ALL
		(SELECT r FROM r WHERE side = 'cpu' EXCEPT ALL SELECT r FROM r WHERE side = 'heap') UNION ALL
		(SELECT r FROM r WHERE side = 'heap' EXCEPT ALL SELECT r FROM r WHERE side = 'cpu')) x;
	RETURN d = 0 AND (SELECT count(*) FROM r WHERE side = 'heap') > 0;
END $$;

CREATE FUNCTION agree_all() RETURNS TABLE (ungrouped bool, filtered bool, dense bool, dense2 bool, hashed bool, high_card bool, avg_and_extremes bool) LANGUAGE sql AS $$
	SELECT agree($q$SELECT count(*), sum(w), min(w), max(w), min(q), max(q) FROM %s$q$),
	       agree($q$SELECT count(*), sum(w), avg(w) FROM %s WHERE q < 500 AND k >= 2$q$),
	       agree($q$SELECT g, count(*), sum(w), min(q), max(q) FROM %s GROUP BY g$q$),
	       agree($q$SELECT g, k, count(*), sum(w) FROM %s WHERE w > 0 GROUP BY g, k$q$),
	       agree($q$SELECT w, count(*), sum(q), min(q), max(h) FROM %s GROUP BY w$q$),
	       agree($q$SELECT id, count(*), sum(w) FROM %s WHERE id BETWEEN 140000 AND 160000 AND q < 600 GROUP BY id$q$),
	       agree($q$SELECT k, count(q), sum(q), avg(q), min(w), max(w) FROM %s WHERE id > 100000 GROUP BY k$q$)
$$;

-- psql -v proof=1: after each state, what the device read of three of the queries (EXPLAIN ANALYZE); it prints nothing otherwise
CREATE FUNCTION gpu_lines() RETURNS TABLE (shape text, gpu text) LANGUAGE plpgsql AS $$
DECLARE
	q text;
	l text;
	line text;
BEGIN
	SET sift.gpu = on;
	SET sift.gpu_min_rows = 1;
	FOR shape, q IN VALUES ('ungrouped', 'SELECT count(*), sum(w), min(q), max(q) FROM ch WHERE k >= 2'),
	                       ('dense', 'SELECT g, count(*), sum(w) FROM ch GROUP BY g'),
	                       ('hash', 'SELECT w, count(*), sum(q) FROM ch GROUP BY w') LOOP
		gpu := '(the scan was not a SiftAggV6)';
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off, BUFFERS off) ' || q LOOP
			line := regexp_replace(trim(l), '; the CPU would take.*$', '');
			IF line LIKE 'GPU:%' OR line LIKE 'GPU Parts:%' THEN
				gpu := CASE WHEN gpu LIKE '(%' THEN line ELSE gpu || ' | ' || substr(line, 11) END;
			END IF;
		END LOOP;
		RETURN NEXT;
	END LOOP;
	RESET sift.gpu;
	RESET sift.gpu_min_rows;
END $$;

\echo clean
SELECT * FROM agree_all();
\if :{?proof}
SELECT * FROM gpu_lines();
\endif

-- inserts, which the index keeps in a slab and in parts of level 0 that the CPU reads
SELECT dml($$INSERT INTO @ SELECT i, i & 31, (i * 5) & 1023, 5 - i % 11, 10 + i % 17, i & 7 FROM generate_series(300001, 302500) i$$) \gset
VACUUM ch;
VACUUM chh;
\echo after inserts
SELECT * FROM agree_all();
\if :{?proof}
SELECT * FROM gpu_lines();
\endif

-- updates: a delete of the old row and an insert of the new one, which the CPU reads
SELECT dml('UPDATE @ SET w = w + 1, q = (q + 7) & 1023 WHERE id % 4999 = 1') \gset
VACUUM (INDEX_CLEANUP on) ch;
VACUUM (INDEX_CLEANUP on) chh;
\echo after updates
SELECT * FROM agree_all();
\if :{?proof}
SELECT * FROM gpu_lines();
\endif

-- a delete that touches every granule of the part the index was built as, folded into its deletion vector by the vacuum
SELECT dml('DELETE FROM @ WHERE id <= 300000 AND id % 1000 = 0 AND id % 4999 <> 1') \gset
VACUUM ch;
VACUUM chh;
\echo after a delete and its vacuum
SELECT * FROM agree_all();
\if :{?proof}
SELECT * FROM gpu_lines();
\endif

-- whole granules gone and the rest of the part thinned
SELECT dml('DELETE FROM @ WHERE id <= 300000 AND id % 4999 <> 1 AND (id <= 20000 OR (id > 100000 AND id <= 130000 AND h < 700))') \gset
VACUUM (INDEX_CLEANUP on) ch;
VACUUM (INDEX_CLEANUP on) chh;
\echo after a delete of whole granules
SELECT * FROM agree_all();
\if :{?proof}
SELECT * FROM gpu_lines();
\endif

-- and deletes again, of rows of the part the earlier ones thinned
SELECT dml('DELETE FROM @ WHERE id > 200000 AND id <= 300000 AND id % 7 = 0 AND id % 4999 <> 1') \gset
VACUUM (INDEX_CLEANUP on) ch;
VACUUM (INDEX_CLEANUP on) chh;
\echo after more deletes
SELECT * FROM agree_all();
\if :{?proof}
SELECT * FROM gpu_lines();
\endif
DROP FUNCTION agree_all(), agree(text), gpu_lines(), rows_of(text, text, text), dml(text);
DROP TABLE ch, chh;
DROP EXTENSION pg_sift;
