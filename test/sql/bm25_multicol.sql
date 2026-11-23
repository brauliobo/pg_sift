-- A sift index on several columns holds one section for each, named as its column: `col @@> q` searches the section of the column (the index, the
-- seq scan and the recheck agree), sift.sections(a, b) @@> 'a:word b:word' searches by the names of the columns, and INCLUDE columns and UNIQUE work on it.
CREATE EXTENSION pg_sift;

CREATE TABLE mc (id int PRIMARY KEY, title text, body text, n int);
INSERT INTO mc
SELECT i, concat_ws(' ', w[1 + i % 5], w[1 + (i / 5) % 5], CASE WHEN i % 7 = 0 THEN 'rare' END),
       concat_ws(' ', w[1 + (i * 3) % 5], 'x' || i % 11, w[1 + i % 2]), i % 10
FROM generate_series(1, 400) i, LATERAL (SELECT ARRAY['alpha', 'beta', 'gamma', 'delta', 'omega'] w) a;
INSERT INTO mc VALUES (401, NULL, 'alpha', 1), (402, 'alpha', NULL, 2), (403, NULL, NULL, 3), (404, '', '', 4);

CREATE INDEX mc_idx ON mc USING sift (title, body);
VACUUM ANALYZE mc;
SELECT reloptions FROM pg_class WHERE relname = 'mc_idx';

-- the fields are the columns: a qualifier of the index is a column name, and a name that is not one is refused
SELECT sift.parse_query('title:alpha body:(beta gamma)', 'mc_idx'::regclass);
SELECT sift.parse_query('nope:alpha', 'mc_idx'::regclass);

CREATE FUNCTION cnt(pred text, via_index boolean) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	PERFORM set_config('enable_seqscan', (NOT via_index)::text, true);
	PERFORM set_config('enable_bitmapscan', via_index::text, true);
	PERFORM set_config('sift.enable_scan', via_index::text, true);
	EXECUTE 'SELECT count(*) FROM mc WHERE ' || pred INTO n;
	RETURN n;
END $$;

CREATE FUNCTION preds() RETURNS SETOF text LANGUAGE sql AS $$ SELECT p FROM unnest(ARRAY[
	'title @@> ''alpha''', 'body @@> ''alpha''', 'title @@> ''rare''', 'body @@> ''x3''', 'title @@> ''alpha AND beta''', 'title @@> ''alpha OR gamma''',
	'title @@> ''alp*''', 'body @@> ''x1?''', 'body @@> ''alpa~1''', 'title @@> ''alpha NOT beta''', 'title @@> ''"alpha beta"''', 'body @@> ''"x3 alpha"''',
	'title @@> ''/al.*/''', 'title @@> ''alpha NOT /bet.*/''', 'title @@> ''body:alpha''', 'body @@> ''title:x3''', 'title @@> ''alpha^2 OR beta''',
	'title @@> ''alpha'' AND body @@> ''beta''', 'title @@> ''alpha'' AND body @@> ''alpha''', 'title @@> ''beta'' AND body @@> ''x4 -alpha''',
	'title @@> ''gamma'' AND n = 3', 'body @@> ''delta'' AND id < 100', 'title @@> ''alpha'' OR body @@> ''omega''',
	'title @@> ''alpha'' AND NOT body @@> ''alpha''', 'title @@> ''zzz''', 'title @@> ''beta'' AND title @@> ''gamma''']) p $$;

-- every clause answers as the seq scan does, whatever the scan
SELECT pred, cnt(pred, true) AS idx, cnt(pred, false) AS seq FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);
SELECT sum(cnt(pred, true)) AS hits, count(*) AS clauses FROM preds() pred;

-- a clause on a column is answered by the index
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM mc WHERE title @@> 'beta';
EXPLAIN (COSTS OFF) SELECT count(*) FROM mc WHERE title @@> 'beta' AND body @@> 'alpha';
RESET enable_seqscan;

-- sift.sections(a, b) @@> 'a:word b:word' is the index's own scan, by the names of the columns, as the same query on the sections of the rows
CREATE FUNCTION scnt(q text, via_index boolean) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	PERFORM set_config('enable_seqscan', (NOT via_index)::text, true);
	PERFORM set_config('enable_bitmapscan', via_index::text, true);
	PERFORM set_config('sift.enable_scan', via_index::text, true);
	EXECUTE format('SELECT count(*) FROM mc WHERE sift.sections(title, body) @@> %L', q) INTO n;
	RETURN n;
END $$;
SELECT q, scnt(q, true) AS idx, scnt(q, false) AS seq
FROM unnest(ARRAY['title:alpha', 'body:alpha', 'alpha', 'title:alpha body:beta', 'title:(alpha beta) body:x3', 'title:alp* NOT body:alpha', 'title:alpha OR body:omega', 'rare',
                  'body:x1?', 'title:alpha body:delta~1', 'title:"alpha beta"']) q
WHERE scnt(q, true) <> scnt(q, false);
SELECT scnt('title:alpha body:beta', true), scnt('title:beta body:alpha', true), scnt('alpha', true), scnt('title:alpha', true) + scnt('body:alpha', true);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM mc WHERE sift.sections(title, body) @@> 'title:alpha body:beta';
EXPLAIN (COSTS OFF) SELECT id, sift.score(ctid) FROM mc WHERE sift.sections(title, body) @@> 'title:alpha body:beta' ORDER BY sift.score(ctid) DESC, id LIMIT 3;
RESET enable_seqscan;
SELECT id, round(sift.score(ctid)::numeric, 3) AS score FROM mc WHERE sift.sections(title, body) @@> 'title:alpha body:beta' ORDER BY sift.score(ctid) DESC, id LIMIT 3;

