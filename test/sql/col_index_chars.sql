-- sift_col over char(n), bpchar (char without a length) and name (docs/COL.md 1): the values read back from the index are the heap's, the aggregate
-- plans over them give the plain plan's results, and the bpchar leaf is never used by a pushed predicate or group.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;

CREATE TABLE ch (id int, g int, c5 char(5), cu bpchar, nm name, t text);
INSERT INTO ch SELECT i, i % 4,
	CASE i % 6 WHEN 0 THEN NULL WHEN 1 THEN 'a' WHEN 2 THEN 'ab ' WHEN 3 THEN 'abcde' ELSE (i % 9)::text END,
	CASE i % 5 WHEN 0 THEN NULL WHEN 1 THEN 'x' WHEN 2 THEN 'x  ' ELSE 'y ' END,
	CASE i % 7 WHEN 0 THEN NULL WHEN 1 THEN 'plain' WHEN 2 THEN repeat('n', 63) WHEN 3 THEN 'ÀB' ELSE 'k' || i % 11 END,
	'v' || i % 3
FROM generate_series(1, 3000) i;
CREATE INDEX ch_col ON ch USING sift_col (id, g, c5, cu, nm, t) WITH (write_buffer_limit = 1024);

-- the values of every leaf against the heap, as the bytes the heap holds (char(n) keeps its blank padding, bpchar its blanks as given)
SELECT count(*) AS c5_differing FROM (SELECT tid, val FROM sift._test_col_read('ch_col', 2, '') EXCEPT SELECT ctid, convert_from(bpcharsend(c5), 'UTF8') FROM ch) z;
SELECT count(*) AS c5_missing FROM (SELECT ctid, convert_from(bpcharsend(c5), 'UTF8') FROM ch EXCEPT SELECT tid, val FROM sift._test_col_read('ch_col', 2, '')) z;
SELECT count(*) AS cu_differing FROM (SELECT tid, val FROM sift._test_col_read('ch_col', 3, '') EXCEPT SELECT ctid, convert_from(bpcharsend(cu), 'UTF8') FROM ch) z;
SELECT count(*) AS nm_differing FROM (SELECT tid, val FROM sift._test_col_read('ch_col', 4, '') EXCEPT SELECT ctid, nm::text FROM ch) z;
SELECT count(*) AS nm_missing FROM (SELECT ctid, nm::text FROM ch EXCEPT SELECT tid, val FROM sift._test_col_read('ch_col', 4, '')) z;
SELECT sift.col_verify('ch_col', true);

CREATE FUNCTION chars_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

CREATE FUNCTION chars_pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

-- char(n) and name are pushed; bpchar is not: equal values may differ in blanks, which the leaf's bytes keep
SELECT chars_pushed('SELECT c5, count(*) FROM ch GROUP BY c5') AS c5_grouped, chars_pushed('SELECT nm, count(*) FROM ch GROUP BY nm') AS nm_grouped,
	chars_pushed('SELECT count(*) FROM ch WHERE nm = ''plain''') AS nm_filtered, chars_pushed('SELECT cu, count(*) FROM ch GROUP BY cu') AS bpchar_grouped;
SELECT cu, count(*) FROM ch GROUP BY cu ORDER BY cu;

CREATE TABLE ch_q (n serial, q text);
INSERT INTO ch_q (q) VALUES
	('SELECT c5, count(*) FROM ch GROUP BY c5'),
	('SELECT cu, count(*) FROM ch GROUP BY cu'),
	('SELECT nm, count(*), sum(id) FROM ch GROUP BY nm'),
	('SELECT g, count(DISTINCT c5), count(DISTINCT nm), count(DISTINCT cu) FROM ch GROUP BY g'),
	('SELECT c5, nm, count(*) FROM ch WHERE g = 1 GROUP BY c5, nm'),
	('SELECT count(*) FROM ch WHERE c5 = ''ab'''),
	('SELECT count(*) FROM ch WHERE cu = ''x'''),
	('SELECT count(*) FROM ch WHERE nm = ''plain'''),
	('SELECT t, count(*) FROM ch WHERE nm IS NOT NULL GROUP BY t'),
	('SELECT min(nm COLLATE "C"), max(c5), count(*) FROM ch');

SELECT n, chars_rows(q, true) = chars_rows(q, false) AS same FROM ch_q ORDER BY n;

-- rows written after the build (the write buffer), changed, deleted and vacuumed
INSERT INTO ch SELECT 3000 + i, i % 4, 'z' || i % 3, 'q', 'late' || i % 5, 't' FROM generate_series(1, 200) i;
UPDATE ch SET c5 = 'upd', nm = 'upd' WHERE id % 10 = 0;
DELETE FROM ch WHERE id % 13 = 0;
VACUUM ch;
SELECT n, chars_rows(q, true) = chars_rows(q, false) AS same FROM ch_q ORDER BY n;
SELECT sift.col_verify('ch_col', true);

DROP TABLE ch, ch_q;
DROP FUNCTION chars_rows, chars_pushed;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
