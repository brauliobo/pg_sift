-- ORDER BY a text (under C), uuid or bytea column LIMIT n over dictionaries of thousands of entries and several segments: a uuid or bytea key is read as the
-- codes of the dictionary the segment stores, and the rows of a unit are compared with the bar by the order code of that dictionary (its first 8 bytes, or its
-- position in byte order once it is sorted). Every query runs twice, the plain plan as the reference (sift.enable_agg off) and the pushed one, and the rows are
-- compared in order, after the build, after promotions that make more segments, with rows in the write buffer, after updates and deletes, a merge and VACUUM.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;

-- pad makes a unit hold about 1300 rows, so that a segment has several units and the bar of one is a string of another
CREATE TABLE oc (id int PRIMARY KEY, g int, u uuid, b bytea, w bytea, s text, sp text, pad text DEFAULT repeat('x', 1500));
-- b: entries that share their first 8 bytes, empty and one-byte ones; w: five values in all the rows; s: distinct strings; sp: strings that share their first 8 bytes
CREATE FUNCTION oc_rows(lo int, hi int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO oc (id, g, u, b, w, s, sp)
	SELECT i, i % 7,
		CASE WHEN i % 17 = 0 THEN NULL ELSE md5('u' || i)::uuid END,
		CASE WHEN i % 13 = 0 THEN NULL WHEN i % 29 = 0 THEN '\x'::bytea WHEN i % 31 = 0 THEN '\x00'::bytea
			 WHEN i % 3 = 0 THEN '\x0102030405060708'::bytea || decode(md5('b' || i), 'hex') ELSE decode(md5('b' || i), 'hex') END,
		decode(md5((i % 5)::text), 'hex'),
		CASE WHEN i % 19 = 0 THEN NULL WHEN i % 23 = 0 THEN '' ELSE md5('s' || i) END,
		CASE WHEN i % 11 = 0 THEN NULL ELSE 'prefix_c' || md5('p' || i) END
	FROM generate_series(lo, hi) i $$;
SELECT oc_rows(1, 6000);
CREATE INDEX oc_col ON oc USING sift_col (id, g, u, b, w, s, sp);
VACUUM (ANALYZE) oc;

CREATE FUNCTION oc_mode(pushed bool, workers int) RETURNS void LANGUAGE sql AS $$ SELECT sift_helpers.agg_mode_workers(pushed, workers) $$;

-- 'same' / 'DIFFERENT' (the rows and their order), '+topn' when the pushed plan is the top-N scan, '+stored' when it reads the first key as the stored dictionary, and the rows
CREATE FUNCTION oc_check(q text, workers int DEFAULT 0) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; plan text := ''; line text;
BEGIN
	DROP TABLE IF EXISTS oc_a, oc_b;
	PERFORM oc_mode(true, workers);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	EXECUTE 'CREATE TEMP TABLE oc_a AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	PERFORM oc_mode(false, 0);
	EXECUTE 'CREATE TEMP TABLE oc_b AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	SELECT count(*) INTO n FROM ((SELECT pos, r FROM oc_a EXCEPT ALL SELECT pos, r FROM oc_b) UNION ALL (SELECT pos, r FROM oc_b EXCEPT ALL SELECT pos, r FROM oc_a)) d;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END || CASE WHEN plan ~ 'Top-N: ' THEN '+topn' ELSE '' END || CASE WHEN plan ~ 'stored dictionary' THEN '+stored' ELSE '' END || ' ' || (SELECT count(*) FROM oc_b);
END $$;

-- the order codes the units of a pushed query were compared by: bytes (the rows seen so far are too few to make a code), prefix, exact (the dictionary is in byte order)
CREATE FUNCTION oc_path(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text; m text[];
BEGIN
	PERFORM oc_mode(true, 0);
	FOR line IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		m := regexp_match(line, 'Top-N Units By Order Code: exact (\d+), prefix (\d+), bytes (\d+)');
		IF m IS NOT NULL THEN
			RETURN concat_ws('+', CASE WHEN m[1]::int > 0 THEN 'exact' END, CASE WHEN m[2]::int > 0 THEN 'prefix' END, CASE WHEN m[3]::int > 0 THEN 'bytes' END);
		END IF;
	END LOOP;
	RETURN 'none';
END $$;

CREATE TABLE oc_q (tag text PRIMARY KEY, q text NOT NULL);
INSERT INTO oc_q VALUES
	('uuid_asc',         $q$SELECT id, u FROM oc ORDER BY u, id LIMIT 25$q$),
	('uuid_desc',        $q$SELECT id, u FROM oc ORDER BY u DESC, id LIMIT 25$q$),
	('uuid_nulls_first', $q$SELECT id, u FROM oc ORDER BY u NULLS FIRST, id LIMIT 25$q$),
	('uuid_nulls_last',  $q$SELECT id, u FROM oc ORDER BY u DESC NULLS LAST, id DESC LIMIT 25$q$),
	('uuid_offset',      $q$SELECT id, u FROM oc ORDER BY u, id LIMIT 20 OFFSET 900$q$),
	('uuid_filter',      $q$SELECT id, u FROM oc WHERE g IN (1, 3) AND id > 2000 ORDER BY u DESC, id LIMIT 30$q$),
	('uuid_ties',        $q$SELECT id, g, u FROM oc ORDER BY g, u, id LIMIT 40 OFFSET 11$q$),
	('bytea_asc',        $q$SELECT id, b FROM oc ORDER BY b, id LIMIT 25$q$),
	('bytea_desc',       $q$SELECT id, b FROM oc ORDER BY b DESC NULLS LAST, id LIMIT 25$q$),
	('bytea_nulls_first',$q$SELECT id, b FROM oc ORDER BY b NULLS FIRST, id LIMIT 25$q$),
	('bytea_offset',     $q$SELECT id, b FROM oc ORDER BY b DESC, id LIMIT 20 OFFSET 1500$q$),
	('bytea_filter',     $q$SELECT id, b FROM oc WHERE g = 2 ORDER BY b, id LIMIT 30$q$),
	('bytea_few_values', $q$SELECT id, w FROM oc ORDER BY w, id LIMIT 30$q$),
	('bytea_few_desc',   $q$SELECT id, w FROM oc ORDER BY w DESC, id DESC LIMIT 30$q$),
	-- the key leaf is read by something else too: the datum dictionary stays
	('uuid_same_leaf',   $q$SELECT id, u FROM oc WHERE u > '80000000-0000-0000-0000-000000000000' ORDER BY u, id LIMIT 25$q$),
	('bytea_same_leaf',  $q$SELECT id, b FROM oc WHERE b <> '\x00' AND length(b) > 4 ORDER BY b DESC, id LIMIT 25$q$),
	('uuid_expression',  $q$SELECT id, u FROM oc ORDER BY u::text, id LIMIT 25$q$),
	('text_asc',         $q$SELECT id, s FROM oc ORDER BY s COLLATE "C", id LIMIT 25$q$),
	('text_desc',        $q$SELECT id, s FROM oc ORDER BY s COLLATE "C" DESC, id LIMIT 25$q$),
	('text_nulls_first', $q$SELECT id, s FROM oc ORDER BY s COLLATE "C" NULLS FIRST, id LIMIT 25$q$),
	('text_nulls_last',  $q$SELECT id, s FROM oc ORDER BY s COLLATE "C" DESC NULLS LAST, id DESC LIMIT 25$q$),
	('text_offset',      $q$SELECT id, s FROM oc ORDER BY s COLLATE "C", id LIMIT 20 OFFSET 1100$q$),
	('text_filter',      $q$SELECT id, s FROM oc WHERE g IN (2, 5) AND id > 3000 ORDER BY s COLLATE "C" DESC, id LIMIT 30$q$),
	('text_same_prefix', $q$SELECT id, sp FROM oc ORDER BY sp COLLATE "C", id LIMIT 40$q$),
	('text_same_prefix_desc', $q$SELECT id, sp FROM oc ORDER BY sp COLLATE "C" DESC NULLS LAST, id LIMIT 40 OFFSET 7$q$),
	('text_few_values',  $q$SELECT id, g, s FROM oc WHERE g = 3 ORDER BY g, s COLLATE "C", id LIMIT 40$q$),
	('text_locale',      $q$SELECT id, s FROM oc ORDER BY s COLLATE "en-x-icu", id LIMIT 25$q$),
	('two_keys',         $q$SELECT id, u, b FROM oc ORDER BY u DESC, b, id LIMIT 30$q$),
	('text_then_uuid',   $q$SELECT id, s, u FROM oc ORDER BY s COLLATE "C", u DESC, id LIMIT 30$q$),
	('second_key',       $q$SELECT id, g, u FROM oc ORDER BY g DESC, u, id LIMIT 30$q$),
	('all_rows',         $q$SELECT id, u FROM oc ORDER BY u, id LIMIT 200000$q$);

CREATE FUNCTION oc_stage(stage text, workers int DEFAULT 0) RETURNS TABLE (stage text, tag text, result text) LANGUAGE sql AS $$
	SELECT stage, tag, oc_check(q, workers) FROM oc_q ORDER BY tag COLLATE "C" $$;

SELECT * FROM oc_stage('build');
SELECT tag, oc_path(q) FROM oc_q WHERE tag IN ('uuid_asc', 'bytea_few_values', 'text_asc', 'text_same_prefix', 'text_filter', 'text_locale', 'uuid_expression') ORDER BY tag COLLATE "C";
-- workers: each keeps a heap of the strings it saw and sends them with their bytes; the leader merges them
SELECT tag, oc_check(q, 2) FROM oc_q WHERE tag IN ('uuid_asc', 'uuid_desc', 'bytea_asc', 'bytea_few_desc', 'two_keys', 'text_asc', 'text_desc', 'text_same_prefix') ORDER BY tag COLLATE "C";

-- more segments: each promotion turns the rows of the write buffer into a segment of its own, with a dictionary of its own
SELECT oc_rows(6001, 9000);
SELECT sift.col_promote('oc_col') >= 0 AS promoted;
SELECT oc_rows(9001, 12000);
SELECT sift.col_promote('oc_col') >= 0 AS promoted;
SELECT * FROM oc_stage('segments');
SELECT tag, oc_check(q, 2) FROM oc_q WHERE tag IN ('uuid_asc', 'bytea_desc', 'bytea_filter', 'text_asc', 'text_same_prefix_desc') ORDER BY tag COLLATE "C";

-- rows of the write buffer and dirty units
SELECT oc_rows(12001, 12400);
UPDATE oc SET u = md5('new' || id)::uuid, b = NULL, s = 'Aaa' || id WHERE id % 97 = 0;
DELETE FROM oc WHERE id % 89 = 0;
SELECT * FROM oc_stage('buffer');
SELECT tag, oc_check(q, 2) FROM oc_q WHERE tag IN ('uuid_asc', 'bytea_asc', 'text_desc') ORDER BY tag COLLATE "C";

-- a heap of keys wider than work_mem sorts instead, and the rows past work_mem spill to a tuplesort
SET work_mem = '64kB';
SELECT oc_check($q$SELECT id, u, b FROM oc ORDER BY u, b DESC, id LIMIT 30 OFFSET 9000$q$) AS spilled_two_keys,
	   oc_check($q$SELECT id, s FROM oc ORDER BY s COLLATE "C" DESC, id LIMIT 3000$q$) AS spilled_text,
	   oc_check($q$SELECT id, b FROM oc ORDER BY b, id LIMIT 4000$q$) AS wide_heap;
RESET work_mem;

-- a dictionary cache that keeps almost nothing: the dictionaries of the leaves of a scan are parsed again between its units, the answers do not change
SELECT set_config('sift.test_limits', 'dict_cache_kb=1', false) IS NOT NULL AS tiny_cache;
SELECT tag, oc_check(q) FROM oc_q WHERE tag IN ('uuid_asc', 'uuid_filter', 'bytea_desc', 'text_asc', 'text_desc', 'text_same_prefix', 'two_keys', 'text_then_uuid') ORDER BY tag COLLATE "C";
SELECT tag, oc_check(q) FROM oc_q WHERE tag IN ('uuid_asc', 'text_asc', 'text_same_prefix') ORDER BY tag COLLATE "C";
RESET sift.test_limits;

-- a dictionary above the limit of entries an order code is made for (lowered here): the rows are compared as bytes
SELECT set_config('sift.test_limits', 'topn_code_entries=50', false) IS NOT NULL AS low_limit;
SELECT tag, oc_check(q) FROM oc_q WHERE tag IN ('uuid_asc', 'bytea_desc', 'text_asc', 'text_same_prefix', 'two_keys') ORDER BY tag COLLATE "C";
SELECT oc_path($q$SELECT id, s FROM oc ORDER BY s COLLATE "C", id LIMIT 10$q$) AS above_the_limit;
RESET sift.test_limits;

SELECT sift.col_merge('oc_col', true) >= 0 AS merged;
SELECT * FROM oc_stage('merged');
VACUUM oc;
SELECT * FROM oc_stage('vacuumed');
SELECT sift.col_verify('oc_col', true);

-- bytea of every length from 0 to 40, scanned again and again: the order of the entries as stored (their length first) is not byteacmp's, so a bytea key has the prefix code only
CREATE TABLE ob (id int PRIMARY KEY, b bytea, pad text DEFAULT repeat('x', 1500));
INSERT INTO ob (id, b) SELECT i, decode(substr(md5(i::text) || md5((i * 7)::text), 1, 2 * (i % 41)), 'hex') FROM generate_series(1, 4000) i;
CREATE INDEX ob_col ON ob USING sift_col (id, b);
VACUUM (ANALYZE) ob;
SELECT r, count(*) AS runs FROM (
	SELECT oc_check($q$SELECT id, b FROM ob ORDER BY b, id LIMIT 900$q$) AS r FROM generate_series(1, 8)) x GROUP BY r;
SELECT r, count(*) AS runs FROM (
	SELECT oc_check($q$SELECT id, b FROM ob ORDER BY b DESC, id LIMIT 900 OFFSET 1500$q$) AS r FROM generate_series(1, 8)) x GROUP BY r;
SELECT oc_path($q$SELECT id, b FROM ob ORDER BY b, id LIMIT 10$q$) AS bytea_path;

-- a dictionary that stores its codes in byte order (dict_lookup on): the order code is exact from the first unit
CREATE TABLE ox (id int PRIMARY KEY, g int, u uuid, s text, sp text, pad text DEFAULT repeat('x', 1500));
INSERT INTO ox (id, g, u, s, sp)
SELECT i, i % 7, CASE WHEN i % 17 = 0 THEN NULL ELSE md5('u' || i)::uuid END, CASE WHEN i % 19 = 0 THEN NULL ELSE md5('s' || i) END,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE 'prefix_c' || md5('p' || i) END
FROM generate_series(1, 6000) i;
CREATE INDEX ox_col ON ox USING sift_col (id, g, u, s, sp) WITH (dict_lookup = on);
VACUUM (ANALYZE) ox;
SELECT tag, oc_path(replace(q, 'FROM oc ', 'FROM ox ')) FROM oc_q WHERE tag IN ('uuid_asc', 'text_asc', 'text_same_prefix') ORDER BY tag COLLATE "C";
SELECT tag, oc_check(replace(q, 'FROM oc ', 'FROM ox ')) FROM oc_q WHERE tag IN ('uuid_asc', 'uuid_desc', 'uuid_filter', 'text_asc', 'text_desc', 'text_nulls_first', 'text_offset', 'text_filter', 'text_same_prefix', 'text_same_prefix_desc', 'text_then_uuid') ORDER BY tag COLLATE "C";
INSERT INTO ox (id, g, u, s, sp) SELECT i, i % 7, md5('n' || i)::uuid, 'Zz' || md5('n' || i), 'prefix_c' || i FROM generate_series(6001, 6800) i;
SELECT sift.col_promote('ox_col') >= 0 AS promoted;
INSERT INTO ox (id, g, u, s, sp) SELECT i, i % 7, md5('m' || i)::uuid, 'Zy' || md5('m' || i), NULL FROM generate_series(6801, 7000) i;
SELECT tag, oc_check(replace(q, 'FROM oc ', 'FROM ox ')) FROM oc_q WHERE tag IN ('uuid_asc', 'text_desc', 'text_same_prefix') ORDER BY tag COLLATE "C";

-- a sift_table without the stored order (sift.table_dict_lookup off): its dictionaries get the prefix code
SET sift.table_dict_lookup = off;
CREATE TABLE ot (id int PRIMARY KEY, g int, u uuid, s text, sp text, pad text DEFAULT repeat('x', 1500)) USING sift_table;
INSERT INTO ot (id, g, u, s, sp)
SELECT i, i % 7, CASE WHEN i % 17 = 0 THEN NULL ELSE md5('u' || i)::uuid END, CASE WHEN i % 19 = 0 THEN NULL ELSE md5('s' || i) END,
	   CASE WHEN i % 11 = 0 THEN NULL ELSE 'prefix_c' || md5('p' || i) END
FROM generate_series(1, 6000) i;
VACUUM (ANALYZE) ot;
SELECT tag, oc_path(replace(q, 'FROM oc ', 'FROM ot ')) FROM oc_q WHERE tag IN ('uuid_asc', 'text_asc', 'text_same_prefix') ORDER BY tag COLLATE "C";
SELECT tag, oc_check(replace(q, 'FROM oc ', 'FROM ot ')) FROM oc_q WHERE tag IN ('uuid_asc', 'uuid_desc', 'uuid_filter', 'text_asc', 'text_desc', 'text_nulls_first', 'text_offset', 'text_filter', 'text_same_prefix', 'text_same_prefix_desc', 'text_then_uuid') ORDER BY tag COLLATE "C";
INSERT INTO ot (id, g, u, s, sp) SELECT i, i % 7, md5('n' || i)::uuid, 'Zz' || md5('n' || i), 'prefix_c' || i FROM generate_series(6001, 6300) i;
DELETE FROM ot WHERE id % 61 = 0;
SELECT tag, oc_check(replace(q, 'FROM oc ', 'FROM ot ')) FROM oc_q WHERE tag IN ('uuid_asc', 'text_desc', 'text_same_prefix') ORDER BY tag COLLATE "C";
RESET sift.table_dict_lookup;

-- by default the order of a dictionary that takes a quarter of the segment at most is stored: the units are ordered by exact codes
CREATE TABLE otd (id int PRIMARY KEY, g int, u uuid, s text, sp text, pad text DEFAULT repeat('x', 1500)) USING sift_table;
INSERT INTO otd (id, g, u, s, sp) SELECT id, g, u, s, sp FROM ot;
VACUUM (ANALYZE) otd;
SELECT tag, oc_path(replace(q, 'FROM oc ', 'FROM otd ')) FROM oc_q WHERE tag IN ('uuid_asc', 'text_asc', 'text_same_prefix') ORDER BY tag COLLATE "C";
SELECT tag, oc_check(replace(q, 'FROM oc ', 'FROM otd ')) FROM oc_q WHERE tag IN ('uuid_asc', 'text_desc', 'text_same_prefix') ORDER BY tag COLLATE "C";

-- values the leaf could not store (the byte budget of a unit lowered to 64 kB for 3000 rows of 200 bytes): the unit keeps them as overflow, and the query stays with the normal plan or reads them from the heap
CREATE TABLE ov (id int PRIMARY KEY, b bytea);
INSERT INTO ov SELECT i, decode(repeat(md5(i::text), 6) || md5((i % 40)::text), 'hex') FROM generate_series(1, 3000) i;
SET sift.test_limits = 'unit_bytes=65536';
CREATE INDEX ov_col ON ov USING sift_col (id, b);
RESET sift.test_limits;
ANALYZE ov;
SELECT oc_check($q$SELECT id, b FROM ov ORDER BY b DESC, id LIMIT 10$q$) AS overflow_key;

DROP FUNCTION oc_check, oc_mode, oc_path, oc_rows, oc_stage;
DROP TABLE oc, oc_q, ov, ox, ot, otd, ob;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
