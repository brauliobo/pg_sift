-- NOT (a && k), NOT (a @> k) and NOT (a <@ k) of an array column that may be NULL are tests of the index: the rows are those of a IS NOT NULL whose elements
-- fail the test (the test is NULL for a NULL array and NOT of NULL keeps no row; a NULL element never matches, so no other array gives NULL). They run on the
-- index, forced onto it, with the rows of the normal plan, in every state of the index (built, write buffer, promoted, updated, merged, reindexed) and for both
-- layouts of an array column (the lengths and the lists of elements the build chooses; the image with the lists only). NULL and empty arrays, NULL elements,
-- duplicates, multidimensional arrays and a lower bound other than 1 are among the rows.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ec (id int PRIMARY KEY, k int, a int4[], b int8[], t text[]);
CREATE FUNCTION fill(lo int, hi int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO ec SELECT g, g % 7,
		CASE g % 13 WHEN 0 THEN NULL WHEN 1 THEN '{}' WHEN 2 THEN ARRAY[NULL, 3]::int4[] WHEN 3 THEN ARRAY[3, 3, 3] WHEN 4 THEN ARRAY[[1, 2], [3, 4]]::int4[]
			WHEN 5 THEN '[0:2]={5,6,7}'::int4[] WHEN 6 THEN ARRAY[NULL]::int4[] ELSE (ARRAY[g % 10, (g * 3) % 10, (g * 7) % 10])[1:1 + g % 3] END,
		CASE g % 11 WHEN 0 THEN NULL WHEN 1 THEN '{}' WHEN 2 THEN ARRAY[NULL, 3]::int8[] ELSE ARRAY[(g % 10)::int8, 3] END,
		CASE g % 9 WHEN 0 THEN NULL WHEN 1 THEN '{}' WHEN 2 THEN ARRAY['x', NULL] WHEN 3 THEN ARRAY['', 'a'] ELSE ARRAY[(ARRAY['a', 'b', 'c', 'd'])[1 + g % 4], 'zz' || g % 5] END
	FROM generate_series(lo, hi) g
$$;
SELECT fill(1, 4000);
CREATE TABLE ec2 (LIKE ec INCLUDING ALL);
INSERT INTO ec2 SELECT * FROM ec;
CREATE INDEX ec_col ON ec USING sift_col (id, k, a, b, t) WITH (key_cut = k);
CREATE INDEX ec2_col ON ec2 USING sift_col (id, k, a, b, t) WITH (key_cut = k, array_lengths = off, paths = '+a[] int4, +b[] int8, +t[] text');
VACUUM (FREEZE, ANALYZE) ec;
VACUUM (FREEZE, ANALYZE) ec2;

CREATE TABLE shapes (n int, q text);
INSERT INTO shapes VALUES
	(1, 'SELECT count(*) FROM @T@ WHERE NOT (a && ARRAY[3, 7])'),
	(2, 'SELECT count(*) FROM @T@ WHERE NOT (a @> ARRAY[3])'),
	(3, 'SELECT count(*) FROM @T@ WHERE NOT (a @> ARRAY[3, 5])'),
	(4, 'SELECT count(*) FROM @T@ WHERE NOT (a <@ ARRAY[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10])'),
	(5, 'SELECT count(*) FROM @T@ WHERE NOT (ARRAY[3] <@ a)'),
	(6, 'SELECT count(*) FROM @T@ WHERE NOT (a && ''{}'')'),
	(7, 'SELECT count(*) FROM @T@ WHERE NOT (a @> ''{}'')'),
	(8, 'SELECT count(*) FROM @T@ WHERE NOT (a <@ ''{}'')'),
	(9, 'SELECT count(*) FROM @T@ WHERE NOT (a && ARRAY[NULL, 3]::int4[])'),
	(10, 'SELECT count(*) FROM @T@ WHERE NOT (a @> ARRAY[NULL]::int4[])'),
	(11, 'SELECT count(*) FROM @T@ WHERE NOT (a <@ ARRAY[3, NULL]::int4[])'),
	(12, 'SELECT count(*) FROM @T@ WHERE NOT (b && ARRAY[3, 7]::int8[])'),
	(13, 'SELECT count(*) FROM @T@ WHERE NOT (b <@ ARRAY[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]::int8[])'),
	(14, 'SELECT count(*) FROM @T@ WHERE NOT (t && ARRAY[''a'', ''zz1''])'),
	(15, 'SELECT count(*) FROM @T@ WHERE NOT (t @> ARRAY[''''])'),
	(16, 'SELECT count(*) FROM @T@ WHERE NOT (t <@ ARRAY[''a'', ''b'', '''', ''x''])'),
	(17, 'SELECT count(*) FROM @T@ WHERE NOT (t && ARRAY[NULL, ''a''])'),
	(18, 'SELECT count(*) FROM @T@ WHERE NOT (a && ARRAY[3, 7]) AND k = 3'),
	(19, 'SELECT count(*) FROM @T@ WHERE NOT (a && ARRAY[3, 7]) OR k = 1'),
	(20, 'SELECT count(*) FROM @T@ WHERE NOT (a @> ARRAY[3]) AND NOT (b @> ARRAY[4]::int8[])'),
	(21, 'SELECT k, count(*), sum(id) FROM @T@ WHERE NOT (t @> ARRAY[''a'']) GROUP BY k'),
	(22, 'SELECT id FROM @T@ WHERE NOT (a <@ ARRAY[1, 2, 3, 4, 5, 6, 7, 8, 9, 10]) ORDER BY id LIMIT 50'),
	(23, 'SELECT count(*) FROM @T@ WHERE NOT (7 = ANY (a))');

CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT md5(string_agg(x::text, '','' ORDER BY x::text)) FROM (%s) x', q) INTO r;
	RETURN r;
END $$;
-- the scan forced onto the index, and the normal plan (the settings are the function's own)
CREATE FUNCTION rows_scan(q text) RETURNS text LANGUAGE sql SET sift.enable_agg = on SET sift.agg_force = on AS $$ SELECT rows_md5(q) $$;
CREATE FUNCTION rows_plain(q text) RETURNS text LANGUAGE sql SET sift.enable_agg = off AS $$ SELECT rows_md5(q) $$;

-- one row for each state and layout: the shapes that ran on the index, and the ones whose rows differ from the normal plan's
CREATE FUNCTION matrix(st text) RETURNS TABLE (state text, tbl text, shapes int, on_index int, different int) LANGUAGE plpgsql AS $$
DECLARE t text;
BEGIN
	PERFORM set_config('sift.agg_force', 'on', true);
	FOREACH t IN ARRAY ARRAY['ec', 'ec2'] LOOP
		RETURN QUERY SELECT st, t, count(*)::int,
			count(*) FILTER (WHERE (SELECT w.used FROM sift.explain_why(replace(s.q, '@T@', t)) w LIMIT 1))::int,
			count(*) FILTER (WHERE rows_scan(replace(s.q, '@T@', t)) <> rows_plain(replace(s.q, '@T@', t)))::int
			FROM shapes s;
	END LOOP;
END $$;

SELECT * FROM matrix('built');
SELECT fill(4001, 6000);
INSERT INTO ec2 SELECT * FROM ec WHERE id > 4000;
SELECT * FROM matrix('write buffer');
SELECT sift.col_promote('ec_col'), sift.col_promote('ec2_col');
SELECT * FROM matrix('promoted');
UPDATE ec SET a = ARRAY[1, 2] WHERE id % 17 = 0;
UPDATE ec SET a = NULL WHERE id % 19 = 0;
UPDATE ec SET t = ARRAY['a'] WHERE id % 23 = 0;
DELETE FROM ec WHERE id % 29 = 0;
UPDATE ec2 SET a = ARRAY[1, 2] WHERE id % 17 = 0;
UPDATE ec2 SET a = NULL WHERE id % 19 = 0;
UPDATE ec2 SET t = ARRAY['a'] WHERE id % 23 = 0;
DELETE FROM ec2 WHERE id % 29 = 0;
SELECT * FROM matrix('updated');
SELECT sift.col_promote('ec_col'), sift.col_promote('ec2_col');
SELECT sift.col_merge('ec_col', true), sift.col_merge('ec2_col', true);
SELECT * FROM matrix('merged');
REINDEX INDEX ec_col;
REINDEX INDEX ec2_col;
SELECT * FROM matrix('reindexed');

DROP FUNCTION matrix(text);
DROP FUNCTION rows_scan(text), rows_plain(text), rows_md5(text);
DROP FUNCTION fill(int, int);
DROP TABLE shapes, ec, ec2;
DROP EXTENSION pg_sift;
