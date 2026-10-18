-- Keyword facets: sift.facets() counts per value of a keyword index equal GROUP BY count(*) over the
-- rows the query matches, through updates, deletes, VACUUM, segment maintenance and mutable rows
CREATE EXTENSION pg_sift;

-- testing builds also exercise bitmap (GROUPED) postings for the larger values; the pad spreads the rows over several superblocks
SELECT current_setting('sift.grouped_min_df', true) IS NOT NULL AS testing \gset
\if :testing
SET sift.grouped_min_df = 64;
\endif
CREATE TABLE d (id int PRIMARY KEY, body text, cat text, tag text, hc text, pad text) WITH (autovacuum_enabled = off);
INSERT INTO d
SELECT i, 'w' || i % 10 || ' common x' || i % 7,
	(ARRAY[NULL, '', 'Alpha', 'alpha', 'ALPHA', 'Café', 'cafe', 'CAFÉ', 'gamma delta', 'Beta', 'beta', 'Zeta'])[1 + i % 12],
	't' || i % 5, 'k' || i % 2500, repeat('p', 1000)
FROM generate_series(1, 6000) i;
CREATE INDEX d_body ON d USING sift (body) WITH (mutable_limit = 64);
-- folded: case and accent folding on (the default); raw: values kept byte for byte
CREATE INDEX d_cat ON d USING sift (cat) WITH (analyzer = keyword);
CREATE INDEX d_raw ON d USING sift (cat) WITH (analyzer = keyword, case_folding = false, accent_folding = false, build_segments = 2);
CREATE INDEX d_hc ON d USING sift (hc) WITH (analyzer = keyword, case_folding = false, accent_folding = false, build_segments = 3);
VACUUM ANALYZE d;
SELECT idx, count(*) AS segments, max(sb_hi) AS last_sb
FROM (SELECT 'd_raw' idx, * FROM sift.segments('d_raw') UNION ALL SELECT 'd_hc', * FROM sift.segments('d_hc')) s GROUP BY idx ORDER BY idx;

-- the folded value of a row: the keyword index analyzer's token, NULL when the value has none
CREATE FUNCTION fac_expected(col text, facet regclass, q text, lim int, nulls bool) RETURNS text LANGUAGE plpgsql
	SET sift.enable_scan = off SET sift.enable_count = off SET enable_bitmapscan = off SET enable_indexscan = off AS $$
DECLARE
	r text;
BEGIN
	EXECUTE format($f$
		SELECT string_agg(coalesce(v, '<null>') || '=' || n, ' ' ORDER BY n DESC, v COLLATE "C") FROM (
			SELECT v, count(*) n FROM (SELECT (SELECT t.token FROM sift.tokenize(%I, %L) t) v FROM d WHERE body @@> %L) s
			WHERE %s OR v IS NOT NULL GROUP BY v ORDER BY n DESC, v COLLATE "C" LIMIT %s) g$f$,
		col, facet, q, nulls::text, coalesce(lim::text, 'ALL')) INTO r;
	RETURN coalesce(r, '');
END $$;

CREATE FUNCTION fac_actual(facet regclass, q text, lim int, nulls bool) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce(string_agg(coalesce(value, '<null>') || '=' || count, ' ' ORDER BY ord), '')
	FROM sift.facets('d_body', facet, q, lim, nulls) WITH ORDINALITY f(value, count, ord)
$$;

CREATE FUNCTION fac_all(facet regclass, col text) RETURNS TABLE (q text, lim int, nulls bool, result text) LANGUAGE sql AS $$
	SELECT q, lim, nulls, CASE WHEN fac_actual(facet, q, lim, nulls) = fac_expected(col, facet, q, lim, nulls) THEN 'ok'
		ELSE fac_actual(facet, q, lim, nulls) || ' <> ' || fac_expected(col, facet, q, lim, nulls) END
	FROM (VALUES ('common'), ('w3 OR w4'), ('w3 AND NOT x2'), ('"w3 common"'), ('w*'), ('nothing'), ('x1 x2'), ('/w[12]/ OR x6'))
		AS qs(q),
		(VALUES (NULL::int), (1), (3), (100)) AS ls(lim), (VALUES (false), (true)) AS ns(nulls)
$$;

CREATE FUNCTION fac_summary(stage text) RETURNS TABLE (stage text, facet text, checks bigint, failed bigint) LANGUAGE sql AS $$
	SELECT stage, facet, count(*), count(*) FILTER (WHERE result <> 'ok') FROM (
		SELECT 'd_cat' facet, * FROM fac_all('d_cat', 'cat') UNION ALL
		SELECT 'd_raw', * FROM fac_all('d_raw', 'cat') UNION ALL
		SELECT 'd_hc', * FROM fac_all('d_hc', 'hc')) a GROUP BY facet ORDER BY facet
