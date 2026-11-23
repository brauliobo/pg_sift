-- Format major 2: the point directory (SEC_POINT) of a part answers an equality or a short list on a key whose values are nearly all different: a bigint, a uuid, a text, or two columns that are one key
-- together, also in a part that is kept in the order of a key (the lead column of the key has its marks and no directory). A scan reads the granules of the rows it names and no other, and every
-- answer is the heap's, through the build, the writes after it and a text column of a collation that does not compare bytes (which the directory does not answer).
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE FUNCTION point_keys(index regclass) RETURNS text LANGUAGE C AS 'pg_sift', 'sift_test_f2_point_keys';
CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT' END;
END $$;
-- {exact, recheck, zones, read, sparse, ...}: whether every granule the scan of the index read was one a directory named for the rows it asked (so that a scan with no directory, or one that
-- reads a granule for another reason, says false), and whether it read fewer granules than the index has
CREATE FUNCTION granules(idx regclass, q text, OUT parity text, OUT by_directory bool, OUT fewer bool) LANGUAGE plpgsql AS $$
DECLARE read int; named int;
BEGIN
	PERFORM sift._test_f2_scan_counters(true);
	parity := f2_same(q);
	SELECT c[4], c[5] INTO read, named FROM (SELECT sift._test_f2_scan_counters(true) AS c) s;
	by_directory := read = named;
	fewer := read < (SELECT sum(granules) FROM sift.parts(idx));
END $$;
-- The keys the parts that have a directory share: kind:columns:unique of each key that every one of them holds (the others differ from one layout of the parts to another)
CREATE FUNCTION shared_keys(idx regclass) RETURNS TABLE(key text, in_every_part bool) LANGUAGE sql AS $$
	WITH parts AS (SELECT p, part FROM unnest(string_to_array(point_keys(idx), ' | ')) WITH ORDINALITY u(part, p) WHERE part <> '-'),
		 k AS (SELECT p, regexp_replace(k, ':[0-9]+$', '') AS key FROM parts, unnest(string_to_array(part, ' ')) k)
	SELECT key, count(DISTINCT p) = (SELECT count(*) FROM parts) FROM k GROUP BY key HAVING count(DISTINCT p) = (SELECT count(*) FROM parts) ORDER BY key $$;

