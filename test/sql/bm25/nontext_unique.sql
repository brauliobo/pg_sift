-- A sift index key of any type: the text its output function writes is the document, `col @@> 'token'` reads the column the same way (sift.any_ops);
-- and UNIQUE on a sift index: the exact key of a row, whatever its words, is one posting of the row, which finds the rows that may have it.
CREATE EXTENSION pg_sift;

CREATE TABLE nt (id int PRIMARY KEY, v varchar(30), c char(12), nm name, i4 int, i8 bigint, u uuid, j jsonb, d date, num numeric, tsv tsvector, ts timestamp);
INSERT INTO nt
SELECT i, 'foo bar ' || i % 5, 'tok' || i % 3, 'nm' || i % 4, i, i * 10000000000, md5(i::text)::uuid, jsonb_build_object('k', 'val' || i % 4, 'tags', jsonb_build_array('t' || i % 3, 'common')),
       DATE '2026-01-01' + i % 30, i / 4.0, to_tsvector('simple', 'ts' || i % 6 || ' word'), TIMESTAMP '2026-03-01 00:00' + i * interval '7 hours'
FROM generate_series(1, 300) i;
INSERT INTO nt (id) VALUES (301);
CREATE INDEX nt_v ON nt USING sift (v);
CREATE INDEX nt_c ON nt USING sift (c);
CREATE INDEX nt_nm ON nt USING sift (nm);
CREATE INDEX nt_i4 ON nt USING sift (i4);
CREATE INDEX nt_i8 ON nt USING sift (i8);
CREATE INDEX nt_u ON nt USING sift (u);
CREATE INDEX nt_j ON nt USING sift (j);
CREATE INDEX nt_d ON nt USING sift (d);
CREATE INDEX nt_num ON nt USING sift (num);
CREATE INDEX nt_tsv ON nt USING sift (tsv);
CREATE INDEX nt_ts ON nt USING sift (ts);
VACUUM ANALYZE nt;
SELECT c.relname, o.opcname FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_opclass o ON o.oid = i.indclass[0] WHERE i.indrelid = 'nt'::regclass AND c.relname LIKE 'nt\_%' ORDER BY 1;

CREATE FUNCTION cnt(pred text, via_index boolean) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	PERFORM set_config('enable_seqscan', (NOT via_index)::text, true);
	PERFORM set_config('enable_bitmapscan', via_index::text, true);
	EXECUTE 'SELECT count(*) FROM nt WHERE ' || pred INTO n;
	RETURN n;
END $$;
CREATE FUNCTION preds() RETURNS SETOF text LANGUAGE sql AS $$ SELECT p FROM unnest(ARRAY[
	'v @@> ''bar''', 'v @@> ''foo AND 3''', 'v @@> ''b*''', 'c @@> ''tok1''', 'c @@> ''tok*''', 'nm @@> ''nm2''', 'i4 @@> ''17''', 'i4 @@> ''1*''', 'i8 @@> ''170000000000''',
	'u @@> ''c4ca4238''', 'u @@> ''a*''', 'j @@> ''val1''', 'j @@> ''tags AND t2 NOT val0''', 'j @@> ''"k" AND common''', 'd @@> ''2026-01-05''', 'd @@> ''2026-01-0*''',
	'num @@> ''2.25''', 'num @@> ''1''', 'tsv @@> ''ts3''', 'tsv @@> ''word''', 'ts @@> ''2026-03-02''', 'ts @@> ''2026-03-0?''', 'j @@> ''val1'' AND i4 > 100',
	'j @@> ''val1'' AND v @@> ''bar''', 'i4 @@> ''zzz''']) p $$;
SELECT pred, cnt(pred, true) AS idx, cnt(pred, false) AS seq FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);
SELECT sum(cnt(pred, true)) AS hits, count(*) AS clauses FROM preds() pred;

SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM nt WHERE j @@> 'val1';
EXPLAIN (COSTS OFF) SELECT count(*) FROM nt WHERE i4 @@> '17';
EXPLAIN (COSTS OFF) SELECT count(*) FROM nt WHERE v @@> 'bar';
RESET enable_seqscan;

