-- ORDER BY <columns> LIMIT n from the column units (docs/COL-SEARCH.md, "Top-N"): the filters, the match set of a text search and the sort
-- columns are read from the units, the heap only for the n rows of the result. Every query runs twice, the plain plan as the reference
-- (sift.enable_agg off) and the pushed one, and the rows are compared in order: after the build and after each kind of write.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;

-- pad makes a page hold about 5 rows: 24000 rows are about 19 superblocks, more than a window of the visit order
CREATE TABLE ct (id serial PRIMARY KEY, store int, status text, amount numeric(10,2), placed date, ts timestamp, seq int, f float8, memo text,
				 pad text DEFAULT repeat('x', 1500));
ALTER TABLE ct ALTER COLUMN pad SET STORAGE PLAIN;
CREATE FUNCTION ct_words(seed int) RETURNS text LANGUAGE sql IMMUTABLE AS $$
	SELECT string_agg((ARRAY['fast','late','damaged','refund','gift','invoice','exchange','missing','perfect','broken','express','pickup',
							 'thanks','wrong','size','color','clean','wire','box','label'])[1 + (hashint4(seed * 31 + w) & 2147483647) % 20], ' ')
	FROM generate_series(1, 3 + seed % 5) w $$;
INSERT INTO ct (store, status, amount, placed, ts, seq, f, memo)
SELECT 1 + (hashint4(i) & 2147483647) % 40,
	   CASE WHEN i % 23 = 0 THEN NULL ELSE (ARRAY['paid','shipped','delivered','cancelled'])[1 + (hashint4(i + 7) & 2147483647) % 4] END,
	   CASE WHEN i % 997 = 0 THEN 'NaN'::numeric WHEN i % 29 = 0 THEN NULL ELSE ((hashint4(i + 3) & 2147483647) % 30000)::numeric / 100 END,
	   CASE WHEN i % 31 = 0 THEN NULL ELSE date '2024-01-01' + (hashint4(i + 5) & 2147483647) % 700 END,
	   timestamp '2024-01-01' + (hashint4(i + 11) & 2147483647) % 60000000 * interval '1 second',
	   i * 3,
	   CASE WHEN i % 991 = 0 THEN 'NaN'::float8 WHEN i % 37 = 0 THEN NULL WHEN i % 41 = 0 THEN -0.0 ELSE ((hashint4(i + 13) & 2147483647) % 100000) / 7.0 END,
	   CASE WHEN i % 41 = 0 THEN NULL ELSE ct_words(i) END
FROM generate_series(1, 24000) i;
CREATE INDEX ct_bm ON ct USING sift (memo);
CREATE INDEX ct_col ON ct USING sift_col (id, store, status, amount, placed, ts, seq, f) WITH (mutable_limit = 256);
VACUUM (ANALYZE) ct;

