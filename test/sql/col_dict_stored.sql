-- The dictionary of a string leaf stored to be read in place: the strings in blocks, and with the option
-- dict_lookup the codes in byte order too, so that an equality, an IN list and a prefix or range test of a stored segment read it from the stream and
-- the backend parses none. The answers are those of the compact form (sift.col_format_minor 13), of the blocks without the order (dict_lookup off) and of the heap, over NULLs, empty, long (inline),
-- non-ASCII and tied strings, uuids, DML, the write buffer, promotion and a merge of segments of both forms.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;

CREATE TABLE ds (id int, s text, u uuid, v varchar(40), pad text DEFAULT repeat('x', 400));
ALTER TABLE ds ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ds (id, s, u, v)
SELECT i,
	   CASE WHEN i % 53 = 0 THEN NULL
			WHEN i % 47 = 0 THEN ''
			WHEN i BETWEEN 9000 AND 9050 THEN repeat('long', 100) || i % 9
			WHEN i % 41 = 0 THEN (ARRAY['é', 'éa', '日本', '日本語', 'ab', 'abc', 'abd', 'a', 'b', 'B', 'Ab', 'zz'])[1 + i % 12]
			WHEN i % 7 = 0 THEN 'commonprefix_' || (i * 7919) % 3000
			ELSE 'k' || md5((i % 2500)::text) END,
	   CASE WHEN i % 59 = 0 THEN NULL ELSE md5((i % 4000)::text)::uuid END,
	   CASE WHEN i % 61 = 0 THEN NULL ELSE 'v' || (i * 31) % 1700 END
FROM generate_series(1, 12000) i;