-- writes, updates and deletes of every type keep the index in step; the build and the inserts write the same document
INSERT INTO nt SELECT 400 + i, 'foo bar 9', 'tok9', 'nm9', 400 + i, 1, md5('x' || i)::uuid, '{"k": "val9"}', DATE '2027-01-01', 9.5, to_tsvector('simple', 'late'), TIMESTAMP '2027-01-01' FROM generate_series(1, 20) i;
DELETE FROM nt WHERE id % 9 = 0;
UPDATE nt SET j = '{"k": "moved"}', i4 = 777 WHERE id IN (1, 2, 3);
SELECT pred FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);
SELECT cnt('j @@> ''moved''', true), cnt('i4 @@> ''777''', true), cnt('v @@> ''9''', true);
SELECT sift.promote('nt_j') >= 0 AND sift.promote('nt_i4') >= 0 AS promoted;
VACUUM nt;
SELECT pred FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);
SELECT sift.verify('nt_v', true), sift.verify('nt_j', true), sift.verify('nt_i4', true), sift.verify('nt_u', true);
REINDEX TABLE nt;
SELECT pred FROM preds() pred WHERE cnt(pred, true) <> cnt(pred, false);

-- the column that has an operator class of its own (text) keeps it: an index on text is not changed by the class of the others
CREATE TABLE tx (id int, t text, vc varchar);
CREATE INDEX tx_t ON tx USING sift (t);
CREATE INDEX tx_vc ON tx USING sift (vc);
SELECT c.relname, o.opcname FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_opclass o ON o.oid = i.indclass[0] WHERE i.indrelid = 'tx'::regclass ORDER BY 1;
SELECT sift.match_any(42, '42'), sift.match_any('{"a": "word"}'::jsonb, 'word'), sift.match_any(DATE '2026-02-03', '2026-02-03'), sift.match_any(NULL::int, 'x');
SELECT 42 @@> '42', 'hello'::char(10) @@> 'hello', '{"a": ["b", "c"]}'::jsonb @@> 'c', ARRAY[1, 2] @@> '2';
DROP TABLE tx;

-- UNIQUE: a key is a row's whole value, the words are not what makes it
CREATE TABLE ut (id int, body text);
INSERT INTO ut VALUES (1, 'hello world'), (2, 'second doc'), (3, ''), (4, NULL), (5, NULL), (6, 'the'), (7, 'a, b'), (8, '!!!');
CREATE UNIQUE INDEX ut_idx ON ut USING sift (body);
SELECT indisunique, indisvalid FROM pg_index WHERE indexrelid = 'ut_idx'::regclass;
INSERT INTO ut VALUES (10, 'hello world');
INSERT INTO ut VALUES (11, '');
INSERT INTO ut VALUES (12, 'the');
INSERT INTO ut VALUES (13, '!!!');
INSERT INTO ut VALUES (14, 'a, b');
-- equal as the words, not as the values: case and spacing differ
INSERT INTO ut VALUES (15, 'Hello World'), (16, 'hello  world'), (17, 'world hello'), (18, 'a b'), (19, 'a,b');
INSERT INTO ut VALUES (20, NULL), (21, NULL);
-- the statement that fails leaves nothing of its rows
INSERT INTO ut VALUES (30, 'fresh'), (31, 'fresh');
SELECT count(*) FROM ut WHERE id IN (30, 31);
INSERT INTO ut VALUES (32, 'fresh');
UPDATE ut SET body = 'hello world' WHERE id = 32;
UPDATE ut SET body = 'second doc' WHERE id = 32;
UPDATE ut SET body = 'renamed' WHERE id = 32;
UPDATE ut SET body = 'fresh' WHERE id = 2;
SELECT id, body FROM ut ORDER BY id;
-- a deleted key is free again, in the same statement or in the transaction of the delete
DELETE FROM ut WHERE id = 32;
INSERT INTO ut VALUES (33, 'renamed');
BEGIN;
DELETE FROM ut WHERE id = 33;
INSERT INTO ut VALUES (34, 'renamed');
INSERT INTO ut VALUES (35, 'renamed');
ROLLBACK;
SELECT id, body FROM ut WHERE id >= 30 ORDER BY id;
-- a rolled back insert leaves no key behind
BEGIN;
INSERT INTO ut VALUES (40, 'never');
ROLLBACK;
INSERT INTO ut VALUES (41, 'never');
-- the key is not a word: wildcards and regular expressions see nothing of it
SELECT count(*) FROM ut WHERE body @@> '/.*/';
SELECT count(*) FROM ut WHERE body @@> 'h*';
SELECT sift.verify('ut_idx', true);
SELECT sift.promote('ut_idx') >= 0 AS promoted;
INSERT INTO ut VALUES (42, 'never');
INSERT INTO ut VALUES (43, 'hello world');
VACUUM ut;
SELECT sift.merge('ut_idx', true) IS NOT NULL AS merged;
INSERT INTO ut VALUES (44, 'second doc');
INSERT INTO ut VALUES (45, 'unseen');
SELECT sift.verify('ut_idx', true);
REINDEX INDEX ut_idx;
INSERT INTO ut VALUES (46, 'unseen');
-- the matches and the ranking of a unique index are those of a plain one
CREATE TABLE up (id int, body text);
INSERT INTO up SELECT id, body FROM ut WHERE body IS NOT NULL;
CREATE INDEX up_idx ON up USING sift (body);
SELECT (SELECT array_agg(id ORDER BY id) FROM ut WHERE body @@> 'hello OR world OR second OR doc') = (SELECT array_agg(id ORDER BY id) FROM up WHERE body @@> 'hello OR world OR second OR doc') AS same_matches;
SELECT (SELECT array_agg(id ORDER BY s DESC, id) FROM (SELECT id, sift.score(ctid) s FROM ut WHERE body @@> 'hello OR second OR world' ORDER BY id) a) =
       (SELECT array_agg(id ORDER BY s DESC, id) FROM (SELECT id, sift.score(ctid) s FROM up WHERE body @@> 'hello OR second OR world' ORDER BY id) b) AS same_ranking;
