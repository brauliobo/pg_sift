-- The rows of a thousand keys joined to a table that has a sift_col index and no b-tree: a probe of the index for each key (a nested loop) reads a unit and a heap row, 8.5 us
-- measured at 1,000,000 rows, where the hash join reads the table (50 ms). The probe was priced as the first scan of the index is (the index opened, its pages walked, every
-- unit the zones may keep for a key the plan does not know): 82 units a probe for the arrival-order heap and 895 for the one sorted by the key, against 26,000 for the hash join
-- of 1,000,000 rows. The repeats of a scan open the index and walk its pages once, the unit a probe finds is looked up, and the units a probe finds by an equality with a
-- parameter are the widths of the zones over the range of the column.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE src AS SELECT (i::bigint * 7919 % 1000003)::int AS k, i % 1000 AS a, 'name ' || i % 3000 AS t FROM generate_series(1, 1000000) i;
CREATE TABLE arrival AS TABLE src;
CREATE TABLE sorted AS SELECT * FROM src ORDER BY k;
CREATE INDEX arrival_c ON arrival USING sift_col (k, a, t);
CREATE INDEX sorted_c ON sorted USING sift_col (k, a, t);
CREATE TABLE kk AS SELECT (i::bigint * 7919 % 1000003)::int AS k FROM generate_series(1, 1000) i;
VACUUM (FREEZE, ANALYZE) arrival, sorted, kk;

CREATE FUNCTION join_path(tbl text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'hash join';
BEGIN
	FOR l IN EXECUTE format('EXPLAIN (COSTS OFF) SELECT t.k, t.a, t.t FROM %I t JOIN kk ON t.k = kk.k', tbl) LOOP
		IF l ~ 'Nested Loop' THEN r := 'nested loop'; EXIT; END IF;
	END LOOP;
	RETURN r;
END $$;

SELECT tbl, join_path(tbl) AS path FROM unnest(ARRAY['arrival', 'sorted']) tbl;

-- the same rows with either
CREATE FUNCTION join_run(tbl text, nl boolean) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('enable_hashjoin', (NOT nl)::text, true);
	PERFORM set_config('enable_mergejoin', (NOT nl)::text, true);
	PERFORM set_config('enable_seqscan', (NOT nl)::text, true);
	EXECUTE format('SELECT md5(string_agg(concat_ws(%L, t.k, t.a, t.t), %L ORDER BY t.k, t.a, t.t)) FROM %I t JOIN kk ON t.k = kk.k', ',', ';', tbl) INTO r;
	RETURN r;
END $$;
SELECT tbl, join_run(tbl, true) = join_run(tbl, false) AS same_rows FROM unnest(ARRAY['arrival', 'sorted']) tbl;

DROP FUNCTION join_path(text), join_run(text, boolean);
DROP TABLE src, arrival, sorted, kk;
DROP EXTENSION pg_sift;