CREATE FUNCTION ds_cmp(idx regclass, preds text, heapwhere text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; ex bigint; mi bigint;
BEGIN
	SELECT count(*) INTO a FROM sift._test_col_tids(idx, preds);
	EXECUTE format('SELECT count(*) FROM ds WHERE %s', heapwhere) INTO b;
	EXECUTE format('SELECT count(*) FROM (SELECT tid FROM sift._test_col_tids(%L, %L) EXCEPT SELECT ctid FROM ds WHERE %s) z', idx, preds, heapwhere) INTO ex;
	EXECUTE format('SELECT count(*) FROM (SELECT ctid FROM ds WHERE %s EXCEPT SELECT tid FROM sift._test_col_tids(%L, %L)) z', heapwhere, idx, preds) INTO mi;
	RETURN CASE WHEN a = b AND ex = 0 AND mi = 0 THEN 'ok' ELSE format('MISMATCH index=%s heap=%s extra=%s missing=%s', a, b, ex, mi) END;
END $$;

-- leaf 0 is s, 1 is u, 2 is v
CREATE TABLE ds_q (n serial, preds text, heapwhere text);
INSERT INTO ds_q (preds, heapwhere) VALUES
 ('0:eq:commonprefix_17', 's = ''commonprefix_17'''),
 ('0:eq:kc4ca4238a0b923820dcc509a6f75849b', 's = ''kc4ca4238a0b923820dcc509a6f75849b'''),
 ('0:eq:nothing_here', 's = ''nothing_here'''),
 ('0:eq:', 's = '''''),
 ('0:eq:日本', 's = ''日本'''),
 ('0:eq:ab', 's = ''ab'''),
 ('0:ne:ab', 's <> ''ab'''),
 ('0:in:ab|abc|zz|none', 's IN (''ab'', ''abc'', ''zz'', ''none'')'),
 ('0:in:ab|ab|ab', 's IN (''ab'', ''ab'', ''ab'')'),
 ('0:notin:ab|abc|zz|none', 's NOT IN (''ab'', ''abc'', ''zz'', ''none'')'),
 ('0:prefix:commonprefix_1', 'starts_with(s, ''commonprefix_1'')'),
 ('0:prefix:', 'starts_with(s, '''')'),
 ('0:prefix:a', 'starts_with(s, ''a'')'),
 ('0:prefix:é', 'starts_with(s, ''é'')'),
 ('0:prefix:long', 'starts_with(s, ''long'')'),
 ('0:prefix:zzzz', 'starts_with(s, ''zzzz'')'),
 ('0:lt:ab', 's < ''ab'' COLLATE "C"'),
 ('0:ge:abc;0:lt:abd', 's >= ''abc'' COLLATE "C" AND s < ''abd'' COLLATE "C"'),
 ('0:gt:b', 's > ''b'' COLLATE "C"'),
 ('2:eq:v77', 'v = ''v77'''),
 ('2:prefix:v1', 'starts_with(v, ''v1'')'),
 ('2:in:v1|v2|v3|nope', 'v IN (''v1'', ''v2'', ''v3'', ''nope'')'),
 ('1:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b', 'u = ''c4ca4238-a0b9-2382-0dcc-509a6f75849b'''),
 ('1:eq:00000000-0000-0000-0000-000000000001', 'u = ''00000000-0000-0000-0000-000000000001'''),
 ('1:in:c4ca4238-a0b9-2382-0dcc-509a6f75849b|c81e728d-9d4c-2f63-6f06-7f89cc14862c', 'u IN (''c4ca4238-a0b9-2382-0dcc-509a6f75849b'', ''c81e728d-9d4c-2f63-6f06-7f89cc14862c'')'),
 ('0:eq:ab;1:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b', 's = ''ab'' AND u = ''c4ca4238-a0b9-2382-0dcc-509a6f75849b''');

-- the stored form: the segment is minor 14, the dictionaries are not parsed by an equality, an IN list or a prefix, and every answer is the heap's
CREATE INDEX ds_new ON ds USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', write_buffer_limit = 256, dict_lookup = on);
VACUUM (ANALYZE) ds;
SELECT sift.col_verify('ds_new', true);
SELECT count(*) AS segments, min(format) AS format, sum(units) > 2 AS units FROM sift.col_segments('ds_new') WHERE kind = 'immutable';
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT n, ds_cmp('ds_new', preds, heapwhere) FROM ds_q ORDER BY n;
SELECT loads = 0 AS no_dictionary_parsed FROM sift._test_col_dict_cache(false);

-- a dictionary that the backend tests again and again is parsed and cached (about 32 tests), and then answers from the cache
SELECT sum(rows) AS rows FROM generate_series(1, 40) g, LATERAL sift._test_col_counters('ds_new', '1:eq:c4ca4238-a0b9-2382-0dcc-509a6f75849b');
SELECT loads > 0 AS parsed, hits > 0 AS then_cached FROM sift._test_col_dict_cache(false);
SELECT n, ds_cmp('ds_new', preds, heapwhere) FROM ds_q ORDER BY n;

-- the option off (blocks for the dictionaries that pay, without the order) and the compact form (the setting 13): the same answers, the dictionaries are parsed
CREATE INDEX ds_off ON ds USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', write_buffer_limit = 256, dict_lookup = off);
SET sift.col_format_minor = 13;
CREATE INDEX ds_13 ON ds USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', write_buffer_limit = 256, dict_lookup = on);
RESET sift.col_format_minor;
SELECT index, min(format) AS format FROM (SELECT 'off' AS index, format FROM sift.col_segments('ds_off') WHERE kind = 'immutable'
	UNION ALL SELECT '13', format FROM sift.col_segments('ds_13') WHERE kind = 'immutable') x GROUP BY index ORDER BY index;
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT n, ds_cmp('ds_off', preds, heapwhere), ds_cmp('ds_13', preds, heapwhere) FROM ds_q ORDER BY n;
SELECT loads > 0 AS dictionaries_parsed FROM sift._test_col_dict_cache(false);
SELECT sift.col_verify('ds_off', true), sift.col_verify('ds_13', true);
DROP INDEX ds_off, ds_13;

-- a dictionary under the minimum is kept compact (the minimum is a limit)
CREATE TABLE dfew AS SELECT i AS id, 'few' || i % 300 AS s FROM generate_series(1, 6000) i;
CREATE INDEX dfew_i ON dfew USING sift_col (s sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', dict_lookup = on);
SELECT min(format) AS format FROM sift.col_segments('dfew_i') WHERE kind = 'immutable';
DROP TABLE dfew;
SELECT set_config('sift.test_limits', 'dict_lookup_min=1', false) IS NOT NULL AS lowered;
CREATE TABLE dt (id int, s text);
INSERT INTO dt SELECT i, CASE i % 4 WHEN 0 THEN NULL WHEN 1 THEN 'one' WHEN 2 THEN '' ELSE repeat('x', 255) END FROM generate_series(1, 400) i;
CREATE TABLE dt1 (id int, s text);
INSERT INTO dt1 SELECT i, 'same' FROM generate_series(1, 100) i;
CREATE INDEX dt_i ON dt USING sift_col (s sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', dict_lookup = on);
CREATE INDEX dt1_i ON dt1 USING sift_col (s sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', dict_lookup = on);
SELECT (SELECT min(format) FROM sift.col_segments('dt_i') WHERE kind = 'immutable') AS format,
	   (SELECT min(format) FROM sift.col_segments('dt1_i') WHERE kind = 'immutable') AS format_one_string;
SELECT (SELECT count(*) FROM sift._test_col_tids('dt_i', '0:eq:one')) AS one,
	   (SELECT count(*) FROM sift._test_col_tids('dt_i', '0:eq:')) AS empty,
	   (SELECT count(*) FROM sift._test_col_tids('dt_i', '0:prefix:x')) AS long,
	   (SELECT count(*) FROM sift._test_col_tids('dt_i', '0:gt:one')) AS above_one,
	   (SELECT count(*) FROM sift._test_col_tids('dt1_i', '0:eq:same')) AS same,
	   (SELECT count(*) FROM sift._test_col_tids('dt1_i', '0:eq:sam')) AS shorter,
	   (SELECT count(*) FROM sift._test_col_tids('dt1_i', '0:prefix:sa')) AS prefix;
SELECT sift.col_verify('dt_i', true), sift.col_verify('dt1_i', true);
RESET sift.test_limits;
DROP TABLE dt, dt1;

-- DML, the write buffer, promotion and a merge of segments of the compact and the stored form
UPDATE ds SET s = 'commonprefix_' || id, u = md5('new' || id)::uuid, v = 'v' || id WHERE id % 97 = 0;
DELETE FROM ds WHERE id % 89 = 0;
INSERT INTO ds (id, s, u, v) SELECT i, 'late' || i % 700, md5('late' || i)::uuid, 'w' || i % 300 FROM generate_series(12001, 13000) i;
SELECT n, ds_cmp('ds_new', preds, heapwhere) FROM ds_q ORDER BY n;
VACUUM ds;
SELECT n, ds_cmp('ds_new', preds, heapwhere) FROM ds_q ORDER BY n;
SET sift.col_format_minor = 13;
INSERT INTO ds (id, s, u, v) SELECT i, 'older' || i % 900, md5('older' || i)::uuid, 'x' || i % 400 FROM generate_series(13001, 14000) i;
VACUUM ds;
RESET sift.col_format_minor;
SELECT format, count(*) AS segments FROM sift.col_segments('ds_new') WHERE kind = 'immutable' GROUP BY format ORDER BY format;
SELECT n, ds_cmp('ds_new', preds, heapwhere) FROM ds_q ORDER BY n;
SELECT sift.col_merge('ds_new'::regclass, true) >= 0 AS merged;
SELECT format, count(*) AS segments FROM sift.col_segments('ds_new') WHERE kind = 'immutable' GROUP BY format ORDER BY format;
SELECT sift.col_verify('ds_new', true);
SELECT n, ds_cmp('ds_new', preds, heapwhere) FROM ds_q ORDER BY n;

-- a cache that holds nothing: the same answers
SELECT set_config('sift.test_limits', 'dict_cache_kb=1', false) IS NOT NULL AS sized;
SELECT n, ds_cmp('ds_new', preds, heapwhere) FROM ds_q ORDER BY n;
RESET sift.test_limits;

-- ALTER INDEX: the option applies to the segments written afterwards
SELECT sum(bytes) AS with_order FROM sift.col_sections('ds_new') WHERE section = 'dict' \gset
ALTER INDEX ds_new SET (dict_lookup = off);
REINDEX INDEX ds_new;
SELECT sum(bytes) < :with_order AS without_order FROM sift.col_sections('ds_new') WHERE section = 'dict';
ALTER INDEX ds_new SET (dict_lookup = on);
REINDEX INDEX ds_new;
SELECT min(format) AS format FROM sift.col_segments('ds_new') WHERE kind = 'immutable';
SELECT n, ds_cmp('ds_new', preds, heapwhere) FROM ds_q ORDER BY n;

-- the default (auto) stores the order of the strings of a leaf that a lookup structure reads (set_columns, point_columns, return_columns) and of no other; on stores every one, off none
SET max_parallel_maintenance_workers = 0;
CREATE INDEX ds_on ON ds USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', dict_lookup = on);
CREATE INDEX ds_off2 ON ds USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', dict_lookup = off);
CREATE INDEX ds_auto ON ds USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off');
CREATE INDEX ds_auto_set ON ds USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', key_columns = 's');
CREATE INDEX ds_auto_point ON ds USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', point_columns = 'u');
CREATE INDEX ds_auto_ret ON ds USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', return_columns = 's, v');
CREATE INDEX ds_auto_off ON ds USING sift_col (s sift.col_text_ops, u sift.col_uuid_ops, v sift.col_text_ops, id sift.col_int4_ops) WITH (count_columns = 'off', point_columns = 's, u', dict_lookup = off);
RESET max_parallel_maintenance_workers;
SELECT i.idx, d.leaf, d.bytes = (SELECT x.bytes FROM sift.col_sections('ds_on') x WHERE x.section = 'dict' AND x.leaf = d.leaf) AS with_order
FROM (VALUES ('ds_on'), ('ds_off2'), ('ds_auto'), ('ds_auto_set'), ('ds_auto_point'), ('ds_auto_ret'), ('ds_auto_off')) i(idx), LATERAL sift.col_sections(i.idx::regclass) d
WHERE d.section = 'dict' ORDER BY i.idx, d.leaf;
SELECT sift.col_verify('ds_auto_set', true), sift.col_verify('ds_auto_point', true), sift.col_verify('ds_auto_ret', true);
CREATE INDEX ds_bad ON ds USING sift_col (s sift.col_text_ops, id sift.col_int4_ops) WITH (dict_lookup = sometimes);
CREATE INDEX ds_bool ON ds USING sift_col (s sift.col_text_ops, id sift.col_int4_ops) WITH (dict_lookup = true);
DROP INDEX ds_bool;

-- the default stores a dictionary in blocks (read in place) only where that adds little to the segment: uuids and hashes compress as well in blocks as whole,
-- short strings that repeat their words (titles) do not and keep the compact form; the answers are the same
CREATE TABLE dw AS SELECT i AS id, md5((i % 5000)::text)::uuid AS u,
	(SELECT string_agg(substr(md5((1 + (hashint4((i % 5000) * 31 + j) & 2147483647) % 400)::text), 1, 3 + j % 5), ' ') FROM generate_series(1, 5) j) AS w
FROM generate_series(1, 20000) i;
-- (dict_lookup = 'off': the blocks alone, the hash they carry by default is 5 bytes a string more: col_dict_blocks_hash)
CREATE INDEX dw_i ON dw USING sift_col (id sift.col_int4_ops, u sift.col_uuid_ops, w sift.col_text_ops) WITH (count_columns = 'off', dict_lookup = 'off');
SET sift.col_format_minor = 13;
CREATE INDEX dw_13 ON dw USING sift_col (id sift.col_int4_ops, u sift.col_uuid_ops, w sift.col_text_ops) WITH (count_columns = 'off');
RESET sift.col_format_minor;
SELECT d.leaf, d.bytes <= x.bytes * 1.03 AS within_3_percent, d.bytes > x.bytes AS blocks
FROM sift.col_sections('dw_i') d JOIN sift.col_sections('dw_13') x ON x.section = d.section AND x.leaf = d.leaf WHERE d.section = 'dict' ORDER BY d.leaf;
SELECT sift.col_verify('dw_i', true), sift.col_verify('dw_13', true);
SELECT (SELECT count(*) FROM sift._test_col_tids('dw_i', '2:eq:' || (SELECT w FROM dw WHERE id = 77))) = (SELECT count(*) FROM dw WHERE w = (SELECT w FROM dw WHERE id = 77)) AS same_answer;
DROP TABLE dw;

DROP TABLE ds CASCADE;
DROP FUNCTION ds_cmp(regclass, text, text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
DROP TABLE ds_q;