DROP TABLE up;

-- a duplicate where the index is built, whatever the row: live ones, in the same transaction
CREATE TABLE ud (id int, body text);
INSERT INTO ud VALUES (1, 'same'), (2, 'other'), (3, 'same');
CREATE UNIQUE INDEX ud_idx ON ud USING sift (body);
SELECT count(*) FROM pg_index WHERE indrelid = 'ud'::regclass AND indisunique;
DELETE FROM ud WHERE id = 3;
INSERT INTO ud VALUES (4, 'same');
DELETE FROM ud WHERE id = 4;
INSERT INTO ud VALUES (5, 'same');
CREATE UNIQUE INDEX ud_idx ON ud USING sift (body);
INSERT INTO ud VALUES (6, 'same');
UPDATE ud SET body = 'same' WHERE id = 2;
-- NULLS NOT DISTINCT: NULL is a key
CREATE TABLE un (id int, body text);
INSERT INTO un VALUES (1, NULL), (2, 'x');
CREATE UNIQUE INDEX un_idx ON un USING sift (body) NULLS NOT DISTINCT;
INSERT INTO un VALUES (3, NULL);
INSERT INTO un VALUES (4, 'y'), (5, 'y');
DELETE FROM un WHERE id = 1;
INSERT INTO un VALUES (6, NULL);
-- an expression, another type, a partial index, INCLUDE columns, the index built online
CREATE TABLE ue (id int, body text, num int, tag text);
INSERT INTO ue VALUES (1, 'Hello', 1, 'a'), (2, 'other', 2, 'a'), (3, 'hello', 3, 'b');
CREATE UNIQUE INDEX ue_lower ON ue USING sift (lower(body));
DELETE FROM ue WHERE id = 3;
CREATE UNIQUE INDEX ue_lower ON ue USING sift (lower(body));
INSERT INTO ue VALUES (4, 'HELLO', 4, 'a');
INSERT INTO ue VALUES (5, 'Third', 5, 'a');
DROP INDEX ue_lower;
CREATE UNIQUE INDEX ue_num ON ue USING sift (num);
INSERT INTO ue VALUES (6, 'x', 1, 'a');
INSERT INTO ue VALUES (7, 'x', 7, 'a');
DROP INDEX ue_num;
CREATE UNIQUE INDEX ue_part ON ue USING sift (tag) WHERE num > 5;
INSERT INTO ue VALUES (8, 'y', 8, 'a');
INSERT INTO ue VALUES (9, 'y', 1, 'a');
INSERT INTO ue VALUES (10, 'z', 10, 'b'), (11, 'z', 11, 'b');
DROP INDEX ue_part;
CREATE UNIQUE INDEX ue_inc ON ue USING sift (body) INCLUDE (num, tag);
INSERT INTO ue VALUES (12, 'x', 12, 'c');
INSERT INTO ue VALUES (13, 'new', 13, 'c');
DROP INDEX ue_inc;
CREATE UNIQUE INDEX CONCURRENTLY ue_conc ON ue USING sift (id);
SELECT indisvalid FROM pg_index WHERE indexrelid = 'ue_conc'::regclass;
INSERT INTO ue VALUES (7, 'dup', 14, 'c');
DROP TABLE ut, ud, un, ue;
DROP TABLE nt;
DROP FUNCTION cnt(text, boolean), preds();
DROP EXTENSION pg_sift;
