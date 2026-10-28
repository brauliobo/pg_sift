-- The executor tests the strings of a column dictionary with the pattern (LIKE with a _, a second % or ILIKE, a regular expression). The literals every string
-- that matches holds are found first by the substring kernel and the pattern is run on the strings that hold them: the rows are the normal plan's for every pattern,
-- collation and storage, and the literals are those each pattern requires (sift._test_pattern_lits).
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;
\ir test/agg_rows_helpers.sql

-- the literals: of at least 3 bytes, the longest three, the folded ones for ILIKE and ~*; none for what is not read
SELECT op, pat, sift._test_pattern_lits(op, pat) AS lits
FROM (VALUES ('like', '%shop17_example%'), ('like', 'ab%cde_fgh%ij'), ('like', '%a\%bcd%'), ('like', '%ab%'), ('like', '%é%é%'), ('like', '%naïve café%'),
	('nlike', '%abc%def%'), ('ilike', '%Hello_World%'), ('nilike', '%ABC%'), ('ilike', '%é%abc%'),
	('~', 'item/[0-9a-f]{3}c9'), ('~', 'abc(de|fg)hij+k*lmn'), ('~', 'abc|def'), ('~', '^abcd$'), ('~', 'ab*cde'), ('~', 'abc{0}def'), ('~', 'abc{2}def'), ('~', 'abc{2,}def'), ('~', 'abc?def'),
	('~', 'abc+def'), ('~', 'ab\.cd\.ef'), ('~', 'abc\ddef'), ('~', 'abc\x41def'), ('~', 'abc\1def'), ('~', '(?i)abcdef'), ('~', '(?:abc)defg'), ('~', '***:abcdef'), ('~', 'abcde(fgh'),
	('~', 'abc[xyz]def'), ('~', 'abc[]xyz]def'), ('~', 'abc[[:alpha:]]def'), ('~', 'abc[xyz'), ('~', 'abc{x}def'), ('~', 'abc.*def'), ('~', 'éclair*'), ('~', 'éclair'), ('~', 'abc(de)*fgh'),
	('!~', 'abcdef'), ('~*', 'HELLO world'), ('~*', 'abc[d]ef'), ('~*', 'éé'), ('!~*', 'ABC'), ('~', '')) v(op, pat) ORDER BY op, pat;

CREATE TABLE pf (id int, s text, k text, c text) WITH (autovacuum_enabled = off);
INSERT INTO pf SELECT i, 'https://shop' || (i % 50) || '.example/item/' || md5((i % 7000)::text), 'k' || (i % 40), 'c' || md5(i::text)
FROM generate_series(1, 20000) i;
INSERT INTO pf VALUES (20001, '', '', ''), (20002, NULL, NULL, NULL), (20003, 'ab', 'ab', 'ab'), (20004, 'a.b', 'a.b', 'a.b'), (20005, 'a$', '^a', 'a$b'),
	(20006, E'back\\slash', E'x\\', 'x\\y'), (20007, 'naïve café', 'café', 'ï'), (20008, 'abab', 'ABAB', 'bab'), (20009, 'Shop1.EXAMPLE', 'Shop1.Example', 'SHOP1'),
	(20010, 'a+b', 'a*b', 'a(b)'), (20011, 'a|b', '[a]', '{1}'), (20012, 'İk', E'Kk', 'ik'), (20013, 'ITEM/KEY İTEM/Kelvin', 'Kelvin', 'İSTANBUL'),
	(20014, 'item/ke_y item/ke%y', 'ke_y', 'ke%y'), (20015, 'hello world', 'Hello World', 'HELLO WORLD'), (20016, 'abcdef', 'ABCDEF', 'abcDEF'),
	(20017, 'ÉCLAIR éclair', 'éclair', 'ÉCLAIR'), (20018, 'abc1def abc2def', 'abcdef', 'a b c');
INSERT INTO pf SELECT 30000 + i, 'ITEM/' || upper(md5(i::text)) || ' Shop' || (i % 9) || '.Example', 'K' || (i % 30), 'İ' || md5(i::text) FROM generate_series(1, 1500) i;
CREATE INDEX pf_idx ON pf USING sift_col (id sift.col_int4_ops, s sift.col_text_ops, k sift.col_text_ops, c sift.col_text_ops);
VACUUM pf;
CREATE TABLE pft (LIKE pf) USING sift_table;
INSERT INTO pft SELECT * FROM pf;
VACUUM pft;

