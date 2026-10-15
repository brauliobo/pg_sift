-- fields = 'name,city': the indexed text holds sections split at chr(29); unqualified queries see
-- the plain concatenation, name:word and name:(group) only the words of that section. The index,
-- the document-level evaluation (seq scan) and the same query on the column alone must agree.
CREATE EXTENSION sift;

CREATE FUNCTION sections(name text, city text, rest text) RETURNS text LANGUAGE sql IMMUTABLE AS
	$$ SELECT coalesce(name, '') || chr(29) || coalesce(city, '') || chr(29) || coalesce(rest, '') $$;

CREATE TABLE ft (id int PRIMARY KEY, name text, city text, rest text);
INSERT INTO ft
SELECT i, concat_ws(' ', w[1 + i % 7], w[1 + (i / 7) % 7]), w[1 + (i / 3) % 7], concat_ws(' ', w[1 + (i * 5) % 7], 'x' || i % 9, w[1 + i % 3])
FROM generate_series(1, 700) i,
     LATERAL (SELECT ARRAY['alpha', 'alphabet', 'beta', 'bet', 'Über', 'zèbre', 'gamma'] w) a;
INSERT INTO ft VALUES (701, NULL, NULL, NULL), (702, '', '', ''), (703, 'al', 'alpha', 'al'), (704, 'alpha', 'beta', NULL);

-- field names are validated
CREATE INDEX bad ON ft USING sift (sections(name, city, rest)) WITH (fields = 'Name');
CREATE INDEX bad ON ft USING sift (sections(name, city, rest)) WITH (fields = 'a,,b');
CREATE INDEX bad ON ft USING sift (sections(name, city, rest)) WITH (fields = 'a,a');
CREATE INDEX bad ON ft USING sift (sections(name, city, rest)) WITH (fields = 'a1,b2,c3,d4,e5,f6,g7,h8,i9,j10,k11,l12,m13,n14,o15');

-- one index per table: the index decides the analyzer of a seq scan's rewritten qual
CREATE INDEX ft_idx ON ft USING sift (sections(name, city, rest)) WITH (fields = 'name,city');
CREATE TABLE fg (id int PRIMARY KEY, name text, city text, rest text);
INSERT INTO fg SELECT * FROM ft;
CREATE INDEX fg_idx ON fg USING sift (sections(name, city, rest)) WITH (fields = 'name,city', prefix_index = 2);
CREATE TABLE fn (id int PRIMARY KEY, name text, city text, rest text);
INSERT INTO fn SELECT * FROM ft;
CREATE INDEX fn_idx ON fn USING sift (sections(name, city, rest));
VACUUM ANALYZE ft;
VACUUM ANALYZE fg;
VACUUM ANALYZE fn;
SELECT reloptions FROM pg_class WHERE relname IN ('ft_idx', 'fg_idx') ORDER BY relname;
SELECT sift.verify('ft_idx', true), sift.verify('fg_idx', true);

-- canonical queries: qualifiers move to the leaves; without fields name:word is a word, as before
SELECT sift.parse_query('name:alp* AND city:(beta bet) rest', 'ft_idx'::regclass);
SELECT sift.parse_query('name:{alpha beta}^3 OR city:be*', 'ft_idx'::regclass);
SELECT sift.parse_query('city:(name:alpha beta)', 'ft_idx'::regclass);
SELECT sift.parse_query('name:a?p* name:alpha~1', 'ft_idx'::regclass);
SELECT sift.parse_query('name:alpha', 'fn_idx'::regclass);
SELECT sift.parse_query('a\:b name:"x y" 10:30 name: :x name:\a', 'ft_idx'::regclass);
SELECT sift.parse_query('name:alpha');
SELECT sift.parse_query('city:(name:alpha beta)', 'ft_idx'::regclass) =
       sift.parse_query(sift.parse_query('city:(name:alpha beta)', 'ft_idx'::regclass), 'ft_idx'::regclass) AS roundtrip;
SELECT sift.parse_query('nope:alpha', 'ft_idx'::regclass);
SELECT sift.parse_query('nope:(alpha)', 'fn_idx'::regclass);
SELECT sift.parse_query('name:(alpha beta)', 'fn_idx'::regclass);
SELECT sift.parse_query('name:{/al.*/}', 'ft_idx'::regclass);
SELECT sift.parse_query('name:e-mail', 'ft_idx'::regclass);

CREATE FUNCTION fqs() RETURNS SETOF text LANGUAGE sql AS $$ SELECT * FROM unnest(ARRAY[
	'alpha', 'al*', 'a*', 'alph*', 'alpha~1', 'a?p*', 'be*', 'bet', 'z*', 'x3', 'x*', 'ube*', 'gam*']) $$;

