-- Format major 2 filters the chunks of a granule as they are stored (dictionary codes, runs, packed offsets, deltas, decimal floats, bits, coded strings, with and without nulls), reads
-- the other columns only for the rows that are left, keeps summaries of the values of each granule of a part (skipping indexes) that rule out granules a point, IN or substring
-- predicate cannot match, and rewrites the part that holds the end of the file into the free space below it so that the file shrinks. Every answer is the answer of the heap.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

-- {exact, recheck, zones, read, sparse, skip probes, skip prunes, parts pruned, compressed filters, granules left without a row}
CREATE FUNCTION scan_stats(OUT probed bool, OUT pruned bool, OUT part_pruned bool, OUT compressed bool, OUT late_skipped bool) LANGUAGE sql AS $$
	SELECT c[6] > 0, c[7] > 0, c[8] > 0, c[9] > 0, c[10] > 0 FROM (SELECT sift._test_f2_scan_counters(true) AS c) s $$;

CREATE FUNCTION run(q text, OUT parity text, OUT probed bool, OUT pruned bool, OUT part_pruned bool, OUT compressed bool, OUT late_skipped bool) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift._test_f2_scan_counters(true);
	parity := f2_same(q);
	SELECT * INTO probed, pruned, part_pruned, compressed, late_skipped FROM scan_stats();
END $$;

-- The shapes of the codecs, on columns with and without nulls: a dictionary, runs, a frame of reference, a delta, a decimal float, a boolean, unique strings
CREATE TABLE cp (id int, s_low text COLLATE "C", r_run int, f_for int, dl int, fl float8, b bool, s_u text COLLATE "C", n_s text COLLATE "C", n_i int, n_u text COLLATE "C");
INSERT INTO cp SELECT i, 'g' || (i % 12), i / 3000, 100000 + i % 1000, i * 3, round((i % 500) / 10.0, 1), i % 3 = 0, 'u-' || lpad(i::text, 6, '0'),
	CASE WHEN i % 7 = 0 THEN NULL ELSE 'v' || (i % 40) END, CASE WHEN i % 5 = 0 THEN NULL ELSE i % 50 END, CASE WHEN i % 9 = 0 THEN NULL ELSE 'w-' || substr(md5(i::text), 1, 10) END
FROM generate_series(1, 30000) i;
CREATE INDEX cp_i ON cp USING sift_col (id, s_low, r_run, f_for, dl, fl, b, s_u, n_s, n_i, n_u) WITH (key_order = 'id');
SELECT sift._test_f2_check('cp_i') ~ '^parts=[0-9]+ ' AS consistent;
SELECT n_u AS nu FROM cp WHERE id = 5 \gset