CREATE FUNCTION ct_mode(pushed bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
END $$;

-- 'same' / 'DIFFERENT' (the rows and their order), '+topn' when the pushed plan is the top-N scan, and the rows
CREATE FUNCTION ct_check(q text, arg text DEFAULT NULL) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; plan text := ''; line text; res text;
BEGIN
	DROP TABLE IF EXISTS ct_a, ct_b;
	PERFORM ct_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q USING arg LOOP plan := plan || line || E'\n'; END LOOP;
	EXECUTE 'CREATE TEMP TABLE ct_a AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x' USING arg;
	PERFORM ct_mode(false);
	EXECUTE 'CREATE TEMP TABLE ct_b AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x' USING arg;
	SELECT count(*) INTO n FROM ((SELECT pos, r FROM ct_a EXCEPT ALL SELECT pos, r FROM ct_b) UNION ALL (SELECT pos, r FROM ct_b EXCEPT ALL SELECT pos, r FROM ct_a)) d;
	res := CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END;
	RETURN res || CASE WHEN plan ~ 'Top-N: ' THEN '+topn' ELSE '' END || ' ' || (SELECT count(*) FROM ct_b);
END $$;

-- one number of the EXPLAIN ANALYZE of the pushed plan
CREATE FUNCTION ct_stat(q text, field text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE line text; v bigint;
BEGIN
	PERFORM ct_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF line ~ ('^ *' || field || ': ') THEN v := regexp_replace(line, '^ *[A-Za-z -]+: ', '')::bigint; END IF;
	END LOOP;
	RETURN v;
END $$;

CREATE TABLE ct_q (tag text PRIMARY KEY, q text NOT NULL);
INSERT INTO ct_q VALUES
	('newest',     $q$SELECT id, ts, memo FROM ct WHERE memo @@> 'gift' ORDER BY ts DESC, id DESC LIMIT 10$q$),
	('oldest',     $q$SELECT id, ts, memo FROM ct WHERE memo @@> 'gift' ORDER BY ts, id LIMIT 7$q$),
	('filters',    $q$SELECT id, ts, amount FROM ct WHERE memo @@> 'clean wire' AND status = 'paid' AND store > 10 AND placed > date '2024-06-01' ORDER BY ts DESC, id LIMIT 12$q$),
	('offset',     $q$SELECT id, ts FROM ct WHERE memo @@> 'express OR pickup' ORDER BY ts DESC, id DESC LIMIT 5 OFFSET 7$q$),
	('date_desc',  $q$SELECT id, placed FROM ct WHERE memo @@> 'late' ORDER BY placed DESC, id LIMIT 15$q$),
	('date_nl',    $q$SELECT id, placed FROM ct WHERE memo @@> 'late' ORDER BY placed DESC NULLS LAST, id LIMIT 15$q$),
	('date_nf',    $q$SELECT id, placed FROM ct WHERE memo @@> 'late' ORDER BY placed NULLS FIRST, id LIMIT 15$q$),
	('date_asc',   $q$SELECT id, placed FROM ct WHERE memo @@> 'late' ORDER BY placed, id LIMIT 15$q$),
	('num_desc',   $q$SELECT id, amount FROM ct WHERE memo @@> 'refund' ORDER BY amount DESC, id LIMIT 20$q$),
	('num_asc',    $q$SELECT id, amount FROM ct WHERE memo @@> 'refund' ORDER BY amount, id LIMIT 20$q$),
	('num_nan',    $q$SELECT id, amount FROM ct ORDER BY amount DESC NULLS LAST, id LIMIT 30$q$),
	('float_desc', $q$SELECT id, f FROM ct WHERE memo @@> 'size' ORDER BY f DESC, id LIMIT 9$q$),
	('float_asc',  $q$SELECT id, f FROM ct WHERE memo @@> 'size' ORDER BY f, id LIMIT 9$q$),
	('float_all',  $q$SELECT id, f FROM ct ORDER BY f DESC NULLS LAST, id LIMIT 40$q$),
	('two_keys',   $q$SELECT id, store, amount FROM ct WHERE memo @@> 'box' ORDER BY store DESC, amount, id LIMIT 25$q$),
	('three_keys', $q$SELECT id, store, status FROM ct WHERE memo @@> 'box OR wire' ORDER BY store, placed DESC, id DESC LIMIT 25$q$),
	('no_search',  $q$SELECT id, ts FROM ct WHERE status = 'shipped' AND store < 10 ORDER BY ts DESC, id DESC LIMIT 10$q$),
	('no_filter',  $q$SELECT id, ts FROM ct ORDER BY ts DESC, id DESC LIMIT 10$q$),
	('id_desc',    $q$SELECT id FROM ct WHERE memo @@> 'wire' ORDER BY id DESC LIMIT 6$q$),
	('phrase',     $q$SELECT id, ts FROM ct WHERE memo @@> '"clean wire"' ORDER BY ts DESC, id LIMIT 10$q$),
	('prefix',     $q$SELECT id, ts FROM ct WHERE memo @@> 'exch*' ORDER BY ts, id LIMIT 10$q$),
	('fuzzy',      $q$SELECT id, ts FROM ct WHERE memo @@> 'wifii~1' ORDER BY ts, id LIMIT 10$q$),
	('boolean',    $q$SELECT id, ts FROM ct WHERE memo @@> '(damaged OR broken) AND NOT refund' ORDER BY ts DESC, id LIMIT 10$q$),
	('none',       $q$SELECT id, ts FROM ct WHERE memo @@> 'zzzzzz' ORDER BY ts DESC, id LIMIT 10$q$),
	('few',        $q$SELECT id, ts FROM ct WHERE memo @@> 'gift' AND store = 3 AND status = 'paid' ORDER BY ts DESC, id LIMIT 5000$q$),
	('expr_out',   $q$SELECT id, upper(memo) AS m, amount * 2 AS a2 FROM ct WHERE memo @@> 'perfect' ORDER BY seq DESC LIMIT 8$q$),
	('junk_key',   $q$SELECT memo FROM ct WHERE memo @@> 'perfect' ORDER BY seq DESC LIMIT 8$q$),
	('seq_desc',   $q$SELECT id, seq FROM ct WHERE memo @@> 'thanks' ORDER BY seq DESC LIMIT 10$q$),
	('seq_asc',    $q$SELECT id, seq FROM ct WHERE memo @@> 'thanks' ORDER BY seq LIMIT 10$q$),
	('seq_plain',  $q$SELECT id, seq FROM ct WHERE status = 'paid' ORDER BY seq DESC LIMIT 10$q$),
	('text_key',   $q$SELECT id, status FROM ct WHERE memo @@> 'gift' ORDER BY status, id LIMIT 10$q$),
	('col_or',     $q$SELECT id, ts FROM ct WHERE (status = 'paid' OR store < 10) AND memo @@> 'wire' ORDER BY ts, id LIMIT 10$q$),
	('in_or',      $q$SELECT id, ts FROM ct WHERE memo @@> 'wire' OR status = 'paid' ORDER BY ts, id LIMIT 10$q$);

CREATE FUNCTION ct_stage(name text) RETURNS TABLE (stage text, results text) LANGUAGE sql AS $$
	SELECT name, string_agg(tag || '=' || ct_check(q), E'\n' ORDER BY tag) FROM ct_q $$;

SELECT * FROM ct_stage('build');
SET sift.test_search_chunk_sbs = 1;
SELECT * FROM ct_stage('chunks of one superblock');
RESET sift.test_search_chunk_sbs;
SELECT ct_check('SELECT id, ts FROM ct WHERE memo @@> $1 ORDER BY ts DESC, id LIMIT 10', 'clean wire') AS param;
SELECT ct_check('SELECT id, ts FROM ct WHERE memo @@> $1 ORDER BY ts DESC, id LIMIT 10') AS param_null;
SELECT ct_check('SELECT id, ts FROM ct WHERE memo @@> ''gift'' ORDER BY ts DESC, id LIMIT 10 OFFSET 3') AS offset_lit;

-- the heap is read for the rows of the result only, and a column that grows with the table lets the scan skip superblocks
SELECT ct_stat(q, 'Heap Fetches') AS heap_fetches, ct_stat(q, 'Search Matches') > 100 AS many_matches
FROM (SELECT $q$SELECT id, ts FROM ct WHERE memo @@> 'gift' ORDER BY ts DESC, id DESC LIMIT 10$q$ AS q) x;
SELECT ct_stat(q, 'Superblocks Skipped By Top-N') > 0 AS skipped_desc
FROM (SELECT $q$SELECT id, seq FROM ct WHERE memo @@> 'thanks' ORDER BY seq DESC LIMIT 10$q$ AS q) x;
SELECT ct_stat(q, 'Superblocks Skipped By Top-N') > 0 AS skipped_asc
FROM (SELECT $q$SELECT id, seq FROM ct WHERE memo @@> 'thanks' ORDER BY seq LIMIT 10$q$ AS q) x;
SELECT ct_stat(q, 'Superblocks Skipped By Top-N') = 0 AS none_skipped_when_random
FROM (SELECT $q$SELECT id, ts FROM ct WHERE memo @@> 'gift' ORDER BY ts DESC, id DESC LIMIT 10$q$ AS q) x;

-- a sort column that grows with the table is read from its best end: the newest superblock holds the 10 rows, and the units before it are
-- not opened (one unit; a visit in table order opened every unit of its window)
SELECT ct_stat('SELECT id FROM ct WHERE memo @@> ''fast'' ORDER BY id DESC LIMIT 10', 'Units') AS units_desc,
	   ct_stat('SELECT id FROM ct WHERE memo @@> ''fast'' ORDER BY id LIMIT 10', 'Units') AS units_asc;

CREATE FUNCTION ct_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE plan text := ''; line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	RETURN CASE WHEN plan ~ 'Top-N: ' THEN 'top-n' WHEN plan ~ 'Index Scan' THEN 'btree' ELSE 'other' END;
END $$;
-- the top-N opens the match set whole (the posting lists of the words), whatever part of the table it reads: with the btree plan priced a
-- little above the top-N cost without that term and below the cost with it, the btree plan is the cheaper one (COST: what a row costs to test)
BEGIN;
ALTER FUNCTION sift.match(text, text) COST 780;
SELECT ct_plan('SELECT id, ts FROM ct WHERE memo @@> ''fast AND late'' ORDER BY id DESC LIMIT 10') AS open_cost_id;
ROLLBACK;

-- the GUC
SET sift.enable_topn = off;
SELECT ct_check('SELECT id, ts FROM ct WHERE memo @@> ''gift'' ORDER BY ts DESC, id LIMIT 10') NOT LIKE '%+topn%' AS no_topn_when_off;
RESET sift.enable_topn;

-- inserts land in the mutable segments of both indexes, unvacuumed
INSERT INTO ct (store, status, amount, placed, ts, seq, f, memo)
SELECT 1 + (hashint4(i) & 2147483647) % 40, (ARRAY['paid','shipped'])[1 + i % 2], 20 + i % 90, date '2025-06-01' + i % 20,
	   timestamp '2025-01-01' + i * interval '1 minute', 100000 + i, i / 3.0, ct_words(i + 100000)
FROM generate_series(1, 2000) i;
SELECT * FROM ct_stage('insert');

-- updates make new versions (new TIDs, new text); the old ones are dead but not vacuumed
UPDATE ct SET memo = memo || ' wire' WHERE id % 9 = 0;
UPDATE ct SET status = 'paid', ts = ts + interval '3 days' WHERE id % 10 = 0;
SELECT * FROM ct_stage('update');

DELETE FROM ct WHERE id % 13 = 0;
SELECT * FROM ct_stage('delete');

-- rows of this transaction are seen by this transaction only; a rolled back insert is seen by nobody
BEGIN;
INSERT INTO ct (store, status, amount, placed, ts, seq, f, memo)
SELECT 1, 'paid', 500 + i, date '2026-01-01', timestamp '2027-01-01' + i * interval '1 second', 900000 + i, i, 'clean wire broken box gift thanks perfect' FROM generate_series(1, 30) i;
DELETE FROM ct WHERE id % 17 = 0;
SELECT * FROM ct_stage('own');
ROLLBACK;
SELECT * FROM ct_stage('rollback');

-- the same after VACUUM
VACUUM ct;
SELECT * FROM ct_stage('vacuum');

-- and after the mutable segments become immutable ones
SELECT sift.col_promote('ct_col');
SELECT * FROM ct_stage('promote');

-- workers do not change the plan of a top-N
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT * FROM ct_stage('workers');
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- against an ordered btree on the sort column the planner keeps the cheaper plan: a common word is found in a few rows of the index, a rare
-- combination of words is not (the cost of the top-N is its units and its match set, whatever the table order)
CREATE INDEX ct_ts_idx ON ct (ts);
SELECT ct_plan('SELECT id, ts FROM ct WHERE memo @@> ''fast'' ORDER BY ts DESC LIMIT 10') AS common_ts,
	   ct_plan('SELECT id, ts FROM ct WHERE memo @@> ''fast'' ORDER BY id DESC LIMIT 10') AS common_id,
	   ct_plan('SELECT id, ts FROM ct WHERE memo @@> ''gift AND wire AND label'' ORDER BY ts DESC LIMIT 10') AS rare_ts,
	   ct_plan('SELECT id, ts FROM ct WHERE memo @@> ''gift AND wire AND label'' ORDER BY id DESC LIMIT 10') AS rare_id;

-- a table in column format has no TIDs to give: the normal plan
CREATE TABLE ct_t (id serial, store int, ts timestamp, memo text) USING sift_table;
INSERT INTO ct_t (store, ts, memo) SELECT store, ts, memo FROM ct ORDER BY id;
CREATE INDEX ct_t_bm ON ct_t USING sift (memo);
SELECT ct_check('SELECT id, ts FROM ct_t WHERE memo @@> ''gift'' ORDER BY ts DESC, id DESC LIMIT 10') AS tbl;

DROP TABLE ct_q, ct_t, ct;
DROP FUNCTION ct_plan(text), ct_words(int), ct_mode(bool), ct_check(text, text), ct_stat(text, text), ct_stage(text);
DROP EXTENSION pg_sift;