CREATE COLLATION ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
-- Integers, texts and two columns together, in rows wide enough that the directories are within a quarter of the bytes of a part (twelve floats, which have none)
CREATE TABLE pt (id bigint, ord bigint, t text COLLATE "C", tenant int, seq int, status int, tci text COLLATE ci, note text COLLATE "C", f1 float8, f2 float8, f3 float8, f4 float8, f5 float8, f6 float8, f7 float8, f8 float8, f9 float8, f10 float8, f11 float8, f12 float8);
INSERT INTO pt SELECT i, (i * 7919) % 1000003, 'user-' || md5('t' || i), i % 250, i / 250, i % 5, 'Name' || (i % 3000), 'n' || (i % 7), sin(i * 0.7919 + 1), sin(i * 0.7919 + 2), sin(i * 0.7919 + 3), sin(i * 0.7919 + 4), sin(i * 0.7919 + 5), sin(i * 0.7919 + 6), sin(i * 0.7919 + 7), sin(i * 0.7919 + 8), sin(i * 0.7919 + 9), sin(i * 0.7919 + 10), sin(i * 0.7919 + 11), sin(i * 0.7919 + 12) FROM generate_series(1, 80000) i;
INSERT INTO pt VALUES (80001, NULL, NULL, NULL, 1, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL), (80002, NULL, NULL, 3, NULL, 1, 'NAME1', NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
CREATE INDEX pt_i ON pt USING sift_col (id, ord, t, tenant, seq, status, tci, note, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12) WITH (key_order = 'id');
SELECT sift._test_f2_check('pt_i') ~ '^parts=[0-9]+ ' AS consistent;
SELECT sum(rows) AS rows FROM sift.parts('pt_i');
-- The keys of the parts: 1 integer, 2 uuid, 3 text, 4 several columns, the columns, and whether no two rows of the part hold the same key. The lead column of the key (id) has none, nor a column with
-- few values (status, note, tenant alone, seq alone); a part of fewer than 16384 rows has none. A column of a collation that does not compare bytes (tci) is keyed as well: a scan does not ask it
SELECT * FROM shared_keys('pt_i') WHERE key ~ '^(1:ord:1|3:t:1)$' ORDER BY key;

SELECT ord AS o1 FROM pt WHERE id = 1234 \gset
SELECT ord AS o2 FROM pt WHERE id = 56789 \gset
SELECT ord AS o3 FROM pt WHERE id = 79999 \gset
SELECT t AS t1 FROM pt WHERE id = 2468 \gset
SELECT t AS t2 FROM pt WHERE id = 33333 \gset

SELECT n, q, g.parity, g.by_directory FROM (VALUES
	(1, format('SELECT id FROM pt WHERE ord = %s', :o1)),
	(2, format('SELECT id FROM pt WHERE ord IN (%s, %s, %s, -5)', :o1, :o2, :o3)),
	(3, 'SELECT id FROM pt WHERE ord = 1000002'),
	(4, 'SELECT id, ord FROM pt WHERE ord IS NULL'),
	(5, format('SELECT id FROM pt WHERE t = %L', :'t1')),
	(6, format('SELECT id FROM pt WHERE t IN (%L, %L, ''absent'')', :'t1', :'t2')),
	(7, 'SELECT id FROM pt WHERE t = ''user-absent'''),
	(8, 'SELECT id FROM pt WHERE tenant = 17 AND seq = 100'),
	(9, 'SELECT id FROM pt WHERE tenant IN (17, 18, 19) AND seq IN (100, 101)'),
	(10, 'SELECT id FROM pt WHERE tenant = 17 AND seq = 9999'),
	(11, 'SELECT id FROM pt WHERE tenant = 3 AND seq IS NULL'),
	(12, 'SELECT id FROM pt WHERE id = 5555'),
	(13, 'SELECT id FROM pt WHERE id BETWEEN 100 AND 200'),
	(14, 'SELECT id FROM pt WHERE tenant = 17'),
	(15, 'SELECT id FROM pt WHERE seq = 100'),
	(16, 'SELECT id FROM pt WHERE status = 3 AND note = ''n3'''),
	(17, 'SELECT id FROM pt WHERE tci = ''name17'''),
	(18, 'SELECT id FROM pt WHERE tci IN (''NAME17'', ''name18'')'),
	(19, format('SELECT id FROM pt WHERE ord = %s AND status = 2', :o1)),
	(20, format('SELECT id FROM pt WHERE ord = %s AND t = %L', :o1, :'t1')),
	(21, format('SELECT id FROM pt WHERE ord = %s OR t = %L', :o1, :'t1')),
	(22, format('SELECT count(*), sum(id), max(ord) FROM pt WHERE ord IN (%s, %s)', :o2, :o3))
) v(n, q), granules('pt_i', v.q) g ORDER BY n;
-- A lookup of one value reads fewer granules than the index has
SELECT n, (granules('pt_i', q)).fewer FROM (VALUES (1, format('SELECT id FROM pt WHERE ord = %s', :o1)), (2, format('SELECT id FROM pt WHERE t = %L', :'t1')),
	(3, 'SELECT id FROM pt WHERE tenant = 17 AND seq = 100'), (4, 'SELECT id FROM pt WHERE ord = 1000002')) v(n, q) ORDER BY n;

-- Writes after the build: the rows of the write buffer and the deletes are the heap's too, and the merge of the parts that have the directory joins them without it
INSERT INTO pt SELECT i, (i * 7919) % 1000003, 'user-' || md5('t' || i), i % 250, i / 250, i % 5, 'Name' || (i % 3000), 'n' || (i % 7), sin(i * 0.7919 + 1), sin(i * 0.7919 + 2), sin(i * 0.7919 + 3), sin(i * 0.7919 + 4), sin(i * 0.7919 + 5), sin(i * 0.7919 + 6), sin(i * 0.7919 + 7), sin(i * 0.7919 + 8), sin(i * 0.7919 + 9), sin(i * 0.7919 + 10), sin(i * 0.7919 + 11), sin(i * 0.7919 + 12) FROM generate_series(80003, 80300) i;
UPDATE pt SET ord = 777777, t = 'user-updated' WHERE id IN (1234, 56789, 80100);
DELETE FROM pt WHERE id IN (2468, 33333, 4321);
SELECT n, q, f2_same(q) AS parity FROM (VALUES
	(1, 'SELECT id FROM pt WHERE ord = 777777'),
	(2, format('SELECT id FROM pt WHERE ord IN (%s, %s, %s, 777777)', :o1, :o2, :o3)),
	(3, 'SELECT id FROM pt WHERE t = ''user-updated'''),
	(4, format('SELECT id FROM pt WHERE t IN (%L, %L)', :'t1', :'t2')),
	(5, 'SELECT id FROM pt WHERE tenant = 17 AND seq = 100'),
	(6, 'SELECT id FROM pt WHERE tenant IN (0, 1, 2) AND seq IN (320, 321)'),
	(7, 'SELECT id FROM pt WHERE id IN (80100, 80250, 1234)')
) v(n, q) ORDER BY n;
VACUUM pt;
SELECT sift._test_f2_check('pt_i') ~ '^parts=[0-9]+ ' AS consistent;
SELECT n, q, f2_same(q) AS parity FROM (VALUES
	(1, 'SELECT id FROM pt WHERE ord = 777777'),
	(2, format('SELECT id FROM pt WHERE ord = %s', :o2)),
	(3, 'SELECT id FROM pt WHERE tenant = 17 AND seq = 100'),
	(4, format('SELECT id FROM pt WHERE t IN (%L, %L)', :'t1', :'t2'))
) v(n, q) ORDER BY n;

-- Uuids, in a part kept in the order of two columns
CREATE TABLE pu (grp int, id bigint, u uuid, e int, f1 float8, f2 float8, f3 float8, f4 float8, f5 float8, f6 float8, f7 float8, f8 float8, f9 float8, f10 float8, f11 float8, f12 float8);
INSERT INTO pu SELECT i % 3, i, md5('u' || i)::uuid, i % 11, sin(i * 0.7919 + 1), sin(i * 0.7919 + 2), sin(i * 0.7919 + 3), sin(i * 0.7919 + 4), sin(i * 0.7919 + 5), sin(i * 0.7919 + 6), sin(i * 0.7919 + 7), sin(i * 0.7919 + 8), sin(i * 0.7919 + 9), sin(i * 0.7919 + 10), sin(i * 0.7919 + 11), sin(i * 0.7919 + 12) FROM generate_series(1, 60000) i;
CREATE INDEX pu_i ON pu USING sift_col (grp, id, u, e, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12) WITH (key_order = 'grp, id');
SELECT sift._test_f2_check('pu_i') ~ '^parts=[0-9]+ ' AS consistent;
SELECT * FROM shared_keys('pu_i') ORDER BY key;
SELECT u AS u1 FROM pu WHERE id = 4321 \gset
SELECT u AS u2 FROM pu WHERE id = 50001 \gset
SELECT n, q, g.parity, g.by_directory FROM (VALUES
	(1, format('SELECT id FROM pu WHERE u = %L', :'u1')),
	(2, format('SELECT id FROM pu WHERE u IN (%L, %L)', :'u1', :'u2')),
	(3, 'SELECT id FROM pu WHERE u = ''00000000-0000-0000-0000-000000000000'''),
	(4, 'SELECT id FROM pu WHERE id = 4321'),
	(5, 'SELECT id FROM pu WHERE grp = 1 AND id BETWEEN 100 AND 200'),
	(6, 'SELECT id FROM pu WHERE e = 3')
) v(n, q), granules('pu_i', v.q) g ORDER BY n;
SELECT (granules('pu_i', format('SELECT id FROM pu WHERE u = %L', :'u1'))).fewer AS one_uuid_reads_fewer;

DROP TABLE pt, pu;
DROP COLLATION ci;
DROP FUNCTION shared_keys(regclass), point_keys(regclass), f2_same(text), granules(regclass, text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
