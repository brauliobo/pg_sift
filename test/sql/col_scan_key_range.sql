-- A scan whose conditions confine it to keys of the key_cut opens only the segments of the partitions that may hold them: the index scan and the aggregate scan (serial and in workers) over
-- indexes cut by one, two, three and four columns (a key of four is past the depth of a bound). Every answer is the one of a scan that opens every segment (sift.test_col_open_all) and of
-- the heap, rows of the write buffer and deleted rows included; a scan that no condition confines opens all of them, and one confined to an empty set none
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.test_col_segment_kb = 1024;
SET sift.col_auto_merge_partition_kb = 64;
SET sift.test_col_net = off;
SET statement_timeout = '120s';

CREATE FUNCTION make(depth int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE t text := 'k' || depth;
BEGIN
	EXECUTE format('CREATE TABLE %I (id int PRIMARY KEY, a int NOT NULL, b int NOT NULL, c int NOT NULL, d int NOT NULL, v int, w int) WITH (autovacuum_enabled = false)', t);
	EXECUTE format($i$INSERT INTO %I SELECT g, 1 + g %% 3, 1 + (g::bigint * 7919 %% 5)::int, (g::bigint * 2654435761 %% 4294967296 / 65536)::int %% 4000, g %% 7, g %% 100,
		(g::bigint * 40503 %% 1000003)::int FROM generate_series(1, 150000) g$i$, t);
	EXECUTE format('INSERT INTO %I SELECT 150000 + g, 4, 1, g, 0, 0, g FROM generate_series(1, 2000) g', t);
	EXECUTE format('INSERT INTO %I SELECT 170000 + g, 5, 1, 1, g %% 7, g %% 100, g FROM generate_series(1, 60000) g', t);
	EXECUTE format('CREATE INDEX %I ON %I USING sift_col (id, a, b, c, d, v, w) WITH (key_cut = %L, dead_rewrite_pct = 100)', t || '_ix', t,
		(SELECT string_agg(l, ', ') FROM unnest((ARRAY['a', 'b', 'c', 'd'])[1:depth]) l));
	EXECUTE format('DELETE FROM %I WHERE id %% 50 = 7', t);
END $$;
CREATE FUNCTION settle(depth int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE t text := 'k' || depth;
BEGIN
	PERFORM sift.col_maintain(t || '_ix');
	EXECUTE format('INSERT INTO %I SELECT 300000 + g, 1 + g %% 3, 2, g %% 4000, g %% 7, 1, g FROM generate_series(1, 3000) g', t);
END $$;
SELECT make(d) FROM generate_series(1, 4) d;
SELECT format('VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON, ANALYZE) k%s', d) FROM generate_series(1, 4) d \gexec
SELECT settle(d) FROM generate_series(1, 4) d;
SELECT d, (SELECT count(*) FROM sift._test_dir_parts('k' || d || '_ix')) > 8 AS partitions, sift.col_verify('k' || d || '_ix', true) FROM generate_series(1, 4) d;

CREATE TABLE qs (n serial, q text);
INSERT INTO qs (q) VALUES
	('SELECT id, w FROM @ WHERE a = 2 AND b = 3 AND c BETWEEN 1000 AND 1100'),
	('SELECT id, w FROM @ WHERE a = 2 AND b = 3 AND c = 17'),
	('SELECT id FROM @ WHERE a = 4'),
	('SELECT id FROM @ WHERE a = 2 AND b = 3'),
	('SELECT id FROM @ WHERE a IN (1, 3) AND b = 5 AND c < 200'),
	('SELECT id FROM @ WHERE a = 2 AND b >= 4'),
	('SELECT id FROM @ WHERE a = 1 AND b = 2 AND c = 5 AND d = 5'),
	('SELECT id FROM @ WHERE a = 9'),
	('SELECT id FROM @ WHERE b = 3 AND c = 17'),
	('SELECT b, count(*), sum(w) FROM @ WHERE a = 2 AND c BETWEEN 1000 AND 1100 GROUP BY b'),
	('SELECT c / 500, count(*), sum(w) FROM @ WHERE a = 3 AND b = 2 GROUP BY 1'),
	('SELECT d, count(*), sum(v) FROM @ WHERE a = 1 AND b = 4 AND c < 2000 GROUP BY d'),
	('SELECT count(*), sum(w), min(id), max(id) FROM @ WHERE a = 4'),
	('SELECT count(*), sum(w) FROM @ WHERE a = 2 AND b = 3 AND c >= 3000'),
	('SELECT a, b, count(*) FROM @ WHERE a >= 2 AND b <= 2 GROUP BY 1, 2'),
	('SELECT count(*), sum(w) FROM @ WHERE a = 7'),
	('SELECT count(*), sum(w) FROM @ WHERE a = 5 AND b = 1 AND c = 1 AND d = 3'),
	('SELECT id FROM @ WHERE a = 5 AND b = 1 AND c = 1 AND d = 3 AND w < 5000'),
	('SELECT count(*), sum(w) FROM @'),
	('SELECT a, count(*) FROM @ GROUP BY 1'),
	('SELECT count(*) FROM @ WHERE c < 100');

-- the answer of a statement, the segments it opened, and whether its plan reads the index
CREATE FUNCTION answer(q text, enable_agg bool, indexes bool, open_all bool, OUT segs int8, OUT rows text, OUT via_index bool) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM set_config('sift.enable_agg', enable_agg::text, true);
	PERFORM set_config('sift.test_col_open_all', open_all::text, true);
	PERFORM set_config('enable_seqscan', (NOT indexes)::text, true);
	PERFORM set_config('enable_indexscan', indexes::text, true);
	PERFORM set_config('enable_indexonlyscan', indexes::text, true);
	PERFORM set_config('enable_bitmapscan', indexes::text, true);
	via_index := false;
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		via_index := via_index OR line ~ 'SiftAgg|Index';
	END LOOP;
	PERFORM sift._test_col_segments_opened(true);
	EXECUTE 'SELECT coalesce(string_agg(r::text, '';'' ORDER BY r::text), '''') FROM (' || q || ') r' INTO rows;
	segs := sift._test_col_segments_opened(true);
END $$;

CREATE TABLE report (depth int, n int, same_as_full bool, same_as_heap bool, via_index bool, fewer bool, all_or_less bool);
DO $$
DECLARE d int; r record; t text; k record; f record; h record;
BEGIN
	FOR d IN 1..4 LOOP
		t := 'k' || d;
		FOR r IN SELECT n, replace(q, '@', t) AS q FROM qs ORDER BY n LOOP
			SELECT * INTO k FROM answer(r.q, true, true, false);
			SELECT * INTO f FROM answer(r.q, true, true, true);
			SELECT * INTO h FROM answer(r.q, false, false, false);
			INSERT INTO report VALUES (d, r.n, k.rows = f.rows, k.rows = h.rows, k.via_index, k.segs < f.segs, k.segs <= f.segs);
		END LOOP;
	END LOOP;
END $$;
SELECT depth, n, same_as_full, same_as_heap, via_index, fewer, all_or_less FROM report ORDER BY depth, n;
-- no scan opens more than a full one, and every answer is the full one's and the heap's
SELECT count(*) FILTER (WHERE NOT same_as_full OR NOT same_as_heap OR NOT all_or_less) AS wrong FROM report;


-- the keys change from one probe of a join to the next: a source opened for one value is not the source of the next
CREATE TABLE qj (n serial, q text);
INSERT INTO qj (q) VALUES
	('SELECT o.x, t.id, t.w FROM (VALUES (1), (2), (3), (2)) o(x), LATERAL (SELECT id, w FROM @ WHERE a = o.x AND b = 3 AND c < 40 OFFSET 0) t'),
	('SELECT o.x, t.* FROM (VALUES (3), (1), (2), (5)) o(x), LATERAL (SELECT id, w FROM @ WHERE a = o.x AND b = 2 ORDER BY w DESC, id LIMIT 3) t'),
	('SELECT o.x, t.* FROM (VALUES (3), (1), (2), (4)) o(x), LATERAL (SELECT id, c FROM @ WHERE a = o.x AND b = 4 AND c < 1000 ORDER BY c DESC, id LIMIT 5) t');
CREATE TABLE joined (depth int, n int, same_as_full bool, same_as_heap bool, via_index bool);
DO $$
DECLARE d int; r record; k record; f record; h record;
BEGIN
	PERFORM set_config('enable_hashjoin', 'off', true);
	PERFORM set_config('enable_mergejoin', 'off', true);
	FOR d IN 1..4 LOOP
		FOR r IN SELECT n, replace(q, '@', 'k' || d) AS q FROM qj ORDER BY n LOOP
			SELECT * INTO k FROM answer(r.q, true, true, false);
			SELECT * INTO f FROM answer(r.q, true, true, true);
			SELECT * INTO h FROM answer(r.q, false, false, false);
			INSERT INTO joined VALUES (d, r.n, k.rows = f.rows, k.rows = h.rows, k.via_index);
		END LOOP;
	END LOOP;
END $$;
SELECT depth, n, same_as_full, same_as_heap, via_index FROM joined ORDER BY depth, n;

-- float and numeric keys: a NaN (which sorts after every number, and is stored as a NULL) and the two zeros answer as the heap does
CREATE TABLE kf (id int PRIMARY KEY, f float8 NOT NULL, n numeric(12, 2) NOT NULL, w int) WITH (autovacuum_enabled = false);
INSERT INTO kf SELECT g, CASE WHEN g % 97 = 0 THEN 'NaN'::float8 WHEN g % 3 = 0 THEN '-0'::float8 WHEN g % 3 = 1 THEN 0::float8 ELSE (g % 50 + 1)::float8 END,
	CASE WHEN g % 83 = 0 THEN 'NaN'::numeric ELSE (g % 40)::numeric END, g FROM generate_series(1, 200000) g;
CREATE INDEX kf_f ON kf USING sift_col (id, f, w) WITH (key_cut = 'f');
CREATE TABLE kn (id int PRIMARY KEY, f float8 NOT NULL, n numeric(12, 2) NOT NULL, w int) WITH (autovacuum_enabled = false);
INSERT INTO kn SELECT * FROM kf;
CREATE INDEX kn_n ON kn USING sift_col (id, n, w) WITH (key_cut = 'n');
VACUUM (FREEZE, ANALYZE) kf;
VACUUM (FREEZE, ANALYZE) kn;
SELECT sift.col_maintain('kf_f'), sift.col_maintain('kn_n');
SELECT i, (SELECT count(*) FROM sift._test_dir_parts(i)) > 3 AS partitions FROM unnest(ARRAY['kf_f', 'kn_n']) i;
SELECT d.t, d.q, a.rows = f.rows AS same_as_full, a.rows = h.rows AS same_as_heap
FROM (VALUES ('SELECT id FROM kf WHERE f >= 40', 'kf'), ('SELECT id FROM kf WHERE f = 0', 'kf'), ('SELECT id FROM kf WHERE f = ''NaN''', 'kf'), ('SELECT count(*), sum(w) FROM kf WHERE f > 10', 'kf'),
	('SELECT id FROM kn WHERE n >= 30', 'kn'), ('SELECT id FROM kn WHERE n = 7', 'kn'), ('SELECT count(*), sum(w) FROM kn WHERE n < 5', 'kn')) d(q, t),
	LATERAL answer(d.q, true, true, false) a(segs, rows, via_index), LATERAL answer(d.q, true, true, true) f(segs, rows, via_index), LATERAL answer(d.q, false, false, false) h(segs, rows, via_index);

