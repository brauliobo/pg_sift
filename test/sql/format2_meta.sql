-- Format major 2: the metapage, the part-header sections, and the refusal of an index of another format major.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ' ' || SQLERRM;
END $$;

-- An index of format major 1 is refused with the object-not-in-prerequisite-state class, naming REINDEX, and is neither corrupted nor quarantined: it reads on.
CREATE TABLE legacy (id int, v int);
INSERT INTO legacy SELECT g, g % 7 FROM generate_series(1, 2000) g;
SET sift.test_create_format1 = on;
CREATE INDEX legacy_col ON legacy USING sift_col (id, v);
SET sift.test_create_format1 = off;
SELECT regexp_replace(expect('SELECT sift._test_f2_meta(''legacy_col'')'), 'format major [0-9.]+', 'format major N');
SELECT regexp_replace(expect('SELECT sift._test_f2_check(''legacy_col'')'), 'format major [0-9.]+', 'format major N');
SELECT count(*) FROM legacy WHERE v = 3;

-- A fresh index of format major 2
CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT sift._test_f2_init('t_idx');
SELECT major, minor, struct_size, npartitions, manifest_version, next_gen, total_rows, partdir_level, high_water - meta_slab_end AS data_zone_blocks
FROM sift._test_f2_meta('t_idx');
SELECT sift._test_f2_check('t_idx');

-- A metapage of the pre-release layout (392 bytes) is refused as a format mismatch with a rebuild hint: it is neither corruption nor quarantined, and the bytes read again once sealed at the size it had
SELECT sift._test_f2_meta_resize('t_idx', 392) AS full_size \gset
SELECT expect('SELECT sift._test_f2_meta(''t_idx'')');
SELECT expect('SELECT sift._test_f2_check(''t_idx'')');
SELECT sift._test_f2_meta_resize('t_idx', :full_size) AS was;
SELECT sift._test_f2_check('t_idx');

-- Sections: written sorted by tag, read back with their checksums, an unknown optional tag skipped, an unknown required tag refused
SELECT sift._test_f2_sections('t_idx', false, 100, 1000, 20000);
SELECT sift._test_f2_sections('t_idx', false, 1, 1, 20000);
SELECT sift._test_f2_sections('t_idx', false, 0, 1, 8152 * 1017);
SELECT regexp_replace(expect('SELECT sift._test_f2_sections(''t_idx'', true, 1, 1, 20000)'), 'part [0-9]+', 'part N');
SELECT sift._test_f2_check('t_idx');

DROP TABLE legacy;
DROP TABLE t;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