SELECT sift._test_f2_scan_counters(true) IS NOT NULL AS reset;
SELECT n, q, f2_same(q) FROM (VALUES
	(1, 'SELECT id FROM cp WHERE s_low = ''g3'''),
	(2, 'SELECT id FROM cp WHERE s_low <> ''g3'''),
	(3, 'SELECT id FROM cp WHERE s_low IN (''g1'', ''g5'', ''zz'')'),
	(4, 'SELECT id FROM cp WHERE s_low < ''g2'''),
	(5, 'SELECT id FROM cp WHERE s_low >= ''g9'''),
	(6, 'SELECT id FROM cp WHERE s_low = ''absent'''),
	(7, 'SELECT id FROM cp WHERE s_low LIKE ''g1%'''),
	(10, 'SELECT id FROM cp WHERE r_run = 3'),
	(11, 'SELECT id FROM cp WHERE r_run <> 3'),
	(12, 'SELECT id FROM cp WHERE r_run BETWEEN 2 AND 4'),
	(13, 'SELECT id FROM cp WHERE r_run IN (1, 7)'),
	(14, 'SELECT id FROM cp WHERE r_run > 8'),
	(20, 'SELECT id FROM cp WHERE f_for = 100500'),
	(21, 'SELECT id FROM cp WHERE f_for <> 100500'),
	(22, 'SELECT id FROM cp WHERE f_for BETWEEN 100100 AND 100200'),
	(23, 'SELECT id FROM cp WHERE f_for IN (100001, 100999, 5)'),
	(24, 'SELECT id FROM cp WHERE f_for < 100003'),
	(30, 'SELECT id FROM cp WHERE dl = 9000'),
	(31, 'SELECT id FROM cp WHERE dl < 300'),
	(32, 'SELECT id FROM cp WHERE dl >= 89000'),
	(33, 'SELECT id FROM cp WHERE dl BETWEEN 3000 AND 3030'),
	(34, 'SELECT id FROM cp WHERE dl IN (3, 9, 12)'),
	(35, 'SELECT id FROM cp WHERE dl <> 3'),
	(40, 'SELECT id FROM cp WHERE fl = 12.5'),
	(41, 'SELECT id FROM cp WHERE fl <> 12.5'),
	(42, 'SELECT id FROM cp WHERE fl < 0.3'),
	(43, 'SELECT id FROM cp WHERE fl BETWEEN 10 AND 10.5'),
	(44, 'SELECT id FROM cp WHERE fl IN (0.1, 49.9, 3.3)'),
	(50, 'SELECT id FROM cp WHERE b'),
	(51, 'SELECT id FROM cp WHERE NOT b'),
	(52, 'SELECT id FROM cp WHERE b = false'),
	(60, 'SELECT id FROM cp WHERE s_u = ''u-012345'''),
	(61, 'SELECT id FROM cp WHERE s_u <> ''u-012345'''),
	(62, 'SELECT id FROM cp WHERE s_u LIKE ''u-0123%'''),
	(63, 'SELECT id FROM cp WHERE s_u IN (''u-000001'', ''u-029999'', ''nope'')'),
	(64, 'SELECT id FROM cp WHERE s_u LIKE ''%-0123%'''),
	(65, 'SELECT id FROM cp WHERE s_u < ''u-000100'''),
	(66, 'SELECT id FROM cp WHERE s_u LIKE ''%234%'' AND id < 10000'),
	(67, 'SELECT id FROM cp WHERE s_u ILIKE ''U-0123%'''),
	(68, 'SELECT id FROM cp WHERE s_low ILIKE ''%G1%'''),
	(70, 'SELECT id FROM cp WHERE n_s = ''v7'''),
	(71, 'SELECT id FROM cp WHERE n_s <> ''v7'''),
	(72, 'SELECT id FROM cp WHERE n_s IS NULL'),
	(73, 'SELECT id FROM cp WHERE n_s IS NOT NULL AND n_s < ''v2'''),
	(74, 'SELECT id FROM cp WHERE n_s IN (''v1'', ''v39'')'),
	(80, 'SELECT id FROM cp WHERE n_i = 7'),
	(81, 'SELECT id FROM cp WHERE n_i <> 7'),
	(82, 'SELECT id FROM cp WHERE n_i BETWEEN 10 AND 12'),
	(83, 'SELECT id FROM cp WHERE n_i IN (1, 2, 3)'),
	(84, 'SELECT id FROM cp WHERE n_i IS NULL'),
	(85, 'SELECT id FROM cp WHERE n_i > 45'),
	(90, format('SELECT id FROM cp WHERE n_u = %L', :'nu')),
	(91, format('SELECT id FROM cp WHERE n_u <> %L', :'nu')),
	(92, 'SELECT id FROM cp WHERE n_u IS NULL'),
	(93, 'SELECT id FROM cp WHERE n_u LIKE ''w-a%'''),
	(100, 'SELECT id FROM cp WHERE s_low = ''g3'' AND f_for BETWEEN 100100 AND 100400 AND n_i = 3'),
	(101, 'SELECT id FROM cp WHERE r_run = 2 AND dl < 20000 AND fl > 20.0'),
	(102, 'SELECT id FROM cp WHERE n_s = ''v3'' AND n_i IS NULL'),
	(103, 'SELECT id FROM cp WHERE b AND s_low IN (''g0'', ''g3'', ''g6'') AND n_u IS NOT NULL'),
	(104, 'SELECT id FROM cp WHERE s_low = ''absent'' AND f_for = 100500')
) v(n, q) ORDER BY n;
SELECT compressed, late_skipped FROM scan_stats();

-- A predicate no string of the dictionary of the part passes rules out the part, and a part with rows left reads their other columns only
SELECT parity, part_pruned, compressed FROM run('SELECT id FROM cp WHERE s_low = ''absent''');
SELECT parity, late_skipped, compressed FROM run('SELECT id FROM cp WHERE s_low = ''g3'' AND f_for = 100501');

-- High cardinality: a point, an IN list and a substring that the zones cannot rule out are ruled out granule by granule by the summaries kept for each granule (within a hundredth of the
-- bytes of the part: the rows here are wide); the answers are the heap's
CREATE TABLE sk (id int, k_hi int, k_n int, note text COLLATE "C", pad1 bytea, pad2 bytea);
INSERT INTO sk SELECT i, ((i::bigint * 7919) % 1000003)::int, CASE WHEN i % 3 = 0 THEN NULL ELSE ((i::bigint * 104729) % 999983)::int END,
	'ord-' || lpad(i::text, 6, '0') || '-' || (ARRAY['alpha', 'beta', 'gamma', 'delta'])[i % 4 + 1],
	(SELECT decode(string_agg(md5(i::text || 'a' || k), ''), 'hex') FROM generate_series(1, 20) k), (SELECT decode(string_agg(md5(i::text || 'b' || k), ''), 'hex') FROM generate_series(1, 20) k)