-- cnt: through the index; seqcnt: document evaluation; colcnt: the same query on the column alone
CREATE FUNCTION cnt(tbl text, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM %I WHERE sections(name, city, rest) @@> %L', tbl, q) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION seqcnt(tbl text, q text) RETURNS bigint LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_bitmapscan = off SET sift.enable_scan = off SET sift.enable_count = off AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM %I WHERE sections(name, city, rest) @@> %L', tbl, q) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION colcnt(col text, q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM ft WHERE sift.match(%s, %L)', col, q) INTO n;
	RETURN n;
END $$;

-- unqualified: all sections; the plain index on the concatenation is the reference
SELECT q, cnt('ft', q), cnt('fg', q), cnt('fn', q) FROM fqs() q
WHERE cnt('ft', q) <> cnt('fn', q) OR cnt('fg', q) <> cnt('fn', q) OR seqcnt('ft', q) <> cnt('fn', q) OR seqcnt('fg', q) <> cnt('fn', q);
-- qualified: only that section
SELECT f, q, cnt('ft', f || ':' || q) AS idx, cnt('fg', f || ':' || q) AS gram, seqcnt('ft', f || ':' || q) AS seq, colcnt(f, q) AS col
FROM fqs() q, unnest(ARRAY['name', 'city']) f
WHERE cnt('ft', f || ':' || q) <> colcnt(f, q) OR cnt('fg', f || ':' || q) <> colcnt(f, q) OR seqcnt('ft', f || ':' || q) <> colcnt(f, q)
   OR seqcnt('fg', f || ':' || q) <> colcnt(f, q);
SELECT count(*) AS queries, sum(cnt('ft', 'name:' || q)) AS name_hits, sum(cnt('ft', 'city:' || q)) AS city_hits FROM fqs() q;
-- the rest section is not a field
SELECT cnt('ft', 'x3'), cnt('ft', 'name:x3'), cnt('ft', 'city:x3');
-- groups, boosts, negation
SELECT q, cnt('ft', q), seqcnt('ft', q) FROM unnest(ARRAY[
	'name:(alpha beta)', 'name:{alpha beta}', 'name:alpha AND city:beta', 'name:alp* AND NOT city:alp*', 'name:alpha NOT city:alpha',
	'city:(name:alpha beta)', 'name:alpha OR x3', '(name:alpha OR city:beta) AND bet', 'name:{alp* bet*}^2']) q
WHERE cnt('ft', q) <> seqcnt('ft', q);
SELECT cnt('ft', 'name:(alpha beta)'), cnt('ft', 'name:{alpha beta}'), cnt('ft', 'name:alpha AND city:beta');

-- bitmap heap scans agree
SET sift.enable_scan = off;
SET sift.enable_count = off;
SELECT f, q FROM fqs() q, unnest(ARRAY['name', 'city']) f WHERE cnt('ft', f || ':' || q) <> colcnt(f, q);
RESET sift.enable_scan;
RESET sift.enable_count;

-- the boost of a field decides the order: equal documents differ in the section holding the word
CREATE TABLE fr (id int PRIMARY KEY, name text, city text, rest text);
INSERT INTO fr SELECT i, 'p' || i, 'q' || i, 'r' || i FROM generate_series(1, 60) i;
INSERT INTO fr VALUES (800, 'gamma', 'x', 'y'), (801, 'x', 'gamma', 'y'), (802, 'x', 'y', 'gamma');
CREATE INDEX fr_idx ON fr USING sift (sections(name, city, rest)) WITH (fields = 'name,city', prefix_index = 3);
SELECT q, (SELECT string_agg(id::text, ' ' ORDER BY s DESC, id)
           FROM (SELECT id, sift.score(ctid) s FROM fr WHERE sections(name, city, rest) @@> q ORDER BY sift.score(ctid) DESC, id LIMIT 5) t) AS ranked
FROM unnest(ARRAY['gamma', 'name:gamma^3 OR gamma', 'city:gamma^3 OR gamma', 'name:gamma', 'gam*', 'name:gam*^3 OR gam*', 'city:gam*^3 OR gam*',
                  'gamma AND (name:gamma^3 OR gamma)', 'name:{gamma x}^2']) q;
DROP TABLE fr;

-- mutable segments, updates, deletes, promotion, VACUUM, merge keep indexes and columns in step
INSERT INTO ft VALUES (800, 'gamma', 'x', 'y'), (801, 'x', 'gamma', 'y'), (802, 'x', 'y', 'gamma');
INSERT INTO fg SELECT * FROM ft WHERE id >= 800;
INSERT INTO fn SELECT * FROM ft WHERE id >= 800;
DELETE FROM ft WHERE id % 7 = 0;
DELETE FROM fg WHERE id % 7 = 0;
DELETE FROM fn WHERE id % 7 = 0;
UPDATE ft SET name = 'alphabet beta', city = 'zèbre' WHERE id = 6;
UPDATE fg SET name = 'alphabet beta', city = 'zèbre' WHERE id = 6;
UPDATE fn SET name = 'alphabet beta', city = 'zèbre' WHERE id = 6;
CREATE FUNCTION bad(tbl text) RETURNS SETOF text LANGUAGE sql AS $$
	SELECT f || ':' || q FROM fqs() q, unnest(ARRAY['name', 'city']) f
	WHERE cnt(tbl, f || ':' || q) <> colcnt(f, q) OR seqcnt(tbl, f || ':' || q) <> colcnt(f, q)
	UNION ALL SELECT q FROM fqs() q WHERE cnt(tbl, q) <> cnt('fn', q) $$;
SELECT * FROM bad('ft');
SELECT * FROM bad('fg');
SELECT sift.promote('ft_idx') IS NOT NULL AS promoted, sift.promote('fg_idx') IS NOT NULL AS promoted_gram;
VACUUM ft;
VACUUM fg;
SELECT * FROM bad('ft');
SELECT * FROM bad('fg');
SELECT sift.merge('ft_idx', true) IS NOT NULL AS merged, sift.merge('fg_idx', true) IS NOT NULL AS merged_gram;
SELECT * FROM bad('ft');
SELECT * FROM bad('fg');
SELECT sift.verify('ft_idx', true), sift.verify('fg_idx', true);

-- the field count is part of the analyzer
ALTER INDEX fn_idx SET (fields = 'name');
SELECT count(*) FROM fn WHERE sections(name, city, rest) @@> 'name:alpha';
REINDEX INDEX fn_idx;
SELECT count(*) > 0 AS name_matches FROM fn WHERE sections(name, city, rest) @@> 'name:alpha';

DROP TABLE ft, fg, fn;
DROP FUNCTION fqs(), cnt(text, text), seqcnt(text, text), colcnt(text, text), bad(text), sections(text, text, text);
DROP EXTENSION sift;
