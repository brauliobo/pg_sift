-- Index-only scans that return text, varchar and uuid columns (docs/COL-SCAN.md): the option return_columns names the columns an index gives back; a
-- string is read out of the dictionary of its segment one at a time, without parsing the dictionary, when a few rows are read, whether the
-- dictionary is stored for a lookup (column format minor 14), plain, or compact (then it is parsed). The rows must be the heap's: NULLs, empty,
-- long (stored inline) and non-ASCII strings, a write buffer, updates, deletes, promotion and a merge.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;

CREATE TABLE rs (id int, s text, u uuid, vc varchar(30), c char(6), pad text DEFAULT repeat('x', 500));
ALTER TABLE rs ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO rs (id, s, u, vc, c)
SELECT i,
	   CASE WHEN i % 31 = 0 THEN NULL WHEN i % 37 = 0 THEN '' WHEN i % 41 = 0 THEN 'caf' || chr(233) || ' ' || i WHEN i % 43 = 0 AND i > 11000 THEN repeat('long', 100) || i
			ELSE 'str ' || i END,
	   CASE WHEN i % 47 = 0 THEN NULL ELSE md5(i::text)::uuid END,
	   CASE WHEN i % 53 = 0 THEN NULL WHEN i % 59 = 0 THEN '' ELSE 'v' || i % 7000 END,
	   'c' || i % 90
FROM generate_series(1, 12000) i;

-- the option: columns of other types and unknown names are refused
CREATE INDEX rs_bad1 ON rs USING sift_col (id sift.col_int4_ops) WITH (return_columns = 'id');
CREATE INDEX rs_bad2 ON rs USING sift_col (id sift.col_int4_ops) WITH (return_columns = 'nope');
CREATE INDEX rs_bad3 ON rs USING sift_col (id sift.col_int4_ops, c sift.col_bpchar_ops) WITH (return_columns = 'c');
CREATE INDEX rs_bad4 ON rs USING sift_col (id sift.col_int4_ops, s sift.col_text_ops) WITH (return_columns = 's,');
CREATE INDEX rs_i ON rs USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, u sift.col_uuid_ops, vc sift.col_text_ops, c sift.col_bpchar_ops)
	WITH (return_columns = 's, u, vc', write_buffer_limit = 256);
VACUUM (ANALYZE) rs;
SELECT min(format) AS format FROM sift.col_segments('rs_i'::regclass) WHERE kind = 'immutable';