FROM generate_series(1, 40000) i;
CREATE INDEX sk_i ON sk USING sift_col (id, k_hi, k_n, note, pad1, pad2) WITH (key_order = 'id');
SELECT sift._test_f2_check('sk_i') ~ '^parts=[0-9]+ ' AS consistent;
SELECT k_hi AS hi, k_n AS kn, note AS one FROM sk WHERE id = 31337 \gset

SELECT n, parity, probed, pruned FROM (VALUES
	(1, format('SELECT id FROM sk WHERE k_hi = %s', :hi)),
	(2, format('SELECT id FROM sk WHERE k_hi IN (%s, 5, 99)', :hi)),
	(3, format('SELECT id FROM sk WHERE k_hi = %s AND id > 100', :hi)),
	(4, format('SELECT id FROM sk WHERE k_n = %s', :kn)),
	(5, 'SELECT id FROM sk WHERE k_n = 123456789'),
	(6, format('SELECT id FROM sk WHERE note = %L', :'one')),
	(7, 'SELECT id FROM sk WHERE note IN (''ord-000777-beta'', ''ord-039000-alpha'', ''none'')'),
	(8, 'SELECT id FROM sk WHERE note LIKE ''%-032%'''),
	(9, 'SELECT id FROM sk WHERE note LIKE ''%0390%-alp%'''),
	(10, 'SELECT id FROM sk WHERE note LIKE ''%zzzz%'''),
	(11, 'SELECT id FROM sk WHERE note LIKE ''%\_alpha%'''),
	(12, 'SELECT id FROM sk WHERE k_hi >= 0'),
	(13, 'SELECT id FROM sk WHERE note LIKE ''%ord%''')
) v(n, q), LATERAL run(v.q) ORDER BY n;

-- The end of the file: three batches, the first two deleted; VACUUM compacts what is left, and the part that is then the last in the file is rewritten into the free space below it, after
-- which the cut takes the free run it leaves
CREATE TABLE tl (id int, p1 bytea, p2 bytea);
CREATE INDEX tl_i ON tl USING sift_col (id, p1, p2) WITH (key_order = 'id');
INSERT INTO tl SELECT i, decode(md5(i::text) || md5(i::text || 'x'), 'hex'), decode(md5(i::text || 'y') || md5(i::text || 'z'), 'hex') FROM generate_series(1, 60000) i;
INSERT INTO tl SELECT i, decode(md5(i::text) || md5(i::text || 'x'), 'hex'), decode(md5(i::text || 'y') || md5(i::text || 'z'), 'hex') FROM generate_series(60001, 120000) i;
INSERT INTO tl SELECT i, decode(md5(i::text) || md5(i::text || 'x'), 'hex'), decode(md5(i::text || 'y') || md5(i::text || 'z'), 'hex') FROM generate_series(120001, 180000) i;
SELECT pg_relation_size('tl_i') / 8192 AS full \gset
DELETE FROM tl WHERE id <= 120000;
-- the extents a VACUUM released are reused once a later transaction id exists, which an idle server has not handed out yet
VACUUM tl;
SELECT pg_current_xact_id() AS xid_after_first \gset
VACUUM tl;
SELECT pg_current_xact_id() AS xid_after_second \gset
SELECT pg_relation_size('tl_i') / 8192 AS pinned \gset
SELECT :pinned > :full AS tail_pinned;
SELECT blocks_truncated > 0 AS cut, NOT lock_skipped AS unlocked, file_blocks < :pinned * 0.85 AS shrunk, file_blocks <= :full + 16 AS back_to_live_size,
	   file_blocks = high_water AS file_ends_at_high_water FROM sift._test_f2_tail_shrink('tl_i');
SELECT sift._test_f2_check('tl_i') ~ '^parts=1 ' AS consistent;
SELECT f2_same('SELECT id FROM tl WHERE id > 100') AS live_rows, f2_same('SELECT id FROM tl WHERE id BETWEEN 120001 AND 120100') AS first_live, f2_same('SELECT id FROM tl WHERE id <= 120000') AS none_dead;

DROP TABLE cp, sk, tl;
DROP FUNCTION run(text), scan_stats(), f2_same(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