-- the same in workers: the leader's directory is the one they open on
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
CREATE FUNCTION in_workers(q text, open_all bool, OUT workers bool, OUT rows text) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.test_col_open_all', open_all::text, true);
	workers := false;
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		workers := workers OR line LIKE '%Workers Planned%';
	END LOOP;
	EXECUTE 'SELECT coalesce(string_agg(r::text, '';'' ORDER BY r::text), '''') FROM (' || q || ') r' INTO rows;
END $$;
SELECT d, q.n, p.workers, p.rows = f.rows AS same_as_full, p.rows = h.rows AS same_as_heap
FROM generate_series(1, 4) d, (VALUES (1, 'SELECT b, count(*), sum(w) FROM @ WHERE a = 2 AND c < 3000 GROUP BY b'), (2, 'SELECT count(*), sum(w) FROM @ WHERE a = 1 AND b = 4'),
	(3, 'SELECT d, count(*), sum(v) FROM @ WHERE a = 3 AND b = 2 AND c BETWEEN 500 AND 2500 GROUP BY d')) q(n, q),
	LATERAL in_workers(replace(q.q, '@', 'k' || d), false) p(workers, rows) JOIN LATERAL in_workers(replace(q.q, '@', 'k' || d), true) f(workers, rows) ON true
	JOIN LATERAL answer(replace(q.q, '@', 'k' || d), false, false, false) h(segs, rows, via_index) ON true
ORDER BY d, q.n;
DROP FUNCTION in_workers(text, bool);
DROP FUNCTION answer(text, bool, bool, bool), make(int), settle(int);
DROP TABLE qs, qj, report, joined, kf, kn, k1, k2, k3, k4;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
