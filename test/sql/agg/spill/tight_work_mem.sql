-- Statements planned at the default work_mem and run at 64kB: the window frame of a tied peer group with LEAD, the generic aggregates that cannot spill (count(DISTINCT), top-K),
-- the parallel aggregate over group keys and COUNT(DISTINCT) values longer than a chunk, the star join whose dimension holds 45,000 rows of one key, and the tournament of a Top-N with ties.
-- Each runs with the result of the heap plan, no error, and no temporary file left behind (a LIMIT included).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

CREATE FUNCTION tight(label text, q text, wm text DEFAULT '64kB') RETURNS text LANGUAGE plpgsql AS $$
DECLARE want text; got text; st text := 'ok'; n bigint := 0; node boolean := false; l text; tmp bigint;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('SELECT md5(coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''')) FROM (%s) r', q) INTO want;
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP node := node OR l LIKE '%Custom Scan (SiftAgg%'; END LOOP;
	DEALLOCATE ALL;
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	EXECUTE 'PREPARE tq AS ' || q;
	EXECUTE 'CREATE TEMP TABLE tq_warm ON COMMIT DROP AS EXECUTE tq';
	DROP TABLE tq_warm;
	PERFORM set_config('work_mem', wm, true);
	PERFORM set_config('hash_mem_multiplier', '1', true);
	BEGIN
		EXECUTE 'CREATE TEMP TABLE tq_out ON COMMIT DROP AS EXECUTE tq';
		EXECUTE 'SELECT count(*), md5(coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''')) FROM tq_out r' INTO n, got;
		DROP TABLE tq_out;
	EXCEPTION WHEN OTHERS THEN
		st := 'ERROR ' || SQLSTATE || ' ' || left(SQLERRM, 70);
	END;
	SELECT count(*) INTO tmp FROM pg_ls_tmpdir();
	RETURN format('%s | %s | rows=%s | parity=%s | v6=%s | tmp=%s', label, st, n, got IS NOT DISTINCT FROM want, node, tmp);
END $$;