$$;

-- the analyzer: one folded token per value, none for NULL and empty values
SELECT v, (SELECT t.token FROM sift.tokenize(v, 'd_cat') t) AS folded, (SELECT t.token FROM sift.tokenize(v, 'd_raw') t) AS raw
FROM (VALUES ('Alpha'), ('CAFÉ'), ('gamma delta'), (''), ('  ')) x(v);
SELECT * FROM sift.facets('d_body', 'd_cat', 'common', 10);
SELECT * FROM sift.facets('d_body', 'd_raw', 'common', 20, true);
SELECT * FROM sift.facets('d_body', 'd_cat', 'w3 OR w4', 3);
SELECT * FROM sift.facets('d_body', 'd_cat', 'common', 0);
SELECT * FROM sift.facets('d_body', 'd_cat', 'nothing', NULL, true);
SELECT * FROM sift.facets('d_body', 'd_cat', NULL, 3);

SELECT * FROM fac_summary('built');
SELECT * FROM fac_all('d_cat', 'cat') WHERE result <> 'ok';

-- a value is also a keyword query on its index, folded as the values are
SELECT count(*) FROM d WHERE cat @@> 'ALPHA';
SELECT count(*) FROM d WHERE cat IN ('Alpha', 'alpha', 'ALPHA');
SELECT count(*) FROM d WHERE cat @@> '"gamma delta"';

-- the expansion limit does not apply to values: 2500 distinct ones with a limit of 4
SET sift.max_expansions = 4;
SELECT count(*) AS values, sum(count) AS rows FROM sift.facets('d_body', 'd_hc', 'common', NULL);
SELECT count(DISTINCT hc) AS values, count(*) AS rows FROM d WHERE body @@> 'common';
RESET sift.max_expansions;

-- the same on the scalar kernels
SET sift.simd = 'scalar';
SELECT * FROM fac_summary('scalar');
RESET sift.simd;

-- mutable rows: inserts after the build, in the mutable segment of every index
INSERT INTO d SELECT i, 'w' || i % 10 || ' common x' || i % 7,
	(ARRAY[NULL, '', 'Alpha', 'alpha', 'newcat', 'Café', 'cafe', 'CAFÉ', 'gamma delta', 'Beta', 'beta', 'Zeta'])[1 + i % 12],
	't' || i % 5, 'k' || (i + 100) % 2600, repeat('p', 1000) FROM generate_series(6001, 7000) i;
SELECT * FROM fac_summary('mutable');

-- deleted rows, not vacuumed (dead tuples the index still holds)
DELETE FROM d WHERE id % 9 = 0;
SELECT * FROM fac_summary('deleted');

-- values changed: the old and the new index entries coexist until VACUUM; HOT updates keep the entry
UPDATE d SET cat = 'moved' WHERE id % 13 = 0;
UPDATE d SET hc = 'h' || id % 100 WHERE id % 11 = 0;
UPDATE d SET tag = 'u' WHERE id % 17 = 0;
SELECT * FROM fac_summary('updated');

-- rolled back and uncommitted changes are not seen, a transaction sees its own
BEGIN;
DELETE FROM d WHERE body @@> 'w3';
SELECT * FROM fac_summary('own delete');
ROLLBACK;
SELECT * FROM fac_summary('rolled back');

VACUUM d;
SELECT * FROM fac_summary('vacuumed');

-- freed line pointers get new rows with other values (the old entries are dead in a dirty segment)
SELECT bool_or(dirty) AS dirty FROM sift.segments('d_cat') WHERE kind = 'immutable';
INSERT INTO d SELECT i, 'w' || i % 10 || ' common x' || i % 7, (ARRAY['Zeta', 'reused', 'Alpha'])[1 + i % 3], 'r', 'k' || i, repeat('p', 1000)
	FROM generate_series(7001, 7900) i;
SELECT * FROM fac_summary('reused');

SELECT sift.promote('d_cat') >= 0 AS promoted_cat, sift.promote('d_raw') >= 0 AS promoted_raw,
	sift.promote('d_hc') >= 0 AS promoted_hc, sift.promote('d_body') >= 0 AS promoted_body;
