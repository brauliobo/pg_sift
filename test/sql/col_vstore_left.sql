-- The only index left on a shared value store goes back to its own strings by itself (docs/COL-VSTORE.md, the only index left): DROP INDEX says which index it is,
-- its next rewrite puts the strings back into its segments (the size is of the index and the vstores of the table, against an index that never shared), the VACUUM after it lets the store go, and the index is then as large as one that never shared.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.async_maintenance = off;

CREATE TABLE lf (id int PRIMARY KEY, url text);
INSERT INTO lf SELECT i, 'https://example.com/p/' || md5(i::text) || md5((i * 7)::text) FROM generate_series(1, 20000) i;
ANALYZE lf;
-- an index that never shared, on a copy of the table, written the same way
CREATE TABLE lf0 (id int PRIMARY KEY, url text);
INSERT INTO lf0 SELECT * FROM lf;
ANALYZE lf0;
SET sift.test_vstore_columns = '-';
CREATE INDEX lf0_a ON lf0 USING sift_col (id, url);
RESET sift.test_vstore_columns;
CREATE FUNCTION lf_check(label text) RETURNS TABLE (step text, same boolean) LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	SELECT count(*) || ':' || md5(string_agg(url, ',' ORDER BY url)) INTO a FROM lf WHERE url > 'https://example.com/p/5';
	SET LOCAL sift.enable_agg = off;
	SELECT count(*) || ':' || md5(string_agg(url, ',' ORDER BY url)) INTO b FROM lf WHERE url > 'https://example.com/p/5';
	RETURN QUERY SELECT label, a = b;
END $$;

CREATE FUNCTION lf_state(label text) RETURNS TABLE (step text, vstores bigint, users int, with_stores text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT label, (SELECT count(*) FROM sift.col_value_store_status WHERE table_name = 'lf'::regclass),
	       (SELECT coalesce(max(indexes_using), 0)::int FROM sift.col_value_store_status WHERE table_name = 'lf'::regclass),
	       (SELECT CASE WHEN abs(x) <= 5 THEN 'within 5% of never shared' WHEN x > 5 THEN 'larger' ELSE 'smaller' END
	          FROM (SELECT 100.0 * ((sift.col_index_info('lf_a')).bytes + coalesce((SELECT sum(bytes) FROM sift.col_value_store_status WHERE table_name = 'lf'::regclass), 0)
	                                - (sift.col_index_info('lf0_a')).bytes) / (sift.col_index_info('lf0_a')).bytes AS x) q);
END $$;

CREATE INDEX lf_a ON lf USING sift_col (id, url);
CREATE INDEX lf_b ON lf USING sift_col (url, id);
REINDEX INDEX lf_a;
SELECT * FROM lf_check('two peers');
SELECT index_name::text, state FROM sift.col_value_store_members() WHERE table_name = 'lf'::regclass AND column_name = 'url' ORDER BY 1;

SET client_min_messages = notice;
DROP INDEX lf_b;
RESET client_min_messages;
SELECT * FROM lf_check('lf_b dropped'), lf_state('lf_b dropped');

-- the strings of rows written now go into the segments; the copy gets the same rows
INSERT INTO lf SELECT 20000 + i, 'https://example.com/n/' || md5(i::text) FROM generate_series(1, 2000) i;
INSERT INTO lf0 SELECT * FROM lf WHERE id > 20000;
SELECT sift.col_promote('lf_a') > 0 AS promoted, sift.col_promote('lf0_a') > 0 AS promoted_never_shared;
SELECT sift.col_merge('lf_a', true) >= 0 AS merged, sift.col_merge('lf0_a', true) >= 0 AS merged_never_shared;
SELECT * FROM lf_check('rows added'), lf_state('rows added');
SELECT sift.col_maintain('lf_a') AS rewrote;
SELECT * FROM lf_check('rewritten'), lf_state('rewritten');
SELECT sift.col_maintain('lf_a') AS nothing_more;
VACUUM lf;
SELECT * FROM lf_check('vacuumed'), lf_state('vacuumed');
SELECT count(*) AS relations_left FROM pg_class WHERE relname LIKE 'lf%sift\_values%';
SELECT sift.col_verify('lf_a', true);

DROP FUNCTION lf_check(text), lf_state(text);
DROP TABLE lf, lf0;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