CREATE FUNCTION tight_limit(label text, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE st text := 'ok'; tmp bigint; n bigint := 0;
BEGIN
	DEALLOCATE ALL;
	PERFORM set_config('plan_cache_mode', 'force_generic_plan', true);
	EXECUTE 'PREPARE tq AS ' || q;
	PERFORM set_config('work_mem', '64kB', true);
	PERFORM set_config('hash_mem_multiplier', '1', true);
	BEGIN
		EXECUTE 'CREATE TEMP TABLE tq_out ON COMMIT DROP AS EXECUTE tq';
		SELECT count(*) INTO n FROM tq_out;
	EXCEPTION WHEN OTHERS THEN
		st := 'ERROR ' || SQLSTATE;
	END;
	SELECT count(*) INTO tmp FROM pg_ls_tmpdir();
	RETURN format('%s | %s | tmp=%s', label, st, tmp);
END $$;

-- the window frame
CREATE TABLE ev_win (id int, p int, k int, v int);
INSERT INTO ev_win SELECT i, 1, CASE WHEN i <= 50000 THEN 7 ELSE 8 END, i % 1000 FROM generate_series(1, 50100) i;
CREATE INDEX ev_win_col ON ev_win USING sift_col (id, p, k, v);
VACUUM (FREEZE, ANALYZE) ev_win;
SELECT tight('window lead(2) over tied peer group', 'SELECT id, sum(v) OVER w AS s, lead(v, 2) OVER w AS l FROM ev_win WINDOW w AS (PARTITION BY p ORDER BY k)');
SELECT tight('window lead+lag+rank over tied peer group', 'SELECT id, count(*) OVER w AS c, lead(v) OVER w AS l, lag(v) OVER w AS g, rank() OVER w AS r FROM ev_win WINDOW w AS (PARTITION BY p ORDER BY k)');
SELECT tight_limit('window lead, LIMIT', 'SELECT id, sum(v) OVER w AS s, lead(v, 2) OVER w AS l FROM ev_win WINDOW w AS (PARTITION BY p ORDER BY k) LIMIT 100');

-- the generic aggregates (the statistics see 100 groups, the rows inserted after them make 60,000)
CREATE TABLE ev (id int, k int, g int, v int, t text);
INSERT INTO ev SELECT i, (i % 100) * 1000, i % 3, i % 1000, 'v' || (i % 100) FROM generate_series(1, 20000) i;
CREATE INDEX ev_col ON ev USING sift_col (id, k, g, v, t);
VACUUM (FREEZE, ANALYZE) ev;
INSERT INTO ev SELECT i, (i % 60000) * 7 + 1, i % 3, ((i::bigint * 7919) % 1000)::int, 't' || (i % 20000) FROM generate_series(20001, 120000) i;
SELECT tight('generic: string_agg + count(DISTINCT)', 'SELECT k, string_agg(v::text, '','' ORDER BY v) AS b, count(DISTINCT g) AS c FROM ev GROUP BY k');
SELECT tight('generic: string_agg + top-K', 'SELECT k, string_agg(v::text, '','' ORDER BY v) AS b FROM ev GROUP BY k ORDER BY k LIMIT 20');
SELECT tight('generic: bit_or + count(DISTINCT)', 'SELECT k, bit_or(v) AS b, count(DISTINCT g) AS c FROM ev GROUP BY k');
SELECT tight_limit('generic: string_agg + count(DISTINCT), LIMIT', 'SELECT k, string_agg(v::text, '','' ORDER BY v) AS b, count(DISTINCT g) AS c FROM ev GROUP BY k LIMIT 10');

-- the parallel aggregate with group keys and COUNT(DISTINCT) values longer than a chunk
RESET max_parallel_workers_per_gather;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET sift.test_agg_par_min_rows = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
CREATE TABLE ev_long (id int, g int, t text);
INSERT INTO ev_long SELECT i, i % 7, repeat(chr(97 + i % 26), 100000) || i FROM generate_series(1, 300) i;
CREATE INDEX ev_long_col ON ev_long USING sift_col (id, g, t);
VACUUM (FREEZE, ANALYZE) ev_long;
SELECT tight('parallel: group by long text', 'SELECT t, count(*) AS c FROM ev_long GROUP BY t');
SELECT tight('parallel: count(DISTINCT long text)', 'SELECT g, count(DISTINCT t) AS c FROM ev_long GROUP BY g');
SELECT tight('parallel: ungrouped count(DISTINCT long text)', 'SELECT count(DISTINCT t) AS c FROM ev_long');
SELECT tight('parallel: long group key, count(DISTINCT int)', 'SELECT t, count(DISTINCT id) AS c FROM ev_long GROUP BY t');
RESET seq_page_cost;
RESET random_page_cost;
SET max_parallel_workers_per_gather = 0;

-- the star join: the statistics see 20 rows of 20 keys, 50,000 rows of 3 keys follow, 45,000 of them of one key
CREATE TABLE ev_f (id int, g int, v int);
INSERT INTO ev_f SELECT i, i % 20, i % 100 FROM generate_series(1, 400) i;
CREATE INDEX ev_f_col ON ev_f USING sift_col (id, g, v);
VACUUM (FREEZE, ANALYZE) ev_f;
CREATE TABLE ev_dim (k int, label text, w int) WITH (autovacuum_enabled = false);
INSERT INTO ev_dim SELECT i % 20, 'l' || (i % 10), i % 7 FROM generate_series(1, 20) i;
ANALYZE ev_dim;
INSERT INTO ev_dim SELECT CASE WHEN i % 10 = 0 THEN i % 20 ELSE 1 END, 'l' || (i % 10), i % 7 FROM generate_series(21, 50000) i;
SELECT tight('star inner, 45k rows of one key', 'SELECT d.label, count(*) AS c, sum(f.v) AS s, sum(d.w) AS sw FROM ev_f f JOIN ev_dim d ON f.g = d.k AND d.w * 1 = d.w GROUP BY d.label');
SELECT tight('star left, 45k rows of one key', 'SELECT d.label, count(*) AS c, sum(d.w) AS sw FROM ev_f f LEFT JOIN ev_dim d ON f.g = d.k AND d.w * 1 = d.w AND d.w > 3 GROUP BY d.label');
SELECT tight('star inner, filter on the fact', 'SELECT d.label, count(*) AS c, sum(d.w) AS sw FROM ev_f f JOIN ev_dim d ON f.g = d.k AND d.w * 1 = d.w WHERE f.v < 50 GROUP BY d.label');

-- the tournament of a Top-N with ties: the ties held within work_mem / 4 and the rest in a file
CREATE FUNCTION ties_try(label text, wm text, n int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE st text := 'ok'; got bigint := 0; tmp bigint;
BEGIN
	PERFORM set_config('work_mem', wm, true);
	BEGIN
		SELECT count(*) INTO got FROM sift._test_topn(
			(SELECT array_agg(CASE WHEN i <= n THEN 1 ELSE 5 END ORDER BY i) FROM generate_series(1, n + 10) i), NULL, NULL,
			(SELECT array_agg(true ORDER BY i) FROM generate_series(1, n + 10) i), ARRAY[false], ARRAY[false],
			8192, 4, 4, 0, 10, 0, true, 1, false);
	EXCEPTION WHEN OTHERS THEN
		st := 'ERROR ' || SQLSTATE || ' ' || left(SQLERRM, 70);
	END;
	SELECT count(*) INTO tmp FROM pg_ls_tmpdir();
	RETURN format('%s | %s | rows=%s (want %s) | tmp=%s', label, st, got, n, tmp);
END $$;
SELECT ties_try('tournament with ties at 64kB, 100k ties', '64kB', 100000);
SELECT ties_try('tournament with ties at 4MB, 100k ties', '4MB', 100000);
SELECT ties_try('tournament with ties at 64kB, 1000 ties', '64kB', 1000);

DROP FUNCTION tight(text, text, text);
DROP FUNCTION tight_limit(text, text);
DROP FUNCTION ties_try(text, text, int);
DROP TABLE ev_win, ev, ev_long, ev_f, ev_dim;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
