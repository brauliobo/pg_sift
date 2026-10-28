-- The dictionary of a text leaf stored in blocks also holds a trigram signature of each block (column format minor 29): a substring test of three bytes or more reads
-- only the blocks whose signature has the bit of every trigram, and the rows are the normal plan's for every needle, through promotion, merge, delete and REINDEX.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
SELECT set_config('sift.test_limits', 'dict_lookup_min=1,dict_tsig_min=1,dict_cache_kb=1', false) IS NOT NULL AS lowered;
\ir test/agg_rows_helpers.sql

CREATE TABLE tg (id int, s text, k text) WITH (autovacuum_enabled = off);
INSERT INTO tg SELECT i, 'https://shop' || (i % 50) || '.example/item/' || md5(i::text), CASE WHEN i % 7 = 0 THEN NULL ELSE 'key ' || (i % 90) END
FROM generate_series(1, 30000) i;
INSERT INTO tg VALUES (30001, '', ''), (30002, 'ab', 'ab'), (30003, 'abc', 'abc'), (30004, 'naïve café ünï', 'é'), (30005, repeat('x', 200), 'x');
CREATE INDEX tg_idx ON tg USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, k sift.col_text_ops);
VACUUM tg;
SELECT format FROM sift._test_col_formats('tg_idx'::regclass) ORDER BY gen;

-- the long strings have signatures, the short dictionary has none (fewer strings than the lowered minimum)
SELECT bool_and(signed) AS s_signed FROM sift._test_col_dict_tsig('tg_idx'::regclass, 1, 'abc');
SELECT bool_or(signed) AS k_signed FROM sift._test_col_dict_tsig('tg_idx'::regclass, 2, 'abc');
-- a rare literal reads a few blocks, a common one all, one under three bytes all
SELECT sum(blocks_passed) < sum(blocks) / 8 AS rare_reads_few, sum(blocks) AS blocks FROM sift._test_col_dict_tsig('tg_idx'::regclass, 1, (SELECT substr(md5('77'), 3, 12) FROM (SELECT 1) x));
SELECT sum(blocks_passed) = sum(blocks) AS common_reads_all FROM sift._test_col_dict_tsig('tg_idx'::regclass, 1, 'https://shop');
SELECT sum(blocks_passed) = sum(blocks) AS short_reads_all FROM sift._test_col_dict_tsig('tg_idx'::regclass, 1, 'ab');
SELECT sum(blocks_passed) < sum(blocks) / 8 AS absent_reads_few FROM sift._test_col_dict_tsig('tg_idx'::regclass, 1, 'qzx8vk1wjq');

CREATE TABLE tg_pat (pat text);
INSERT INTO tg_pat VALUES ('%a%'), ('%ab%'), ('%abc%'), ('%x%'), ('%xxx%'), ('%' || repeat('x', 150) || '%'), ('%shop7.%'), ('%.example/item/%'), ('%ï%'), ('%naïve café%'),
	('%zzzzzzzz%'), ('%' || substr(md5('77'), 3, 12) || '%'), ('%' || substr(md5('4242'), 9, 5) || '%'), ('%' || md5('1') || '%'), ('%/item/' || substr(md5('9'), 1, 3) || '%'),
	('%e4%'), ('%7f%'), ('%https://shop1.example/item/c4ca4238a0b9%'), ('%é%'), ('%y%');