-- the rows of a query read from the heap and by an index-only scan of the column index: same rows, and the plan is the scan
CREATE FUNCTION rs_mode(ios bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('sift.enable_col_scan', ios::text, true);
	PERFORM set_config('enable_seqscan', (NOT ios)::text, true);
	PERFORM set_config('enable_indexscan', ios::text, true);
	PERFORM set_config('enable_indexonlyscan', ios::text, true);
	PERFORM set_config('enable_bitmapscan', 'false', true);
END $$;

CREATE FUNCTION rs_run(q text, ios bool, OUT n bigint, OUT md5 text, OUT scan text) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM rs_mode(ios);
	scan := '';
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line ~ 'Index Only Scan using r[so]_[a-z]' THEN scan := 'index-only'; END IF;
		IF line ~ 'Index Scan using r[so]_[a-z]' THEN scan := 'index'; END IF;
	END LOOP;
	EXECUTE 'SELECT count(*), md5(string_agg(r, ''|'' ORDER BY r)) FROM (SELECT row_to_json(x)::text AS r FROM (' || q || ') x) y' INTO n, md5;
END $$;

CREATE FUNCTION rs_check(q text, OUT rows bigint, OUT agree bool, OUT scan text) LANGUAGE plpgsql AS $$
DECLARE a record; h record;
BEGIN
	SELECT * INTO a FROM rs_run(q, true);
	SELECT * INTO h FROM rs_run(q, false);
	rows := h.n;
	agree := a.n = h.n AND a.md5 IS NOT DISTINCT FROM h.md5;
	scan := a.scan;
END $$;

CREATE TABLE rs_q (n serial, q text);
INSERT INTO rs_q (q) VALUES
 ('SELECT id, s, u, vc FROM rs WHERE id = 5'),
 ('SELECT s FROM rs WHERE id = 43'),
 ('SELECT id, s, u, vc FROM rs WHERE id IN (1, 31, 37, 41, 43, 47, 53, 59, 4000, 11999)'),
 ('SELECT id, s, u, vc FROM rs WHERE id BETWEEN 100 AND 130'),
 ('SELECT id, s, u, vc FROM rs WHERE id < 200'),
 ('SELECT id, u FROM rs WHERE id > 11900'),
 ('SELECT id, s, u, vc FROM rs WHERE id >= 0'),
 ('SELECT id, s FROM rs WHERE s = ''str 77'''),
 ('SELECT id, u FROM rs WHERE u = md5(''9'')::uuid'),
 ('SELECT id, vc FROM rs WHERE vc LIKE ''v12%'''),
 ('SELECT id, s, vc FROM rs WHERE s IS NULL OR vc IS NULL'),
 ('SELECT id, s FROM rs WHERE s = ''''');

-- a few rows are read without the dictionary of their segment; many rows load it (the backend starts with an empty cache)
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT rows, agree FROM rs_check('SELECT id, s, u, vc FROM rs WHERE id = 5');
SELECT loads = 0 AS no_dictionary_loaded FROM sift._test_col_dict_cache(false);
SELECT rows, agree FROM rs_check('SELECT id, s, u, vc FROM rs WHERE id >= 0');
SELECT loads > 0 AS dictionary_loaded FROM sift._test_col_dict_cache(false);

-- the dictionary is at hand now, and the answers are the heap's
SELECT n, (rs_check(q)).* FROM rs_q ORDER BY n;

-- the write buffer, new versions and deleted rows
UPDATE rs SET s = 's ' || id, u = md5('u' || id)::uuid, vc = NULL WHERE id % 500 = 7;
UPDATE rs SET s = '' , vc = 'w' || id WHERE id % 500 = 8;
DELETE FROM rs WHERE id % 500 = 9;
INSERT INTO rs (id, s, u, vc, c) SELECT i, 'new ' || i, md5('n' || i)::uuid, 'n' || i, 'c' FROM generate_series(12001, 12300) i;
SELECT count(*) > 0 AS buffered FROM sift.col_segments('rs_i'::regclass) WHERE kind <> 'immutable';
SELECT n, (rs_check(q)).* FROM rs_q ORDER BY n;
SELECT (rs_check('SELECT id, s, u, vc FROM rs WHERE id % 500 IN (7, 8, 9) OR id > 12000')).*;

-- promotion and a merge
VACUUM rs;
SELECT n, (rs_check(q)).* FROM rs_q ORDER BY n;
INSERT INTO rs (id, s, u, vc, c) SELECT i, 'late ' || i, md5('l' || i)::uuid, 'l' || i, 'c' FROM generate_series(12301, 16000) i;
VACUUM rs;
SELECT sift.col_merge('rs_i'::regclass) >= 0 AS merged;
SELECT sift.col_verify('rs_i', true);
SELECT n, (rs_check(q)).* FROM rs_q ORDER BY n;

-- a cache that holds nothing keeps the answers
SELECT set_config('sift.test_limits', 'dict_cache_kb=1', false) IS NOT NULL AS sized;
SELECT n, (rs_check(q)).* FROM rs_q ORDER BY n;
RESET sift.test_limits;

-- the dictionary stored plain (no compression, no lookup form) and compact (parsed whole): the same answers
DROP INDEX rs_i;
CREATE INDEX rs_p ON rs USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, u sift.col_uuid_ops, vc sift.col_text_ops, c sift.col_bpchar_ops)
	WITH (return_columns = 's, u, vc', write_buffer_limit = 256, dict_lookup = off, compression = 'none');
SELECT min(format) < 14 AS not_stored_for_lookup FROM sift.col_segments('rs_p'::regclass) WHERE kind = 'immutable';
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT rows, agree, scan FROM rs_check('SELECT id, s, u, vc FROM rs WHERE id = 5');
SELECT loads = 0 AS no_dictionary_loaded FROM sift._test_col_dict_cache(false);
SELECT n, (rs_check(q)).* FROM rs_q ORDER BY n;
DROP INDEX rs_p;
CREATE INDEX rs_c ON rs USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, u sift.col_uuid_ops, vc sift.col_text_ops, c sift.col_bpchar_ops)
	WITH (return_columns = 's, u, vc', write_buffer_limit = 256, dict_lookup = off);
SELECT n, (rs_check(q)).* FROM rs_q ORDER BY n;
DROP INDEX rs_c;
CREATE INDEX rs_i ON rs USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, u sift.col_uuid_ops, vc sift.col_text_ops, c sift.col_bpchar_ops)
	WITH (return_columns = 's, u, vc', write_buffer_limit = 256);

-- strings the index could not keep (the strings of a leaf in a unit passed unit_bytes: the value is overflow and comes from the heap; conditions on the id only,
-- as a row with an overflow value is a candidate of every test of its leaf), and an expression
SELECT set_config('sift.test_limits', 'unit_bytes=3000', false) IS NOT NULL AS lowered;
SET client_min_messages = warning;
CREATE TABLE ro (id int, s text, u uuid, pad text DEFAULT repeat('x', 300));
ALTER TABLE ro ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ro SELECT i, CASE WHEN i % 29 = 0 THEN NULL ELSE 'o' || i || repeat('z', 20) END, CASE WHEN i % 31 = 0 THEN NULL ELSE md5(i::text)::uuid END FROM generate_series(1, 3000) i;
CREATE INDEX ro_i ON ro USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, u sift.col_uuid_ops) WITH (return_columns = 's, u', write_buffer_limit = 256);
CREATE INDEX ro_x ON ro USING sift_col (id sift.col_int4_ops, lower(s) sift.col_text_ops) WITH (return_columns = 'lower');
DROP INDEX ro_i;
CREATE INDEX ro_i ON ro USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, u sift.col_uuid_ops) WITH (return_columns = 's, u', write_buffer_limit = 256);
VACUUM (ANALYZE) ro;
UPDATE ro SET s = s || 'u', u = md5('x' || id)::uuid WHERE id % 10 = 0;
CREATE TABLE rs_q2 (n serial, q text);
INSERT INTO rs_q2 (q) VALUES
 ('SELECT id, s, u FROM ro WHERE id = 2999'),
 ('SELECT id, s, u FROM ro WHERE id BETWEEN 1 AND 3000'),
 ('SELECT id, s FROM ro WHERE id BETWEEN 2000 AND 2100'),
 ('SELECT id, u FROM ro WHERE id % 10 = 0 AND id > 2900');
SELECT n, (rs_check(q)).* FROM rs_q2 ORDER BY n;
RESET sift.test_limits;
RESET client_min_messages;
DROP TABLE ro;

-- without the option the strings are not returnable: the query reads the heap
ALTER INDEX rs_i RESET (return_columns);
SELECT scan FROM rs_check('SELECT id, s FROM rs WHERE id = 5');
SELECT scan, agree FROM rs_check('SELECT id FROM rs WHERE id = 5');
ALTER INDEX rs_i SET (return_columns = 'u');
SELECT scan, agree FROM rs_check('SELECT id, u FROM rs WHERE id < 50');
SELECT scan FROM rs_check('SELECT id, s FROM rs WHERE id < 50');

DROP TABLE rs CASCADE;
DROP FUNCTION rs_mode(bool), rs_run(text, bool), rs_check(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
