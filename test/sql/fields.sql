-- fields = 'name,city': the indexed text holds sections split at chr(29); unqualified queries see
-- the plain concatenation, name:word and name:(group) only the words of that section. The index,
-- the document-level evaluation (seq scan) and the same query on the column alone must agree.
CREATE EXTENSION pg_sift;

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

-- write buffers, updates, deletes, promotion, VACUUM, merge keep indexes and columns in step
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

-- a qualified group keeps its qualifier next to groups of the same operator, in front of a NOT and in nested groups
CREATE FUNCTION nrows(pred text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE 'SELECT count(*) FROM ft WHERE ' || pred INTO n;
	RETURN n;
END $$;
SELECT q, cnt('ft', q) = nrows(p) AS idx, seqcnt('ft', q) = nrows(p) AS seq, nrows(p) > 0 AS nonempty FROM (VALUES
	('name:(alpha beta)', $$sift.match(name, 'alpha beta')$$),
	('city:(alpha OR beta)', $$sift.match(city, 'alpha OR beta')$$),
	('name:{alpha beta}', $$sift.match(name, 'alpha OR beta')$$),
	('bet name:(alpha beta)', $$sift.match(name, 'alpha beta') AND sift.match(sections(name, city, rest), 'bet')$$),
	('alpha OR city:(beta OR bet)', $$sift.match(sections(name, city, rest), 'alpha') OR sift.match(city, 'beta OR bet')$$),
	('name:(alpha NOT beta)', $$sift.match(name, 'alpha NOT beta')$$),
	('bet name:(NOT beta)', $$sift.match(sections(name, city, rest), 'bet') AND NOT sift.match(name, 'beta')$$),
	('name:(city:alpha beta)', $$sift.match(city, 'alpha') AND sift.match(name, 'beta')$$),
	('name:(alpha (beta OR bet))', $$sift.match(name, 'alpha (beta OR bet)')$$)) v(q, p);
SELECT sift.parse_query('x name:(alpha beta) city:{bet z*}', 'ft_idx'::regclass);

-- without fields a colon is text, in groups as well: name:(a b) are the words name:a and name:b
SELECT sift.parse_query('x name:(alpha OR beta)^2 y:(z)', 'fn_idx'::regclass);
SELECT sift.parse_query('name:(alpha beta) NOT name:{gamma}');
SELECT sift.match('name alpha, name beta', 'name:(alpha beta)'), sift.match('alpha beta', 'name:(alpha beta)');
SELECT sift.parse_query('name:("alpha beta" gamma)', 'fn_idx'::regclass);

-- one answer for a query on every plan, and the error of an unknown field with its fields
CREATE FUNCTION fcnt(tbl text, qual text, mode text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	PERFORM set_config('enable_indexscan', CASE WHEN mode = 'seq' THEN 'off' ELSE 'on' END, true);
	PERFORM set_config('enable_bitmapscan', CASE WHEN mode IN ('seq', 'scan') THEN 'off' ELSE 'on' END, true);
	PERFORM set_config('sift.enable_scan', CASE WHEN mode IN ('seq', 'bitmap') THEN 'off' ELSE 'on' END, true);
	PERFORM set_config('sift.enable_count', CASE WHEN mode IN ('seq', 'bitmap') THEN 'off' ELSE 'on' END, true);
	EXECUTE format('SELECT count(*) FROM %s WHERE %s', tbl, qual) INTO n;
	RETURN n::text;
EXCEPTION WHEN OTHERS THEN
	RETURN sqlstate || ' ' || sqlerrm;
END $$;
-- the plan's index lines, to see which index serves
CREATE FUNCTION plan_index(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
	r text[] := '{}';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (costs off) ' || q LOOP
		IF l ~ 'Index' THEN r := r || trim(l); END IF;
	END LOOP;
	RETURN array_to_string(r, ' / ');
END $$;
-- fcnt over the seq scan, bitmap and sift scan plans: the same text means one answer
CREATE FUNCTION plans(tbl text, qual text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN count(DISTINCT r) = 1 THEN min(r) ELSE string_agg(m || '=' || r, ' ') END
	FROM (SELECT m, fcnt(tbl, qual, m) r FROM unnest(ARRAY['seq', 'bitmap', 'scan']) m) t $$;

SELECT q, plans('ft', 'sections(name, city, rest) @@> ' || quote_literal(q)) FROM unnest(ARRAY[
	'name:alpha', 'nope:alpha', 'nope:(alpha beta)', 'name:(alpha nope:beta)', 'alpha NOT nope:beta', 'name:{alpha nope:beta}^2']) q;
SELECT q, plans('fn', 'sections(name, city, rest) @@> ' || quote_literal(q)) FROM unnest(ARRAY[
	'name:alpha', 'nope:alpha', 'nope:(alpha beta)', 'name:(alpha OR beta) gamma']) q;
SELECT q, plans('ft', 'name @@> ' || quote_literal(q)) AS no_sift_index_on_the_column FROM unnest(ARRAY['name:alpha', 'name:(alpha beta)']) q;

-- the field count is part of the analyzer
ALTER INDEX fn_idx SET (fields = 'name');
SELECT count(*) FROM fn WHERE sections(name, city, rest) @@> 'name:alpha';
REINDEX INDEX fn_idx;
SELECT count(*) > 0 AS name_matches FROM fn WHERE sections(name, city, rest) @@> 'name:alpha';

-- two indexes on one key: `@@>` means the default analyzer, so the index with fields (the lowest OID
-- here) does not serve it; match_idx names the analyzer
CREATE TABLE fm (id int PRIMARY KEY, name text);
INSERT INTO fm SELECT id, name FROM ft WHERE id < 100;
INSERT INTO fm VALUES (9001, 'name alpha'), (9002, 'alpha'), (9003, 'beta name');
CREATE INDEX fm_fields ON fm USING sift (name) WITH (fields = 'name');
CREATE INDEX fm_plain ON fm USING sift (name);
ANALYZE fm;
SELECT q, plans('fm', 'name @@> ' || quote_literal(q)) FROM unnest(ARRAY['name:alpha', 'nope:alpha', 'name:(alpha OR beta)', 'name alpha']) q;
SELECT q, plans('fm', 'sift.match_idx(name, ' || quote_literal(q) || ', ''fm_fields''::regclass)') FROM unnest(ARRAY['name:alpha', 'name:(alpha OR beta)', 'name alpha']) q;
SELECT plans('fm', 'sift.match_idx(name, ''nope:alpha'', ''fm_fields''::regclass)');
SET enable_seqscan = off;
SET sift.enable_scan = off;
SET sift.enable_count = off;
SELECT plan_index('SELECT count(*) FROM fm WHERE name @@> ''name:alpha''');
SELECT plan_index('SELECT count(*) FROM fm WHERE sift.match_idx(name, ''name:alpha'', ''fm_fields''::regclass)');
RESET sift.enable_scan;
RESET sift.enable_count;
SET enable_bitmapscan = off;
SELECT plan_index('SELECT count(*) FROM fm WHERE name @@> ''name:alpha''');
SELECT plan_index('SELECT count(*) FROM fm WHERE sift.match_idx(name, ''name:alpha'', ''fm_fields''::regclass)');
RESET enable_seqscan;
RESET enable_bitmapscan;
-- with the default analyzer's index gone, the index with fields is what `@@>` means
DROP INDEX fm_plain;
SELECT q, plans('fm', 'name @@> ' || quote_literal(q)) FROM unnest(ARRAY['name:alpha', 'nope:alpha', 'name:(alpha OR beta)', 'name alpha']) q;
DROP TABLE fm;

-- partitions bind `@@>` to their own indexes, and an unknown field fails only the partitions that are read
CREATE TABLE fp (id int, name text) PARTITION BY RANGE (id);
CREATE TABLE fp1 PARTITION OF fp FOR VALUES FROM (0) TO (1000);
CREATE TABLE fp2 PARTITION OF fp FOR VALUES FROM (1000) TO (2000);
CREATE TABLE fp3 PARTITION OF fp FOR VALUES FROM (2000) TO (3000);
INSERT INTO fp SELECT i + k * 1000, n FROM generate_series(0, 2) k, (VALUES (1, 'name alpha'), (2, 'alpha'), (3, 'beta')) v(i, n);
CREATE INDEX fp1_idx ON fp1 USING sift (name) WITH (fields = 'name');
CREATE INDEX fp2_idx ON fp2 USING sift (name);
ANALYZE fp;
SELECT 'fp1' AS rel, plans('fp1', $$name @@> 'name:alpha'$$) UNION ALL SELECT 'fp2', plans('fp2', $$name @@> 'name:alpha'$$)
UNION ALL SELECT 'fp3', plans('fp3', $$name @@> 'name:alpha'$$) UNION ALL SELECT 'fp', plans('fp', $$name @@> 'name:alpha'$$)
UNION ALL SELECT 'fp nope', plans('fp', $$name @@> 'nope:alpha'$$) UNION ALL SELECT 'fp2 nope', plans('fp2', $$name @@> 'nope:alpha'$$)
UNION ALL SELECT 'fp3 nope', plans('fp3', $$name @@> 'nope:alpha'$$)
UNION ALL SELECT 'fp nope pruned', plans('fp', $$id >= 1000 AND name @@> 'nope:alpha'$$);
DROP TABLE fp;

-- an index on the parent gives every partition its fields
CREATE TABLE fq (id int, name text) PARTITION BY RANGE (id);
CREATE TABLE fq1 PARTITION OF fq FOR VALUES FROM (0) TO (1000);
CREATE TABLE fq2 PARTITION OF fq FOR VALUES FROM (1000) TO (2000);
INSERT INTO fq SELECT i + k * 1000, n FROM generate_series(0, 1) k, (VALUES (1, 'name alpha'), (2, 'alpha'), (3, 'beta')) v(i, n);
CREATE INDEX fq_idx ON fq USING sift (name) WITH (fields = 'name');
ANALYZE fq;
SELECT q, plans('fq', 'name @@> ' || quote_literal(q)) FROM unnest(ARRAY['name:alpha', 'nope:alpha', 'name alpha']) q;
SET enable_seqscan = off;
SET sift.enable_scan = off;
SET sift.enable_count = off;
SELECT plan_index('SELECT count(*) FROM fq WHERE name @@> ''name:alpha''');
RESET sift.enable_scan;
RESET sift.enable_count;
SET enable_bitmapscan = off;
SELECT plan_index('SELECT count(*) FROM fq WHERE name @@> ''name:alpha''');
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE fq;

-- prepared statements and generic plans: the analyzer is bound when planning, the query is read when executing
SET plan_cache_mode = force_generic_plan;
PREPARE gf(text) AS SELECT count(*) FROM ft WHERE sections(name, city, rest) @@> $1;
EXECUTE gf('name:alpha');
EXECUTE gf('city:alpha');
EXECUTE gf('nope:alpha');
EXECUTE gf('name:alpha');
CREATE TABLE fz AS SELECT * FROM ft;
CREATE INDEX fz_idx ON fz USING sift (sections(name, city, rest));
ANALYZE fz;
PREPARE gz(text) AS SELECT count(*) FROM fz WHERE sections(name, city, rest) @@> $1;
EXECUTE gz('name:alpha');
EXECUTE gz('nope:(alpha beta)');
EXECUTE gz('alpha');
DEALLOCATE ALL;
RESET plan_cache_mode;

-- sift.match without an index reads the colon as text; match_idx reads it as the index does
SELECT sift.match('name alpha', 'name:alpha') AS text_colon, sift.match('alpha', 'name:alpha') AS no_field;
SELECT sift.match_idx('alpha' || chr(29) || 'beta', 'name:alpha', 'ft_idx'::regclass) AS in_name,
       sift.match_idx('alpha' || chr(29) || 'beta', 'city:alpha', 'ft_idx'::regclass) AS in_city,
       sift.match_idx('alpha' || chr(29) || 'beta', 'alpha', 'ft_idx'::regclass) AS anywhere;
SELECT sift.match_idx('alpha', 'nope:alpha', 'ft_idx'::regclass);
SELECT sift.match_idx('alpha', 'nope:(alpha beta)', 'ft_idx'::regclass);

DROP TABLE ft, fg, fn, fz;
DROP FUNCTION fqs(), cnt(text, text), seqcnt(text, text), colcnt(text, text), bad(text), nrows(text), fcnt(text, text, text),
	plan_index(text), plans(text, text), sections(text, text, text);
DROP EXTENSION pg_sift;