CREATE FUNCTION tg_check(tag text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r record;
	q text;
	bad int := 0;
	n int := 0;
BEGIN
	FOR r IN SELECT col, pat FROM tg_pat, (VALUES ('s'), ('k')) c(col) ORDER BY col, pat LOOP
		FOREACH q IN ARRAY ARRAY[format('SELECT count(*), sum(id) FROM tg WHERE %I LIKE %L', r.col, r.pat), format('SELECT id FROM tg WHERE %I LIKE %L AND id %% 5 = 1', r.col, r.pat)] LOOP
			n := n + 1;
			IF agg_rows(q, true) IS DISTINCT FROM agg_rows(q, false) THEN
				bad := bad + 1;
				RAISE NOTICE '%: %', tag, q;
			END IF;
		END LOOP;
	END LOOP;
	RETURN tag || ': ' || n || ' statements, ' || bad || ' differ';
END $$;
SELECT tg_check('built');
INSERT INTO tg SELECT 40000 + i, 'https://shop' || (i % 50) || '.example/item/n' || md5(i::text), 'key ' || (i % 90) FROM generate_series(1, 20000) i;
SELECT tg_check('write buffer');
SELECT sift.col_promote('tg_idx') >= 0 AS promoted;
SELECT tg_check('promoted');
DELETE FROM tg WHERE id % 9 = 0;
VACUUM tg;
SELECT sift.col_merge('tg_idx', true) >= 0 AS merged;
SELECT tg_check('merged');
SELECT bool_and(signed) AS merged_signed FROM sift._test_col_dict_tsig('tg_idx'::regclass, 1, 'abc');
SELECT sift.col_verify('tg_idx', true);
REINDEX INDEX tg_idx;
SELECT tg_check('reindexed');

-- a leaf whose dictionary also holds the row counts (count_columns) has the signatures after them
CREATE TABLE tgc (id int, s text) WITH (autovacuum_enabled = off);
INSERT INTO tgc SELECT i, 'https://shop' || (i % 50) || '.example/item/' || md5((i % 9000)::text) FROM generate_series(1, 30000) i;
CREATE INDEX tgc_idx ON tgc USING sift_col (id sift.col_int4_ops, s sift.col_text_ops) WITH (count_columns = 's');
VACUUM tgc;
SELECT bool_and(signed) AS counted_signed FROM sift._test_col_dict_tsig('tgc_idx'::regclass, 1, 'abc');
SELECT sum(blocks_passed) < sum(blocks) / 8 AS counted_rare_reads_few FROM sift._test_col_dict_tsig('tgc_idx'::regclass, 1, (SELECT substr(md5('77'), 3, 12) FROM (SELECT 1) x));
SELECT agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS counted_same
FROM (VALUES ('SELECT count(*), sum(id) FROM tgc WHERE s LIKE ''%' || substr(md5('77'), 3, 12) || '%'''), ('SELECT count(*) FROM tgc WHERE s LIKE ''%.example/item/%'''),
	('SELECT count(*) FROM tgc WHERE s LIKE ''https://shop1.%''')) v(q);
DROP TABLE tgc;

-- a dictionary searched in place (dict_lookup) has the hash of its strings before the signatures
CREATE TABLE tgl (id int, s text) WITH (autovacuum_enabled = off);
INSERT INTO tgl SELECT i, 'https://shop' || (i % 50) || '.example/item/' || md5((i % 9000)::text) FROM generate_series(1, 30000) i;
CREATE INDEX tgl_idx ON tgl USING sift_col (id sift.col_int4_ops, s sift.col_text_ops) WITH (dict_lookup = 'on');
VACUUM tgl;
SELECT bool_and(signed) AS hashed_signed FROM sift._test_col_dict_tsig('tgl_idx'::regclass, 1, 'abc');
SELECT sum(blocks_passed) < sum(blocks) / 8 AS hashed_rare_reads_few FROM sift._test_col_dict_tsig('tgl_idx'::regclass, 1, (SELECT substr(md5('77'), 3, 12) FROM (SELECT 1) x));
SELECT agg_rows(q, true) IS NOT DISTINCT FROM agg_rows(q, false) AS hashed_same
FROM (VALUES ('SELECT count(*), sum(id) FROM tgl WHERE s LIKE ''%' || substr(md5('77'), 3, 12) || '%'''), ('SELECT count(*) FROM tgl WHERE s = ''https://shop1.example/item/' || md5('1') || ''''),
	('SELECT count(*) FROM tgl WHERE s LIKE ''https://shop1.%''')) v(q);
SELECT sift.col_verify('tgl_idx', true);
DROP TABLE tgl;

-- a rare part is searched in the stream test after test (the index is built with a cache of 1 kB, so that it overflows it and is signed; the cache is the default for the searches); a part the signatures hardly help (found in every block) makes the next test parse and keep the dictionary
CREATE TABLE tgh (id int, s text) WITH (autovacuum_enabled = off);
INSERT INTO tgh SELECT i, 'https://shop' || (i % 50) || '.example/item/' || md5(i::text) FROM generate_series(1, 30000) i;
CREATE INDEX tgh_idx ON tgh USING sift_col (id sift.col_int4_ops, s sift.col_text_ops);
VACUUM tgh;
SELECT set_config('sift.test_limits', 'dict_lookup_min=1,dict_tsig_min=1', false) IS NOT NULL AS cache_default;
SELECT count(*) > 0 AS reset FROM sift._test_col_dict_cache(true);
SELECT count(*) FROM tgh WHERE s LIKE '%' || substr(md5('77'), 3, 12) || '%';
SELECT count(*) FROM tgh WHERE s LIKE '%' || substr(md5('78'), 3, 12) || '%';
SELECT loads = 0 AS rare_stays_in_stream FROM sift._test_col_dict_cache(false);
SELECT count(*) FROM tgh WHERE s LIKE '%.example/item/%';
SELECT count(*) FROM tgh WHERE s LIKE '%.example/item/%';
SELECT loads > 0 AS common_parsed FROM sift._test_col_dict_cache(false);
DROP TABLE tgh;

DROP FUNCTION tg_check(text);
DROP TABLE tg_pat, tg;
DROP FUNCTION agg_pushed(text), agg_rows(text, bool);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