-- {c} is the column
CREATE TABLE pf_pred (p text);
INSERT INTO pf_pred VALUES
	('{c} LIKE ''%shop17_example%'''), ('{c} LIKE ''%shop1_.example%'''), ('{c} LIKE ''%item/%a%b%'''), ('{c} LIKE ''https://shop_%.example/item/%'''), ('{c} LIKE ''%ab%cd%'''),
	('{c} LIKE ''%e_a%'''), ('{c} LIKE ''%a\%bcd%'''), ('{c} LIKE ''%ke\_y%'''), ('{c} LIKE ''%ke_y%'''), ('{c} LIKE ''%ke%y%'''), ('{c} LIKE ''%é%é%'''), ('{c} LIKE ''%naïve_café%'''),
	('{c} NOT LIKE ''%item/%a%b%'''), ('{c} NOT LIKE ''%shop1_.example%'''), ('{c} NOT LIKE ''%ke%y%'''),
	('{c} ILIKE ''%SHOP17_EXAMPLE%'''), ('{c} ILIKE ''%item/%A%B%'''), ('{c} ILIKE ''%Hello_World%'''), ('{c} ILIKE ''%ke%y%'''), ('{c} ILIKE ''%ISTAN%BUL%'''), ('{c} ILIKE ''%kel%vin%'''),
	('{c} ILIKE ''%item/k%y%'''), ('{c} ILIKE ''%ÉCLAIR%'''), ('{c} ILIKE ''%é%abc%'''), ('{c} NOT ILIKE ''%item/a%b%'''), ('{c} NOT ILIKE ''%shop1_.example%'''), ('{c} NOT ILIKE ''%KE%Y%'''),
	('{c} ~ ''item/[0-9a-f]{3}c9'''), ('{c} ~ ''shop17\.example\.com/item/[a-c]'''), ('{c} ~ ''shop1[0-9]\.example'''), ('{c} ~ ''7f3a9c[0-9]'''), ('{c} ~ ''(7f3a9c|e4b2d1)'''),
	('{c} ~ ''abc|def'''), ('{c} ~ ''^abcd'''), ('{c} ~ ''abc$'''), ('{c} ~ ''ab*cde'''), ('{c} ~ ''abc{0}def'''), ('{c} ~ ''abc{2}'''), ('{c} ~ ''abc?def'''), ('{c} ~ ''abc+def'''),
	('{c} ~ ''abc\ddef'''), ('{c} ~ ''abc[0-9]def'''), ('{c} ~ ''abc(1|2)def'''), ('{c} ~ ''(abc)?def'''), ('{c} ~ ''a.c'''), ('{c} ~ ''ab.cd'''), ('{c} ~ ''ke[_%]y'''), ('{c} ~ ''item/.*ke'''),
	('{c} ~ ''^[0-9A-F]{2}-0000'''), ('{c} ~ ''a\.b'''), ('{c} ~ ''éclair*'''), ('{c} ~ ''éclai(r)'''), ('{c} ~ ''(?i)hello'''), ('{c} ~ ''(?:hello) world'''), ('{c} ~ ''hello(?= )'''),
	('{c} ~ ''abc\x41def'''), ('{c} ~ ''(a)\1bc'''), ('{c} ~ ''Shop[0-9]\.Example'''),
	('{c} !~ ''item/[0-9a-f]{3}c9'''), ('{c} !~ ''ab.cd'''), ('{c} !~ ''shop1[0-9]\.example'''), ('{c} !~ ''abc|def'''),
	('{c} ~* ''HELLO world'''), ('{c} ~* ''item/[A-F]{3}'''), ('{c} ~* ''kel[v]in'''), ('{c} ~* ''istan(bul)'''), ('{c} ~* ''ISTANBUL'''), ('{c} ~* ''shop1[0-9]\.EXAMPLE'''), ('{c} ~* ''éclair[x]*'''),
	('{c} !~* ''item/[A-F]{3}'''), ('{c} !~* ''kel[v]in'''), ('{c} !~* ''shop[0-9]\.example'''),
	('{c} LIKE ''%shop17_example%'' AND {c} ~ ''item/[0-9a-f]{3}'''), ('{c} ILIKE ''%item/%'' AND {c} NOT LIKE ''%a%b%'''), ('{c} ~ ''item/[a-f]'' OR {c} LIKE ''%e_a%'''),
	('NOT ({c} ~ ''shop1[0-9]\.example'')'), ('NOT ({c} LIKE ''%ke_y%'')'), ('NOT ({c} ILIKE ''%ke%y%'')');

