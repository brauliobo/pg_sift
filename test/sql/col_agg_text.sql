-- SiftAgg over a sift_col index whose text is longer than a dictionary entry: units that store it inline, mixed with units that use
-- the dictionary, the write buffer, a merge. Every query runs with and without the pushdown; the results must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE ag (id int, g int, s text, f float8, n int);

CREATE FUNCTION agg_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION agg_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE agg_q (q text);
INSERT INTO agg_q VALUES
	('SELECT count(*), count(s), count(DISTINCT s) FROM ag'),
	('SELECT g, count(*), count(s), count(DISTINCT s), min(s COLLATE "C"), max(s COLLATE "C") FROM ag GROUP BY g'),
	('SELECT s, count(*) FROM ag WHERE s IS NOT NULL GROUP BY s HAVING count(*) > 1'),
	('SELECT count(*), sum(n) FROM ag WHERE s = repeat(''é'', 160) || ''3'''),
	('SELECT count(*) FROM ag WHERE s IN (repeat(''é'', 160) || ''3'', ''short7'', ''absent'')'),
	('SELECT count(*), max(n) FROM ag WHERE s LIKE repeat(''é'', 50) || ''%'' COLLATE "C"'),
	('SELECT count(*) FROM ag WHERE s <> ''short7'''),
	('SELECT g, count(*) FROM ag WHERE s NOT IN (''short7'', ''short8'') GROUP BY g'),
	('SELECT count(*), sum(length(s)) FROM ag WHERE g = 2'),
	('SELECT f, count(*) FROM ag WHERE s IS NULL AND f IS NOT NULL GROUP BY f');

INSERT INTO ag SELECT i, i % 5,
	CASE i % 7 WHEN 0 THEN NULL WHEN 1 THEN repeat('é', 150 + i % 300) || (i % 13) WHEN 2 THEN 'short' || i % 50
		 ELSE md5(i::text) || repeat('x', 200 + i % 100) END,
	CASE i % 17 WHEN 0 THEN NULL WHEN 1 THEN -0.0 ELSE i / 8.0 END, i % 100
FROM generate_series(1, 30000) i;
CREATE INDEX ag_col ON ag USING sift_col (id, g, s, f, n) WITH (write_buffer_limit = 4096);
VACUUM ANALYZE ag;
SELECT sift.col_verify('ag_col', true);
SELECT inline_chunks > 0 AS inline, const_chunks + for_chunks + pack_chunks + codec_chunks > 0 AS coded FROM sift.col_leaves('ag_col') WHERE leaf = 2;

SELECT query, pushed, same FROM (SELECT q AS query, agg_pushed(q) AS pushed, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM agg_q) z ORDER BY query COLLATE "C";

-- rows in the write buffer, then sealed, then deleted and merged
INSERT INTO ag SELECT 40000 + i, i % 5, CASE WHEN i % 3 = 0 THEN repeat('é', 160) || '3' ELSE 'short' || i % 20 END, i, i % 100 FROM generate_series(1, 500) i;
SELECT query, pushed, same FROM (SELECT q AS query, agg_pushed(q) AS pushed, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM agg_q) z ORDER BY query COLLATE "C";
SELECT sift.col_promote('ag_col') >= 0 AS promoted;
SELECT query, pushed, same FROM (SELECT q AS query, agg_pushed(q) AS pushed, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM agg_q) z ORDER BY query COLLATE "C";
DELETE FROM ag WHERE id % 4 = 0;
VACUUM ag;
SELECT sift.col_merge('ag_col', true) >= 0 AS merged;
SELECT sift.col_verify('ag_col', true);
SELECT query, pushed, same FROM (SELECT q AS query, agg_pushed(q) AS pushed, agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS same FROM agg_q) z ORDER BY query COLLATE "C";

DROP TABLE ag, agg_q;
DROP FUNCTION agg_rows, agg_pushed;
DROP EXTENSION pg_sift;
