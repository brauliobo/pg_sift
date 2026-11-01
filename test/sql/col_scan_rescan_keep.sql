-- An index scan that its executor starts over for each row of an outer query (the inner side of a nested loop on an equality) finds the segments of the index it opened for the last
-- key still current under the same snapshot, and begins on them again: the directory and the headers of the segments are not read for every key. The 200 probes of one statement pin eight buffers
-- each through the point directory (an Index Scan, a Bitmap Heap Scan, the heap page of the row among them), not the twelve of a scan that opens the index every time. The rows are the heap's through the changes between probes: rows
-- in the write buffer, deleted and updated rows, VACUUM, a promotion in the middle of the statement.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;

CREATE TABLE sk (id int, k int, tenant int, v int, pad text);
INSERT INTO sk SELECT i, (i * 7919) % 20011, 1 + i % 40, i % 100, repeat('x', 100) FROM generate_series(1, 20000) i;
CREATE INDEX sk_i ON sk USING sift_col (k, tenant, v, id) WITH (point_tid_columns = 'k', key_cut = 'tenant');
-- 300 outer keys: some the table has, some it lacks, a repeat, a NULL
CREATE TABLE sk_o AS SELECT n AS i, CASE WHEN n % 41 = 0 THEN NULL WHEN n % 17 = 0 THEN 30000 + n WHEN n % 29 = 0 THEN 7 ELSE (n * 31337) % 20011 END AS k FROM generate_series(1, 300) n;
VACUUM (FREEZE, ANALYZE) sk, sk_o;

-- the buffers the inner scan of a statement pins for each of its probes (the node over its loops), and whether the plan is the nested loop on the index
CREATE FUNCTION inner_buffers(q text, OUT per_probe numeric, OUT probes int, OUT inner_scan text) LANGUAGE plpgsql AS $$
DECLARE l text; seen boolean := false; blocks bigint := 0;
BEGIN
	probes := 0;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF NOT seen AND l ~ '(Index Scan|Index Only Scan|Bitmap Heap Scan) (using|on) ' THEN
			seen := true; inner_scan := substring(l FROM '(Index Scan|Index Only Scan|Bitmap Heap Scan)'); probes := substring(l FROM 'loops=([0-9]+)')::int;
		ELSIF seen AND l ~ '^\s+Buffers: shared' THEN
			blocks := coalesce(substring(l FROM 'hit=([0-9]+)')::bigint, 0) + coalesce(substring(l FROM 'read=([0-9]+)')::bigint, 0);
			EXIT;
		END IF;
	END LOOP;
	per_probe := round(blocks::numeric / greatest(probes, 1), 1);
END $$;

CREATE FUNCTION probe_q(kind text) RETURNS text LANGUAGE sql AS $$
	SELECT 'SELECT o.i, s.v FROM sk_o o JOIN sk s ON s.k = o.k' $$;

SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_seqscan = off; SET enable_material = off;
-- an Index Scan, and a Bitmap Heap Scan, for each key of the outer rows
SET enable_indexonlyscan = off; SET enable_bitmapscan = off;
SELECT p.inner_scan, p.probes, p.per_probe <= 9 AS few_buffers FROM inner_buffers(probe_q('')) p;
SET enable_bitmapscan = on; SET enable_indexscan = off;
SELECT p.inner_scan, p.probes, p.per_probe <= 9 AS few_buffers FROM inner_buffers(probe_q('')) p;
RESET enable_indexonlyscan; RESET enable_bitmapscan; RESET enable_indexscan; RESET enable_seqscan; RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_material;

-- the rows are the heap's: the same statement with the index scans off, before and after each change between probes
CREATE FUNCTION sk_rows(on_ bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('enable_hashjoin', (NOT on_)::text, true);
	PERFORM set_config('enable_mergejoin', (NOT on_)::text, true);
	PERFORM set_config('enable_seqscan', (NOT on_)::text, true);
	PERFORM set_config('enable_indexscan', on_::text, true);
	PERFORM set_config('enable_bitmapscan', on_::text, true);
	PERFORM set_config('enable_indexonlyscan', 'off', true);
	PERFORM set_config('enable_material', 'off', true);
	EXECUTE 'SELECT string_agg(z::text, '','' ORDER BY z.i, z.v) FROM (' || probe_q('') || ') z' INTO r;
	RETURN r;
END $$;
CREATE TABLE sk_step (n int, label text, same boolean);
CREATE FUNCTION sk_check(lbl text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO sk_step SELECT coalesce(max(n), 0) + 1, lbl, sk_rows(true) = sk_rows(false) FROM sk_step;
END $$;
SELECT sk_check('as built');
DELETE FROM sk WHERE id % 13 = 0;
UPDATE sk SET k = k + 1 WHERE id % 17 = 0;
INSERT INTO sk SELECT 30000 + n, (n * 31337) % 20011, 1 + n % 40, n % 100, 'x' FROM generate_series(1, 500) n;
SELECT sk_check('writes in the write buffer, deleted and updated rows');
VACUUM sk;
SELECT sk_check('after VACUUM');
SELECT sift.col_promote('sk_i'::regclass) >= 0 AS promoted;
SELECT sk_check('after a promotion');
SELECT n, label, same FROM sk_step ORDER BY n;

-- writes and a promotion in the middle of the statement: the outer rows call a function that does them between two probes; the statement keeps its snapshot
CREATE FUNCTION sk_mut(i int) RETURNS int LANGUAGE plpgsql AS $$
BEGIN
	IF i = 150 AND current_setting('sk.mut', true) = 'on' THEN
		DELETE FROM sk WHERE id % 11 = 0;
		INSERT INTO sk SELECT 50000 + n, (n * 31337) % 20011, 1 + n % 40, n % 100, 'x' FROM generate_series(1, 300) n;
		PERFORM sift.col_promote('sk_i'::regclass);
	END IF;
	RETURN i;
END $$;
DO $$
DECLARE before text; during text; q text := 'SELECT string_agg((v.i, s.v)::text, '','' ORDER BY v.i, s.v) FROM (SELECT i, k, sk_mut(i) AS m FROM sk_o OFFSET 0) v JOIN sk s ON s.k = v.k';
BEGIN
	SET LOCAL enable_hashjoin = off; SET LOCAL enable_mergejoin = off; SET LOCAL enable_seqscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_material = off;
	SET LOCAL enable_bitmapscan = off;
	EXECUTE q INTO before;
	PERFORM set_config('sk.mut', 'on', true);
	EXECUTE q INTO during;
	RAISE WARNING 'writes in the statement: %', CASE WHEN during = before THEN 'the rows of the snapshot' ELSE 'DIFFERS' END;
END $$;

DROP FUNCTION inner_buffers(text), probe_q(text), sk_rows(bool), sk_check(text), sk_mut(int);
DROP TABLE sk, sk_o, sk_step;
DROP EXTENSION pg_sift;