CREATE FUNCTION pf_run(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	RETURN agg_rows(q, push);
EXCEPTION WHEN OTHERS THEN
	RETURN 'ERROR: ' || SQLERRM;
END $$;

CREATE FUNCTION pf_check(tag text, tbl text, coll text DEFAULT NULL, ps text[] DEFAULT NULL) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r record;
	q text;
	bad int := 0;
	n int := 0;
BEGIN
	FOR r IN SELECT col, replace(p, '{c}', col || coalesce(' COLLATE ' || quote_ident(coll), '')) AS pred FROM pf_pred, (VALUES ('s'), ('k'), ('c')) c(col) WHERE ps IS NULL OR p = ANY (ps) ORDER BY col, p LOOP
		FOREACH q IN ARRAY ARRAY[
			format('SELECT count(*), sum(id), min(id), max(id) FROM %s WHERE %s', tbl, r.pred),
			format('SELECT id FROM %s WHERE %s AND id %% 3 = 0', tbl, r.pred),
			format('SELECT count(*) FROM %s WHERE NOT (%s)', tbl, r.pred)] LOOP
			n := n + 1;
			IF pf_run(q, true) IS DISTINCT FROM pf_run(q, false) THEN
				bad := bad + 1;
				RAISE NOTICE '%: %', tag, q;
			END IF;
		END LOOP;
	END LOOP;
	RETURN tag || ': ' || n || ' statements, ' || bad || ' differ';
END $$;

-- a literal that few strings hold leaves the others untested; one that every string holds (here more than half of the first 256) is left out, the pattern runs on all
SELECT dicts >= 0 AS reset FROM sift._test_prefilter(true);
SELECT count(*) FROM pf WHERE s ~ 'item/c4ca4[0-9]';
SELECT dicts > 0 AS prefiltered, skipped > 8000 AS most_skipped FROM sift._test_prefilter(true);
SELECT count(*) FROM pf WHERE s ~ 'https://shop[0-9]+\.example';
SELECT dicts = 0 AS common_not_prefiltered, skipped = 0 AS none_skipped FROM sift._test_prefilter(true);
SELECT count(*) FROM pf WHERE s ~ 'abc|def';
SELECT dicts = 0 AS alternative_not_prefiltered FROM sift._test_prefilter(true);

SELECT pf_check('sift_col', 'pf');
SELECT pf_check('sift_table', 'pft');
SELECT pf_check('collation C', 'pf', 'C');
SELECT pf_check('collation POSIX', 'pft', 'POSIX');
SELECT EXISTS (SELECT 1 FROM pg_collation WHERE collname = 'und-x-icu') AS icu \gset
\if :icu
SELECT pf_check('ICU und', 'pf', 'und-x-icu');
CREATE COLLATION pf_tr (provider = icu, locale = 'tr-TR');
SELECT pf_check('ICU tr-TR', 'pf', 'pf_tr');
CREATE COLLATION pf_nd (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
SELECT pf_check('nondeterministic', 'pf', 'pf_nd', ARRAY['{c} LIKE ''%shop17_example%''', '{c} ~ ''shop1[0-9]\.example''']);
DROP COLLATION pf_tr, pf_nd;
\endif
-- a pattern the regex compiler refuses raises its error on the first entry whatever the strings
SELECT pf_run('SELECT count(*) FROM pf WHERE s ~ ''abc(de|fg''', true) = pf_run('SELECT count(*) FROM pf WHERE s ~ ''abc(de|fg''', false) AS error_same,
	pf_run('SELECT count(*) FROM pf WHERE s ~ ''zzzzz[b-a]''', true) = pf_run('SELECT count(*) FROM pf WHERE s ~ ''zzzzz[b-a]''', false) AS range_error_same,
	pf_run('SELECT count(*) FROM pf WHERE s ~ ''zzzzz{2,1}''', true) = pf_run('SELECT count(*) FROM pf WHERE s ~ ''zzzzz{2,1}''', false) AS bound_error_same,
	pf_run('SELECT count(*) FROM pf WHERE s ~ ''zzzzz{2,1}''', true) LIKE 'ERROR:%' AS raises;

INSERT INTO pf SELECT 50000 + i, 'https://shop' || (i % 50) || '.example/item/' || md5(i::text), 'k' || (i % 40), 'c' || i FROM generate_series(1, 4000) i;
DELETE FROM pf WHERE id % 11 = 0;
VACUUM pf;
SELECT pf_check('vacuumed', 'pf');
SET sift.simd = scalar;
SELECT pf_check('scalar', 'pf');
RESET sift.simd;

DROP FUNCTION pf_check(text, text, text, text[]), pf_run(text, bool);
DROP TABLE pf_pred, pf, pft;
DROP FUNCTION agg_pushed(text), agg_rows(text, bool);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
