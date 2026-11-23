-- An UPDATE of one column of a table of two or three columns is patched in place (SIFT_PT_COL_DELTA) and reads back as the heap twin does; the rows keep their address and the statement stays cheap.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE EXTENSION IF NOT EXISTS pageinspect;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;

CREATE TABLE n2 (id int PRIMARY KEY, t text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE n3 (id int PRIMARY KEY, n name, u uuid) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE h2 (LIKE n2 INCLUDING ALL);
CREATE TABLE h3 (LIKE n3 INCLUDING ALL);
INSERT INTO n2 SELECT g, 'row ' || g FROM generate_series(1, 20000) g;
INSERT INTO n3 SELECT g, ('nm' || g % 40)::name, ('00000000-0000-4000-8000-' || lpad(to_hex(g), 12, '0'))::uuid FROM generate_series(1, 20000) g;
INSERT INTO h2 SELECT * FROM n2;
INSERT INTO h3 SELECT * FROM n3;
SELECT sift.table_maintain('n2') IS NOT NULL AS maintained_n2, sift.table_maintain('n3') IS NOT NULL AS maintained_n3;

CREATE FUNCTION delta_pages(rel regclass) RETURNS int LANGUAGE sql AS $$
	SELECT count(*)::int FROM generate_series(0, pg_relation_size(rel) / 8192 - 1) n
	WHERE get_byte(get_raw_page(rel::text, n::int), 8176) + 256 * get_byte(get_raw_page(rel::text, n::int), 8177) = 42 $$;
CREATE FUNCTION same(a regclass, b regclass) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE x text; y text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, E''\n'' ORDER BY r::text) FROM %s r', a) INTO x;
	EXECUTE format('SELECT string_agg(r::text, E''\n'' ORDER BY r::text) FROM %s r', b) INTO y;
	RETURN x = y;
END $$;

SELECT delta_pages('n2') AS n2_delta_pages_before, delta_pages('n3') AS n3_delta_pages_before;
UPDATE n2 SET t = 'upd ' || id WHERE id % 50 = 0;
UPDATE h2 SET t = 'upd ' || id WHERE id % 50 = 0;
UPDATE n3 SET n = 'changed' WHERE id % 50 = 0;
UPDATE h3 SET n = 'changed' WHERE id % 50 = 0;
UPDATE n3 SET u = NULL WHERE id % 70 = 0;
UPDATE h3 SET u = NULL WHERE id % 70 = 0;
SELECT delta_pages('n2') > 0 AS n2_patched_in_place, delta_pages('n3') > 0 AS n3_patched_in_place;
SELECT same('n2', 'h2') AS n2_matches_heap, same('n3', 'h3') AS n3_matches_heap;

UPDATE n2 SET t = t || '!' WHERE id % 3 = 0;
UPDATE h2 SET t = t || '!' WHERE id % 3 = 0;
UPDATE n3 SET n = 'again', u = '11111111-1111-4111-8111-111111111111' WHERE id % 5 = 0;
UPDATE h3 SET n = 'again', u = '11111111-1111-4111-8111-111111111111' WHERE id % 5 = 0;
SELECT same('n2', 'h2') AS n2_matches_heap_after_rewrites, same('n3', 'h3') AS n3_matches_heap_after_rewrites;
SELECT count(*) AS n2_check_problems FROM sift.check('n2'::regclass);
SELECT count(*) AS n3_check_problems FROM sift.check('n3'::regclass);

DROP TABLE n2, n3, h2, h3;
DROP FUNCTION delta_pages(regclass), same(regclass, regclass);
DROP EXTENSION pageinspect;
DROP EXTENSION pg_sift;