SELECT * FROM fac_summary('promoted');
DELETE FROM d WHERE id % 5 = 1;
VACUUM d;
SELECT sift.merge('d_cat', true) >= 0 AS merged_cat, sift.merge('d_raw', true) >= 0 AS merged_raw,
	sift.merge('d_hc', true) >= 0 AS merged_hc, sift.merge('d_body', true) >= 0 AS merged_body;
SELECT * FROM fac_summary('merged');
SELECT sift.verify('d_cat', true), sift.verify('d_raw', true), sift.verify('d_hc', true);

-- phrase recheck, and a query on a column that is an expression
CREATE TABLE e (id int, a text, b text);
INSERT INTO e SELECT i, 'one two ' || i % 3 || ' three', (ARRAY['x', 'y', NULL])[1 + i % 3] FROM generate_series(1, 300) i;
CREATE INDEX e_a ON e USING sift ((a || ' ' || coalesce(b, '')));
CREATE INDEX e_b ON e USING sift (upper(b)) WITH (analyzer = keyword);
SELECT count(*) FROM e WHERE (a || ' ' || coalesce(b, '')) @@> '"two 1 three"';
SELECT * FROM sift.facets('e_a', 'e_b', '"two 1 three"', 10, true);
SELECT upper(b), count(*) FROM e WHERE (a || ' ' || coalesce(b, '')) @@> '"two 1 three"' GROUP BY 1 ORDER BY 2 DESC, 1;
SELECT * FROM sift.facets('e_a', 'e_b', '"one two" x', 10, true);
SELECT upper(b), count(*) FROM e WHERE (a || ' ' || coalesce(b, '')) @@> '"one two" x' GROUP BY 1 ORDER BY 2 DESC, 1;
DROP TABLE e;

-- options and errors
CREATE INDEX bad1 ON d USING sift (cat) WITH (analyzer = keyword, stemmer = en);
CREATE INDEX bad2 ON d USING sift (cat) WITH (analyzer = keyword, fields = 'a');
CREATE INDEX bad3 ON d USING sift (cat) WITH (analyzer = nope);
SELECT * FROM sift.facets('d_body', 'd_body', 'common');
SELECT * FROM sift.facets('d_cat', 'd_cat', 'common');
SELECT * FROM sift.facets('d_body', 'd_pkey', 'common');
SELECT * FROM sift.facets('d_body', 'd_cat', 'common', -1);
SELECT * FROM sift.facets('d_body', 'd_cat', 'common AND', 1);
SELECT * FROM sift.facets('d_body', 'd_cat', 'NOT common', 1);
CREATE TABLE other (id int, c text);
CREATE INDEX other_c ON other USING sift (c) WITH (analyzer = keyword);
SELECT * FROM sift.facets('d_body', 'other_c', 'common');
CREATE INDEX d_part ON d USING sift (cat) WITH (analyzer = keyword) WHERE id < 100;
SELECT * FROM sift.facets('d_body', 'd_part', 'common');
DROP INDEX d_part;
DROP TABLE other;

-- a value longer than a term is indexed as its start and a hash of all of it: its facet is that term, and no other value's
INSERT INTO d VALUES (9000, 'long', repeat('x', 256), 't', 'k', '');
INSERT INTO d VALUES (9001, 'long', repeat('x', 255), 't', 'k', '');
SELECT length(value), count FROM sift.facets('d_body', 'd_cat', 'long') ORDER BY 1 DESC;
DELETE FROM d WHERE id = 9001;

-- privileges: the table needs SELECT, and row level security is refused
CREATE ROLE fac_user;
GRANT USAGE ON SCHEMA sift TO fac_user;
SET ROLE fac_user;
SELECT * FROM sift.facets('d_body', 'd_cat', 'common', 1);
RESET ROLE;
GRANT SELECT ON d TO fac_user;
SET ROLE fac_user;
SELECT * FROM sift.facets('d_body', 'd_cat', 'common', 1);
RESET ROLE;
ALTER TABLE d ENABLE ROW LEVEL SECURITY;
CREATE POLICY fac_p ON d FOR SELECT USING (id % 2 = 0);
SET ROLE fac_user;
SELECT * FROM sift.facets('d_body', 'd_cat', 'common', 1);
RESET ROLE;
DROP POLICY fac_p ON d;
ALTER TABLE d DISABLE ROW LEVEL SECURITY;
REVOKE SELECT ON d FROM fac_user;
REVOKE USAGE ON SCHEMA sift FROM fac_user;
DROP ROLE fac_user;

DROP TABLE d;
DROP FUNCTION fac_summary(text), fac_all(regclass, text), fac_actual(regclass, text, int, bool),
	fac_expected(text, regclass, text, int, bool);
DROP EXTENSION pg_sift;
