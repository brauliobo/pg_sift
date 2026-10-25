-- The key list and bitmap of a leaf in every unit: an index with the option key_columns keeps, per unit and int2, int4
-- or date leaf, every distinct value as a bitmap or a list, so that a count(DISTINCT leaf) over whole units reads no row. Written only under sift.col_format_minor = 15
-- and for the leaves of the option, where they take at most 3% of the unit's bytes, so that an older library keeps reading what an index without the
-- option, or under a lower setting, writes; read back by the decoder that refuses a damaged bitmap; recomputed by sift.col_verify; kept by promotion, merge
-- and VACUUM; the answers are the heap's.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET statement_timeout = '60s';

-- 60,000 rows in about 8 units of 7,500 rows. k: 3,000 values scattered over every unit; kn: 1,500 values from -700, NULL for every 7th row; s: an int2 from -250;
-- d: a date; ts: ascending with id (a window covers the middle units whole and cuts two); g: 5 groups; big: sparse values (no bitmap fits 3% of the unit)
CREATE TABLE kp (id int, k int, kn int, s int2, d date, ts date, g int, big int, val int, sp int, sp2 int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE kp ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kp
SELECT i, (hashint4(i) & 2147483647) % 3000 + 1,
       CASE WHEN i % 7 = 0 THEN NULL ELSE (hashint4(i + 1) & 2147483647) % 1500 - 700 END,
       ((hashint4(i + 2) & 2147483647) % 500 - 250)::int2,
       date '2025-01-01' + (hashint4(i + 3) & 2147483647) % 900,
       date '2024-01-01' + i / 150, i % 5, hashint4(i + 4), i % 1000,
       ((hashint4(i + 5) & 2147483647) % 300) * 3331, ((hashint4(i + 6) & 2147483647) % 200 - 100) * 150, repeat('x', 200)
FROM generate_series(1, 60000) i;
CREATE TABLE kp_b (LIKE kp) WITH (autovacuum_enabled = off);
ALTER TABLE kp_b ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kp_b SELECT * FROM kp ORDER BY id;
VACUUM (FREEZE, ANALYZE) kp;
VACUUM (FREEZE, ANALYZE) kp_b;

-- the minor of the segments: 4 (this schema has no jsonb paths) without the option or under a setting below 10, 15 with both (a setting from 10 to 14 writes the summaries of section 2.13 for the option's leaves, not the lists)
CREATE INDEX kp_off ON kp USING sift_col (id, k, kn, s, d, ts, g, big, val, sp, sp2) WITH (write_buffer_limit = 4096);
SET sift.col_format_minor = 9;
CREATE INDEX kp_old0 ON kp USING sift_col (id, k, kn, s, d, ts, g, big, val, sp, sp2) WITH (write_buffer_limit = 4096);
CREATE INDEX kp_old ON kp USING sift_col (id, k, kn, s, d, ts, g, big, val, sp, sp2) WITH (write_buffer_limit = 4096, key_columns = 'k, kn, s, d, sp, sp2');
RESET sift.col_format_minor;
CREATE INDEX kp_on ON kp USING sift_col (id, k, kn, s, d, ts, g, big, val, sp, sp2) WITH (write_buffer_limit = 4096, key_columns = 'k, kn, s, d, sp, sp2');
SELECT 'kp_off' AS idx, array_agg(DISTINCT format) AS minors FROM sift._test_col_formats('kp_off')
UNION ALL SELECT 'kp_old', array_agg(DISTINCT format) FROM sift._test_col_formats('kp_old')
UNION ALL SELECT 'kp_on', array_agg(DISTINCT format) FROM sift._test_col_formats('kp_on') ORDER BY 1;
SELECT sift.col_verify('kp_off', true), sift.col_verify('kp_old', true), sift.col_verify('kp_on', true);
-- under a setting below 10 the bytes are those of an index without the option
SELECT pg_relation_size('kp_old') = pg_relation_size('kp_old0') AS old_is_off_size,
       (SELECT array_agg(section || coalesce(':' || leaf, '') || '=' || bytes ORDER BY section, leaf) FROM sift.col_sections('kp_old'))
       = (SELECT array_agg(section || coalesce(':' || leaf, '') || '=' || bytes ORDER BY section, leaf) FROM sift.col_sections('kp_old0')) AS old_is_off_sections;
SELECT count(*) FILTER (WHERE section = 'sets') AS rows_off FROM sift.col_sections('kp_off');
-- a few percent of the index: the bitmaps and lists of k, kn, s, d, sp and sp2
SELECT bytes > 0 AS sets_take_bytes, bytes * 100 < pg_relation_size('kp_on') * 8 AS under_8_percent FROM sift.col_sections('kp_on') WHERE section = 'sets';
DROP INDEX kp_old, kp_old0;

-- the options are checked
CREATE INDEX kp_bad1 ON kp USING sift_col (id, k) WITH (key_columns = 'nosuch');
CREATE INDEX kp_bad2 ON kp USING sift_col (id, k, val) WITH (key_columns = 'k,,val');
CREATE TABLE kp_t (id int, t text, b bigint, f float8, ts timestamp, u uuid);
CREATE INDEX kp_bad5 ON kp_t USING sift_col (id, f) WITH (key_columns = 'f');
DROP TABLE kp_t;

-- three ways: the bitmaps on, the twin table's index without them, the plain plan; the units the bitmaps answered
CREATE FUNCTION kp_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
EXCEPTION WHEN OTHERS THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;
CREATE FUNCTION kp_units(q text, OUT units int, OUT pre int, OUT scanned int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		units := coalesce(substring(l from '^\s*Units: (\d+)')::int, units);
		pre := coalesce(substring(l from 'Units From Pre-aggregates: (\d+)')::int, pre);
		scanned := coalesce(substring(l from 'Rows Scanned: (\d+)')::int, scanned);
	END LOOP;
EXCEPTION WHEN OTHERS THEN
	units := NULL; pre := NULL; scanned := NULL;
END $$;
CREATE TABLE kp_q (n int, shape text, q text);
INSERT INTO kp_q VALUES
	(1, 'distinct int4',              'SELECT count(DISTINCT k) FROM %s'),
	(2, 'with the sums',              'SELECT count(DISTINCT k), count(*), sum(val) FROM %s'),
	(3, 'NULLs and negatives',        'SELECT count(DISTINCT kn), count(kn), min(kn) FROM %s'),
	(4, 'int2',                       'SELECT count(DISTINCT s) FROM %s'),
	(5, 'date',                       'SELECT count(DISTINCT d) FROM %s'),
	(6, 'four distincts',             'SELECT count(DISTINCT k), count(DISTINCT kn), count(DISTINCT s), count(DISTINCT d) FROM %s'),
	(7, 'window, whole units',        'SELECT count(DISTINCT k), count(*) FROM %s WHERE ts >= date ''2024-03-01'' AND ts < date ''2024-11-15'''),
	(8, 'window, one bound',          'SELECT count(DISTINCT k) FROM %s WHERE ts >= date ''2024-06-01'''),
	(9, 'window, no row',             'SELECT count(DISTINCT k), count(*) FROM %s WHERE ts >= date ''2030-01-01'''),
	(10, 'window of one unit',        'SELECT count(DISTINCT s) FROM %s WHERE ts < date ''2024-02-01'''),
	(11, 'filter on another column',  'SELECT count(DISTINCT k) FROM %s WHERE val < 500'),
	(12, 'FILTER',                    'SELECT count(DISTINCT k) FILTER (WHERE val < 500), count(*) FROM %s'),
	(13, 'grouped',                   'SELECT g, count(DISTINCT k) FROM %s GROUP BY g'),
	(14, 'the key tested',            'SELECT count(DISTINCT k) FROM %s WHERE k BETWEEN 100 AND 1200'),
	(15, 'the key and a window',      'SELECT count(DISTINCT k) FROM %s WHERE k < 1000 AND ts >= date ''2024-03-01'' AND ts < date ''2024-11-15'''),
	(16, 'distinct of an expression', 'SELECT count(DISTINCT k + 1), count(DISTINCT k %% 7) FROM %s'),
	(17, 'sparse values (no bitmap)', 'SELECT count(DISTINCT big) FROM %s'),
	(18, 'also the plain sums',      'SELECT count(DISTINCT s), sum(val), min(val), max(val) FROM %s WHERE ts >= date ''2024-02-01'''),
	(19, 'distinct of the id',        'SELECT count(DISTINCT id) FROM %s'),
	(20, 'two keys',                  'SELECT count(DISTINCT k), count(DISTINCT g) FROM %s'),
	(21, 'sparse few values (list)',  'SELECT count(DISTINCT sp), count(*) FROM %s'),
	(22, 'negative base (list)',      'SELECT count(DISTINCT sp2), min(sp2) FROM %s'),
	(23, 'list and bitmap, window',   'SELECT count(DISTINCT sp), count(DISTINCT s), count(DISTINCT k) FROM %s WHERE ts >= date ''2024-03-01'' AND ts < date ''2024-11-15''');
CREATE FUNCTION kp_check(a text, b text) RETURNS TABLE (n int, shape text, same bool, from_pre bool, twin_scans bool) LANGUAGE plpgsql AS $$
DECLARE r record; qa text; qb text; ua record; ub record;
BEGIN
	FOR r IN SELECT * FROM kp_q ORDER BY kp_q.n LOOP
		qa := format(r.q, a); qb := format(r.q, b);
		SELECT * INTO ua FROM kp_units(qa);
		SELECT * INTO ub FROM kp_units(qb);
		n := r.n; shape := r.shape;
		from_pre := ua.pre > 0 AND ua.scanned < ub.scanned;
		twin_scans := ub.scanned > 0;
		same := kp_rows(qa, true) = kp_rows(qb, true) AND kp_rows(qa, true) = kp_rows(qa, false) AND kp_rows(qa, true) NOT LIKE 'ERROR%';
		RETURN NEXT;
	END LOOP;
END $$;
DROP INDEX kp_off;
CREATE INDEX kp_b_off ON kp_b USING sift_col (id, k, kn, s, d, ts, g, big, val, sp, sp2) WITH (write_buffer_limit = 4096);
-- from_pre: the scan read fewer rows than the twin's, because units came from their bitmaps
SELECT n, shape, same, from_pre, twin_scans FROM kp_check('kp', 'kp_b');

-- the same query on the units: all of them, the ones a window covers, none for a filter of another column or a key the plan cannot answer
-- (the last unit holds too few rows for a bitmap to take 3% of its bytes, and is scanned)
SELECT (SELECT scanned FROM kp_units('SELECT count(DISTINCT k), count(*), sum(val) FROM kp')) < 5000 AS only_the_tail_scanned,
       (SELECT units - pre FROM kp_units('SELECT count(DISTINCT k), count(*), sum(val) FROM kp')) <= 1 AS every_unit_but_the_tail_pre,
       (SELECT scanned FROM kp_units('SELECT count(DISTINCT k) FROM kp WHERE ts >= date ''2024-03-01'' AND ts < date ''2024-11-15''')) BETWEEN 1 AND 30000 AS window_scans_its_ends,
       (SELECT pre FROM kp_units('SELECT count(DISTINCT k) FROM kp WHERE ts >= date ''2024-03-01'' AND ts < date ''2024-11-15''')) > 0 AS window_answers_inside,
       (SELECT pre FROM kp_units('SELECT count(DISTINCT k) FROM kp WHERE val < 500')) = 0 AS filter_of_another_column_scans,
       (SELECT scanned FROM kp_units('SELECT count(DISTINCT big) FROM kp')) = 60000 AS no_bitmap_scans,
       (SELECT scanned FROM kp_units('SELECT count(DISTINCT k) FROM kp_b')) = 60000 AS twin_scans_all;
SELECT sift.col_verify('kp_on', true);

-- workers each hold their own set of values and the leader merges them: the same answer, the units come from their bitmaps
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT n, shape, same FROM kp_check('kp', 'kp_b') WHERE NOT same;
SELECT (SELECT scanned FROM kp_units('SELECT count(DISTINCT k), count(DISTINCT s) FROM kp')) < 5000 AS parallel_from_bitmaps;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
SET max_parallel_workers_per_gather = 0;

-- rows in the write buffer are read whatever the bitmaps say; a delete or an update makes its unit not exact, and it is scanned; VACUUM and promotion, merge
-- compute the bitmaps again
INSERT INTO kp SELECT g, 5000 + g % 11, 9000 + g % 13, 300 + (g % 17)::int2, date '2030-01-01' + g % 5, date '2026-01-01', g % 5, g, g % 1000, g % 300 * 3331, (g % 200 - 100) * 150, repeat('x', 200) FROM generate_series(60001, 60400) g;
INSERT INTO kp_b SELECT g, 5000 + g % 11, 9000 + g % 13, 300 + (g % 17)::int2, date '2030-01-01' + g % 5, date '2026-01-01', g % 5, g, g % 1000, g % 300 * 3331, (g % 200 - 100) * 150, repeat('x', 200) FROM generate_series(60001, 60400) g;
SELECT n, shape, same FROM kp_check('kp', 'kp_b') WHERE NOT same;
SELECT count(DISTINCT k) AS k_with_new_rows FROM kp;
UPDATE kp SET k = 7000 + id % 3 WHERE id % 1000 = 5;
UPDATE kp_b SET k = 7000 + id % 3 WHERE id % 1000 = 5;
DELETE FROM kp WHERE id % 7 = 3;
DELETE FROM kp_b WHERE id % 7 = 3;
SELECT n, shape, same FROM kp_check('kp', 'kp_b') WHERE NOT same;
SELECT count(DISTINCT k) AS k_after_dml FROM kp;
VACUUM (freeze, index_cleanup on, disable_page_skipping) kp;
VACUUM (freeze, index_cleanup on, disable_page_skipping) kp_b;
SELECT n, shape, same FROM kp_check('kp', 'kp_b') WHERE NOT same;
SELECT sift.col_promote('kp_on') >= 0 AS promoted;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('kp_on');
SELECT sift.col_verify('kp_on', true);
SELECT n, shape, same FROM kp_check('kp', 'kp_b') WHERE NOT same;
SELECT sift.col_merge('kp_on', true) >= 0 AS merged;
VACUUM (freeze, index_cleanup on, disable_page_skipping) kp;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors, count(*) AS segments FROM sift._test_col_formats('kp_on');
SELECT sift.col_verify('kp_on', true);
SELECT n, shape, same, from_pre FROM kp_check('kp', 'kp_b') WHERE NOT same OR n IN (1, 2, 7);

-- the bitmaps are decoded with checks: a cut blob, a flag bit, an unknown kind, no keys, a count that does not match the bits, a first or last word that is
-- zero are refused; a changed byte that is accepted is another valid bitmap, and the comparison with the chunk (what sift.col_verify does) refuses it
SELECT blobs > 0 AS has_blobs, exact = 0 AND bloom = 0 AS only_bitmaps, intact = blobs AS intact_decodes, matching = blobs AS intact_match_chunks,
       cut > 0 AS cuts_made, cut_accepted = 0 AS cuts_refused, named > 0 AS named_made, named_accepted = 0 AS named_refused,
       damaged > 0 AS damages_made, accepted < damaged AS some_refused, missed = 0 AS accepted_differ_from_chunk
FROM sift._test_col_set_damage('kp_on', 6);

-- a bit flipped, one at a time, in the section of the summaries in the stream (its structure, every 7th byte of the bitmaps): the reader or the check of the
-- bitmaps against the chunks refuses each. Each flip opens and verifies the whole segment, so the cost grows with the square of the rows: two units of the table
CREATE TABLE kp_f (LIKE kp) WITH (autovacuum_enabled = off);
ALTER TABLE kp_f ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kp_f SELECT * FROM kp WHERE id <= 15000 ORDER BY id;
VACUUM (FREEZE, ANALYZE) kp_f;
CREATE INDEX kp_f_on ON kp_f USING sift_col (id, k, kn, s, d, ts, g, big, val, sp, sp2) WITH (write_buffer_limit = 4096, key_columns = 'k, kn, s, d, sp, sp2');
SELECT runs > 0 AS has_sections, damaged > 1000 AS many, refused > 0 AS some_refused, unnoticed FROM sift._test_col_set_file_damage('kp_f_on', 7);
DROP TABLE kp_f;

-- the same index built by workers: the same bytes of bitmaps, the same answers
CREATE TABLE kp_w (LIKE kp) WITH (autovacuum_enabled = off);
ALTER TABLE kp_w ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kp_w SELECT * FROM kp ORDER BY id;
VACUUM (FREEZE, ANALYZE) kp_w;
ALTER TABLE kp_w SET (parallel_workers = 3);
SET max_parallel_maintenance_workers = 3;
SET min_parallel_table_scan_size = 0;
SET parallel_setup_cost = 0;
CREATE INDEX kp_w_on ON kp_w USING sift_col (id, k, kn, s, d, ts, g, big, val, sp, sp2) WITH (write_buffer_limit = 4096, key_columns = 'k, kn, s, d, sp, sp2');
RESET max_parallel_maintenance_workers;
RESET min_parallel_table_scan_size;
RESET parallel_setup_cost;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('kp_w_on');
SELECT sift.col_verify('kp_w_on', true);
SELECT (SELECT scanned FROM kp_units('SELECT count(DISTINCT k), count(*) FROM kp_w')) < 5000 AS built_by_workers_answers,
       kp_rows('SELECT count(DISTINCT k), count(DISTINCT s), count(*) FROM kp_w', true) = kp_rows('SELECT count(DISTINCT k), count(DISTINCT s), count(*) FROM kp_w', false) AS same_as_heap;
DROP TABLE kp_w;

-- an index of the old kind takes the option later: the units written before keep their minor and have no bitmap (they are scanned), the ones after are minor 15,
-- a merge writes one segment of minor 15; the option switched off, the next merge writes the old minor; under a lower setting nothing of it is written
CREATE TABLE ka (id int, k int, val int, ts date, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE ka ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ka SELECT g, (hashint4(g) & 2147483647) % 2000, g % 1000, date '2024-01-01' + g / 150, repeat('x', 200) FROM generate_series(1, 30000) g;
CREATE INDEX ka_idx ON ka USING sift_col (id, k, val, ts) WITH (write_buffer_limit = 4096);
SELECT array_agg(DISTINCT format) AS minors FROM sift._test_col_formats('ka_idx');
ALTER INDEX ka_idx SET (key_columns = 'k');
INSERT INTO ka SELECT g, (hashint4(g) & 2147483647) % 2000, g % 1000, date '2024-01-01' + g / 150, repeat('x', 200) FROM generate_series(30001, 60000) g;
SELECT sift.col_promote('ka_idx') >= 0 AS promoted;
VACUUM (freeze) ka;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('ka_idx');
SELECT sift.col_verify('ka_idx', true);
SELECT (SELECT pre FROM kp_units('SELECT count(DISTINCT k) FROM ka')) > 0 AS new_units_answer,
       (SELECT scanned FROM kp_units('SELECT count(DISTINCT k) FROM ka')) > 0 AS old_units_scan;
SELECT kp_rows('SELECT count(DISTINCT k) FROM ka', true) = kp_rows('SELECT count(DISTINCT k) FROM ka', false) AS same_as_heap;
SELECT sift.col_merge('ka_idx', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors, count(*) AS segments FROM sift._test_col_formats('ka_idx');
SELECT (SELECT scanned FROM kp_units('SELECT count(DISTINCT k) FROM ka')) < 5000 AS merged_answers_all,
       kp_rows('SELECT count(DISTINCT k) FROM ka', true) = kp_rows('SELECT count(DISTINCT k) FROM ka', false) AS same_as_heap;
SELECT sift.col_verify('ka_idx', true);
ALTER INDEX ka_idx SET (key_columns = '');
INSERT INTO ka SELECT g, g % 2000, g % 1000, date '2025-01-01', repeat('x', 200) FROM generate_series(60001, 70000) g;
SELECT sift.col_promote('ka_idx') >= 0 AS promoted;
SELECT sift.col_merge('ka_idx', true) >= 0 AS merged;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('ka_idx');
SELECT blobs AS bitmaps FROM sift._test_col_set_damage('ka_idx', 3);
SELECT sift.col_verify('ka_idx', true);
ALTER INDEX ka_idx SET (key_columns = 'k');
SET sift.col_format_minor = 9;
INSERT INTO ka SELECT g, g % 2000, g % 1000, date '2025-02-01', repeat('x', 200) FROM generate_series(70001, 75000) g;
SELECT sift.col_promote('ka_idx') >= 0 AS promoted;
SELECT sift.col_merge('ka_idx', true) >= 0 AS merged;
RESET sift.col_format_minor;
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('ka_idx');
SELECT blobs AS bitmaps FROM sift._test_col_set_damage('ka_idx', 3);
SELECT sift.col_verify('ka_idx', true);
SELECT kp_rows('SELECT count(DISTINCT k), count(*) FROM ka', true) = kp_rows('SELECT count(DISTINCT k), count(*) FROM ka', false) AS same_as_heap;

-- a leaf in both options keeps its bitmap where it fits and its small-set summary where it does not, and the equality test is skipped by either
CREATE TABLE kb (id int, k int, big int, val int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE kb ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kb SELECT g, (hashint4(g) & 2147483647) % 2000, hashint4(g), g % 1000, repeat('x', 200) FROM generate_series(1, 30000) g;
CREATE INDEX kb_idx ON kb USING sift_col (id, k, big, val) WITH (write_buffer_limit = 4096, key_columns = 'k, big');
SELECT sift.col_verify('kb_idx', true);
SELECT array_agg(DISTINCT format ORDER BY format) AS minors FROM sift._test_col_formats('kb_idx');
SELECT kp_rows('SELECT count(*), sum(val) FROM kb WHERE k = 17', true) = kp_rows('SELECT count(*), sum(val) FROM kb WHERE k = 17', false) AS k_point_same,
       kp_rows('SELECT count(*), sum(val) FROM kb WHERE big = ' || (SELECT big FROM kb WHERE id = 777), true) = kp_rows('SELECT count(*), sum(val) FROM kb WHERE big = ' || (SELECT big FROM kb WHERE id = 777), false) AS big_point_same,
       kp_rows('SELECT count(DISTINCT k), count(DISTINCT big) FROM kb', true) = kp_rows('SELECT count(DISTINCT k), count(DISTINCT big) FROM kb', false) AS distinct_same,
       kp_rows('SELECT count(*) FROM kb WHERE k BETWEEN 100 AND 130', true) = kp_rows('SELECT count(*) FROM kb WHERE k BETWEEN 100 AND 130', false) AS range_same;
SELECT sift.col_merge('kb_idx', true) >= 0 AS merged;
SELECT sift.col_verify('kb_idx', true);
DROP TABLE kb, ka, kp_q, kp, kp_b;
DROP FUNCTION kp_rows(text, bool), kp_units(text), kp_check(text, text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