-- writes, deletes, promotion, VACUUM and merge keep it in step
INSERT INTO mc VALUES (500, 'alpha beta', 'beta alpha', 5), (501, 'gamma', 'alpha', 6);
DELETE FROM mc WHERE id % 6 = 0;
UPDATE mc SET title = 'omega alpha', body = 'beta' WHERE id IN (1, 2, 3);
SELECT pred FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);
SELECT q FROM unnest(ARRAY['title:alpha body:beta', 'title:omega', 'body:beta']) q WHERE scnt(q, true) <> scnt(q, false);
SELECT sift.promote('mc_idx') >= 0 AS promoted;
VACUUM mc;
SELECT pred FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);
SELECT sift.merge('mc_idx', true) IS NOT NULL AS merged;
SELECT pred FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);
SELECT q FROM unnest(ARRAY['title:alpha body:beta', 'title:omega', 'body:beta']) q WHERE scnt(q, true) <> scnt(q, false);
SELECT sift.verify('mc_idx', true);
REINDEX INDEX mc_idx;
SELECT pred FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);
SELECT sift.verify('mc_idx', true);

-- other options: the fields of fields = '...' name the columns, and an analyzer that is not the default answers sift.sections only
CREATE INDEX mc_named ON mc USING sift (title, body) WITH (fields = 'headline,text');
SELECT sift.parse_query('headline:alpha text:beta', 'mc_named'::regclass);
DROP INDEX mc_named;
CREATE INDEX mc_stem ON mc USING sift (title, body) WITH (stemmer = 'en', prefix_index = 3);
DROP INDEX mc_idx;
SELECT pred FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);
SELECT q FROM unnest(ARRAY['title:alpha body:beta', 'title:omega', 'body:beta', 'title:alp*']) q WHERE scnt(q, true) <> scnt(q, false);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM mc WHERE title @@> 'beta';
EXPLAIN (COSTS OFF) SELECT count(*) FROM mc WHERE sift.sections(title, body) @@> 'title:beta';
RESET enable_seqscan;
DROP INDEX mc_stem;

-- refused: fields that do not name the columns, a keyword analyzer, more columns than fields
CREATE INDEX bad ON mc USING sift (title, body) WITH (fields = 'headline');
CREATE INDEX bad ON mc USING sift (title, body) WITH (analyzer = 'keyword');
CREATE TABLE wide (c1 text, c2 text, c3 text, c4 text, c5 text, c6 text, c7 text, c8 text, c9 text, c10 text, c11 text, c12 text, c13 text, c14 text, c15 text);
CREATE INDEX bad ON wide USING sift (c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14, c15);
CREATE INDEX wide_idx ON wide USING sift (c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14);
INSERT INTO wide VALUES ('one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine', 'ten', 'eleven', 'twelve', 'thirteen', 'fourteen');
SELECT count(*) FROM wide WHERE sift.sections(c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13, c14) @@> 'c14:fourteen c1:one';
SET enable_seqscan = off;
SELECT count(*) FROM wide WHERE c14 @@> 'fourteen';
SELECT count(*) FROM wide WHERE c13 @@> 'fourteen';
RESET enable_seqscan;

-- names that are not identifiers, the same name twice
CREATE TABLE odd ("Mixed Case" text, "mixed case" text, "1st" text, "é" text);
CREATE INDEX odd_idx ON odd USING sift ("Mixed Case", "mixed case", "1st", "é");
INSERT INTO odd VALUES ('aa', 'bb', 'cc', 'dd');
SELECT sift.parse_query('mixed_case:aa f2:bb _1st:cc f4:dd', 'odd_idx'::regclass);

-- INCLUDE columns, as on one column
CREATE INDEX mc_inc ON mc USING sift (title, body) INCLUDE (n, id);
SELECT pred FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);
SELECT sift.promote('mc_inc') >= 0 AS promoted;
SELECT q FROM unnest(ARRAY['title:alpha', 'title:gamma body:beta']) q WHERE scnt(q, true) <> scnt(q, false);
SELECT count(*) FROM mc WHERE sift.sections(title, body) @@> 'title:alpha' AND n = 3;
SET sift.enable_scan = off;
SELECT count(*) FROM mc WHERE sift.sections(title, body) @@> 'title:alpha' AND n = 3;
RESET sift.enable_scan;
DROP INDEX mc_inc;

-- UNIQUE on several columns: a NULL column makes the key distinct
CREATE TABLE mq (id int, a text, b text);
INSERT INTO mq VALUES (1, 'x', 'y'), (2, 'x', 'z'), (3, 'y', 'y'), (4, 'x', NULL), (5, 'x', NULL);
CREATE UNIQUE INDEX mq_idx ON mq USING sift (a, b);
INSERT INTO mq VALUES (6, 'x', 'y');
INSERT INTO mq VALUES (7, 'y', 'x'), (8, 'x', NULL), (9, NULL, NULL);
UPDATE mq SET b = 'y' WHERE id = 2;
UPDATE mq SET a = 'x', b = 'z' WHERE id = 3;
DELETE FROM mq WHERE id = 1;
INSERT INTO mq VALUES (10, 'x', 'y');
SELECT id, a, b FROM mq ORDER BY id;
INSERT INTO mq VALUES (11, 'x', 'y');
SELECT sift.verify('mq_idx', true);
INSERT INTO mq VALUES (1, 'x y', 'z'), (12, 'x', 'y z');
CREATE TABLE mr (id int, a text, b text);
INSERT INTO mr VALUES (1, 'x', 'y'), (2, 'x', 'y');
CREATE UNIQUE INDEX mr_idx ON mr USING sift (a, b);
DROP TABLE mc, mq, mr, wide, odd;
DROP FUNCTION cnt(text, boolean), scnt(text, boolean), preds();
DROP EXTENSION pg_sift;
