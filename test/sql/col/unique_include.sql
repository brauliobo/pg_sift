-- sift_col (format major 2): INCLUDE columns and UNIQUE indexes. An INCLUDE column is stored and returned like a key column and is no key; a unique index refuses a second live row of a key at the
-- build and at each write (23505), waits for the transaction of a row that is not out of doubt, and knows NULLs as PostgreSQL does (distinct unless NULLS NOT DISTINCT).
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET TimeZone = 'UTC';
-- the library of the testing build creates the indexes of format major 1 by default; these are of format major 2, which the release library creates
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

-- ---------------------------------------------------------------------------------------------------------
-- INCLUDE
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE inc (k1 int, k2 text, c1 bigint, c2 text, c3 numeric(10, 2), c4 point, c5 date);
INSERT INTO inc SELECT g % 50, 'k' || (g % 7), g * 10, CASE WHEN g % 11 = 0 THEN NULL ELSE 'c' || g END, g / 4.0, point(g, -g), DATE '2024-01-01' + g
FROM generate_series(1, 2000) g;
CREATE INDEX inc_i ON inc USING sift_col (k1, k2) INCLUDE (c1, c2, c3, c4, c5) WITH (return_columns = 'c2, c4');
SELECT a.attname, a.attnum <= i.indnkeyatts AS is_key, i.indnatts, i.indnkeyatts
FROM pg_index i JOIN pg_attribute a ON a.attrelid = i.indexrelid WHERE i.indexrelid = 'inc_i'::regclass ORDER BY a.attnum;
VACUUM inc;

-- what an index-only scan returns of the INCLUDE columns is what the heap holds
SET enable_seqscan = off; SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT k1, c1, c2, c3, c4, c5 FROM inc WHERE k1 = 7;
CREATE TABLE inc_ios AS SELECT k1, k2, c1, c2, c3, c4::text AS c4, c5 FROM inc WHERE k1 = 7;
RESET enable_seqscan; RESET enable_bitmapscan;
SELECT count(*) AS ios_rows, (SELECT count(*) FROM (SELECT * FROM inc_ios EXCEPT SELECT k1, k2, c1, c2, c3, c4::text, c5 FROM inc WHERE k1 = 7) x) AS extra,
	   (SELECT count(*) FROM (SELECT k1, k2, c1, c2, c3, c4::text, c5 FROM inc WHERE k1 = 7 EXCEPT SELECT * FROM inc_ios) x) AS missing FROM inc_ios;
DROP TABLE inc_ios;

-- the aggregates read the INCLUDE columns of the index (the answers are the heap's)
SELECT k1, count(*), sum(c1), count(c2), sum(c3), min(c5) FROM inc WHERE k1 < 3 GROUP BY k1 ORDER BY k1;
SET sift.enable_agg = off;
SELECT k1, count(*), sum(c1), count(c2), sum(c3), min(c5) FROM inc WHERE k1 < 3 GROUP BY k1 ORDER BY k1;
RESET sift.enable_agg;

-- a write is read back, and a row that leaves is gone
INSERT INTO inc VALUES (7, 'k0', 1, 'new', 1.50, point(1, 1), '2030-01-01');
UPDATE inc SET c2 = 'changed', c4 = point(0, 0) WHERE c1 = 70;
DELETE FROM inc WHERE c1 = 140;
SELECT k1, k2, c1, c2, c3, c4, c5 FROM inc WHERE k1 = 7 AND c1 IN (1, 70, 140, 210) ORDER BY c1;
SELECT count(*), sum(c1) FROM inc WHERE k1 = 7;

-- the options name INCLUDE columns, and a rename follows
CREATE INDEX inc_o ON inc USING sift_col (k1) INCLUDE (c1, c2) WITH (compression = 'zstd:3; c2=lz4', return_columns = 'c2');
ALTER TABLE inc RENAME COLUMN c1 TO c1_renamed;
SELECT a.attname FROM pg_attribute a WHERE a.attrelid = 'inc_o'::regclass AND a.attnum > 0 ORDER BY a.attnum;
CREATE INDEX inc_bad ON inc USING sift_col (k1) INCLUDE (c1_renamed) WITH (return_columns = 'nosuch');
DROP TABLE inc;

-- ---------------------------------------------------------------------------------------------------------
-- UNIQUE: the build
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE u (id int, a int, b text, n numeric, u uuid, ts timestamptz, flag bool, pad text);
INSERT INTO u SELECT g, g % 10, 'b' || g, g, ('00000000-0000-0000-0000-' || lpad(g::text, 12, '0'))::uuid, TIMESTAMPTZ '2024-01-01 00:00:00+00' + g * interval '1 minute', g % 2 = 0, repeat('x', 20)
FROM generate_series(1, 3000) g;
CREATE UNIQUE INDEX u_id ON u USING sift_col (id);
CREATE UNIQUE INDEX u_ab ON u USING sift_col (a, b) INCLUDE (pad);
CREATE UNIQUE INDEX u_n ON u USING sift_col (n);
CREATE UNIQUE INDEX u_u ON u USING sift_col (u);
CREATE UNIQUE INDEX u_ts ON u USING sift_col (ts);
SELECT indexrelid::regclass AS idx, indisunique, indnullsnotdistinct, indnatts, indnkeyatts FROM pg_index WHERE indrelid = 'u'::regclass ORDER BY indexrelid::regclass::text COLLATE "C";

-- a duplicate in the table is refused, whichever column or columns make the key
CREATE TABLE ud AS SELECT g AS id, g % 100 AS a, 'b' || (g % 30) AS b, g::numeric AS n FROM generate_series(1, 1000) g;
CREATE UNIQUE INDEX ud_a ON ud USING sift_col (a);
CREATE UNIQUE INDEX ud_ab ON ud USING sift_col (a, b);
CREATE UNIQUE INDEX ud_id ON ud USING sift_col (id);
UPDATE ud SET n = -1 WHERE id IN (5, 6);
CREATE UNIQUE INDEX ud_n ON ud USING sift_col (n);
UPDATE ud SET n = -1.00 WHERE id = 7;
CREATE UNIQUE INDEX ud_n ON ud USING sift_col (n);
-- the rows a delete took out are no duplicate, and a failed build leaves no index
DELETE FROM ud WHERE id IN (6, 7);
CREATE UNIQUE INDEX ud_n ON ud USING sift_col (n);
SELECT count(*) FROM pg_class WHERE relname IN ('ud_a', 'ud_ab', 'ud_n');
DROP TABLE ud;

-- the key types: what the leaf looks up by equality, and nothing else
CREATE TABLE ut (f8 float8, p point, c char(3), t text COLLATE "C", j jsonb, d date, v varchar(10));
CREATE UNIQUE INDEX ut_f8 ON ut USING sift_col (f8);
CREATE UNIQUE INDEX ut_p ON ut USING sift_col (p);
CREATE UNIQUE INDEX ut_c ON ut USING sift_col (c);
CREATE UNIQUE INDEX ut_t ON ut USING sift_col (t);
CREATE UNIQUE INDEX ut_dv ON ut USING sift_col (d, v) INCLUDE (f8, p, c);
SELECT count(*) AS built FROM pg_class WHERE relname IN ('ut_t', 'ut_dv');
DROP TABLE ut;

-- ---------------------------------------------------------------------------------------------------------
-- UNIQUE: the writes
-- ---------------------------------------------------------------------------------------------------------
INSERT INTO u (id, a, b, n, u, ts) VALUES (1, 100, 'x100', 100000, '10000000-0000-0000-0000-000000000000', '2000-01-01 00:00:00+00');
INSERT INTO u (id, a, b, n, u, ts) VALUES (3001, 3, 'b3', 100001, '10000000-0000-0000-0000-000000000001', '2000-01-01 00:00:01+00');
INSERT INTO u (id, a, b, n, u, ts) VALUES (3001, 100, 'y', 100002, '10000000-0000-0000-0000-000000000002', '2000-01-01 00:00:02+00');
INSERT INTO u (id, a, b, n, u, ts) VALUES (3002, 100, 'z', 7.000, '10000000-0000-0000-0000-000000000003', '2000-01-01 00:00:03+00');
INSERT INTO u (id, a, b, n, u, ts) VALUES (3002, 100, 'z', 100003, '00000000-0000-0000-0000-000000000005', '2000-01-01 00:00:03+00');
INSERT INTO u (id, a, b, n, u, ts) VALUES (3002, 100, 'z', 100003, '10000000-0000-0000-0000-000000000003', '2024-01-01 00:05:00+00');
INSERT INTO u (id, a, b, n, u, ts) VALUES (3002, 100, 'z', 100003, '10000000-0000-0000-0000-000000000003', '2000-01-01 00:00:03+00');
SELECT count(*) FROM u;

-- a duplicate inside one statement, and inside one transaction
INSERT INTO u (id, a, b, n, u, ts) VALUES (4000, 1, 'q1', 4000, '40000000-0000-0000-0000-000000000000', '2001-01-01'), (4000, 1, 'q2', 4001, '40000000-0000-0000-0000-000000000001', '2001-01-02');
BEGIN;
INSERT INTO u (id, a, b, n, u, ts) VALUES (4001, 1, 'q1', 4001, '40000000-0000-0000-0000-000000000001', '2001-01-03');
INSERT INTO u (id, a, b, n, u, ts) VALUES (4001, 1, 'q3', 4002, '40000000-0000-0000-0000-000000000002', '2001-01-04');
ROLLBACK;
SELECT count(*) FROM u WHERE id >= 4000;

-- the rows of a rolled back transaction and of a rolled back savepoint hold no key
BEGIN;
INSERT INTO u (id, a, b, n, u, ts) VALUES (5000, 5, 'r5000', 5000, '50000000-0000-0000-0000-000000000000', '2002-01-01');
SAVEPOINT s1;
INSERT INTO u (id, a, b, n, u, ts) VALUES (5001, 5, 'r5001', 5001, '50000000-0000-0000-0000-000000000001', '2002-01-02');
ROLLBACK TO s1;
INSERT INTO u (id, a, b, n, u, ts) VALUES (5001, 5, 'r5001', 5001, '50000000-0000-0000-0000-000000000001', '2002-01-02');
INSERT INTO u (id, a, b, n, u, ts) VALUES (5000, 5, 'r5002', 5002, '50000000-0000-0000-0000-000000000002', '2002-01-03');
ROLLBACK TO s1;
COMMIT;
SELECT id FROM u WHERE id BETWEEN 5000 AND 5001 ORDER BY id;
BEGIN;
INSERT INTO u (id, a, b, n, u, ts) VALUES (5100, 5, 'r5100', 5100, '51000000-0000-0000-0000-000000000000', '2003-01-01');
ROLLBACK;
INSERT INTO u (id, a, b, n, u, ts) VALUES (5100, 5, 'r5100', 5100, '51000000-0000-0000-0000-000000000000', '2003-01-01');
SELECT id FROM u WHERE id = 5100;

-- UPDATE: a row keeps its key, takes a free one, and cannot take a live one
UPDATE u SET pad = 'same key' WHERE id = 10;
UPDATE u SET id = 6000 WHERE id = 10;
UPDATE u SET id = 11 WHERE id = 6000;
UPDATE u SET id = 12 WHERE id = 6000;
UPDATE u SET id = id + 100000 WHERE id IN (20, 21);
SELECT id FROM u WHERE id IN (6000, 10, 11, 12, 20, 21, 100020, 100021) ORDER BY id;

-- DELETE, then the key again: in another transaction, in the same one, and after a VACUUM
DELETE FROM u WHERE id = 30;
INSERT INTO u (id, a, b, n, u, ts) VALUES (30, 0, 'again', 30, '30000000-0000-0000-0000-000000000000', '2004-01-01');
BEGIN;
DELETE FROM u WHERE id = 31;
INSERT INTO u (id, a, b, n, u, ts) VALUES (31, 1, 'again', 31, '31000000-0000-0000-0000-000000000000', '2004-01-02');
INSERT INTO u (id, a, b, n, u, ts) VALUES (31, 1, 'twice', 32, '32000000-0000-0000-0000-000000000000', '2004-01-03');
COMMIT;
DELETE FROM u WHERE id = 32;
VACUUM u;
INSERT INTO u (id, a, b, n, u, ts) VALUES (32, 2, 'after vacuum', 32, '32000000-0000-0000-0000-000000000001', '2004-01-04');
INSERT INTO u (id, a, b, n, u, ts) VALUES (33, 3, 'dup after vacuum', 33, '33000000-0000-0000-0000-000000000000', '2004-01-05');
SELECT id, b FROM u WHERE id IN (30, 31, 32, 33) ORDER BY id;

-- the index and the heap agree on every key, whatever the plan
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS by_index, count(DISTINCT id) AS distinct_ids FROM u WHERE id >= 0;
RESET enable_seqscan; RESET enable_bitmapscan;
SELECT count(*) AS by_heap, count(DISTINCT id) AS distinct_ids FROM u;

-- ---------------------------------------------------------------------------------------------------------
-- UNIQUE: NULLs
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE un (a int, b text, v int);
CREATE UNIQUE INDEX un_ab ON un USING sift_col (a, b);
INSERT INTO un VALUES (1, 'x', 1), (1, NULL, 2), (1, NULL, 3), (NULL, NULL, 4), (NULL, NULL, 5);
INSERT INTO un VALUES (1, 'x', 6);
SELECT count(*) FROM un;
CREATE TABLE unn (a int, b text, v int);
INSERT INTO unn VALUES (1, NULL, 1), (2, NULL, 2), (NULL, 'x', 3);
CREATE UNIQUE INDEX unn_ab ON unn USING sift_col (a, b) NULLS NOT DISTINCT;
SELECT indnullsnotdistinct FROM pg_index WHERE indexrelid = 'unn_ab'::regclass;
INSERT INTO unn VALUES (1, NULL, 4);
INSERT INTO unn VALUES (NULL, 'x', 5);
INSERT INTO unn VALUES (NULL, NULL, 6), (3, NULL, 7);
INSERT INTO unn VALUES (NULL, NULL, 8);
SELECT count(*) FROM unn;
INSERT INTO unn VALUES (2, NULL, 9);
DELETE FROM unn WHERE a = 2;
INSERT INTO unn VALUES (2, NULL, 10);
CREATE TABLE unb (a int, v int);
INSERT INTO unb VALUES (NULL, 1), (NULL, 2);
CREATE UNIQUE INDEX unb_a ON unb USING sift_col (a) NULLS NOT DISTINCT;
CREATE UNIQUE INDEX unb_a ON unb USING sift_col (a);
DROP TABLE un, unn, unb;

-- ---------------------------------------------------------------------------------------------------------
-- UNIQUE: INSERT ... ON CONFLICT (the operator class names the type: the default class is polymorphic and PostgreSQL asks it for its equality operator)
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE oc (id int, t text, v int);
CREATE UNIQUE INDEX oc_id ON oc USING sift_col (id sift.col_int4_ops);
CREATE UNIQUE INDEX oc_t ON oc USING sift_col (t sift.col_text_ops);
INSERT INTO oc VALUES (1, 'a', 1), (2, 'b', 2);
INSERT INTO oc VALUES (1, 'z', 9) ON CONFLICT DO NOTHING;
INSERT INTO oc VALUES (1, 'z', 9) ON CONFLICT (id) DO NOTHING;
INSERT INTO oc VALUES (5, 'a', 9) ON CONFLICT (t) DO NOTHING;
INSERT INTO oc VALUES (1, 'z', 9) ON CONFLICT (id) DO UPDATE SET v = oc.v + 100;
INSERT INTO oc VALUES (7, 'b', 9) ON CONFLICT (t) DO UPDATE SET v = excluded.v, id = 8;
INSERT INTO oc VALUES (3, 'c', 3), (4, 'd', 4), (1, 'e', 5) ON CONFLICT (id) DO UPDATE SET v = oc.v + 1000;
INSERT INTO oc VALUES (9, 'a', 9);
SELECT id, t, v FROM oc ORDER BY id, t;
INSERT INTO oc VALUES (10, 'k', 1), (11, 'l', 2) ON CONFLICT (id) DO UPDATE SET v = oc.v + excluded.v;
INSERT INTO oc VALUES (10, 'k', 5), (11, 'l', 6) ON CONFLICT (id) DO UPDATE SET v = oc.v + excluded.v;
INSERT INTO oc VALUES (12, 'm', 7) ON CONFLICT (id) DO NOTHING;
INSERT INTO oc VALUES (13, 'k', 7) ON CONFLICT (id) DO NOTHING;
DROP TABLE oc;

-- ---------------------------------------------------------------------------------------------------------
-- UNIQUE: expression and partial indexes
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE ue (id int, name text, live bool);
CREATE UNIQUE INDEX ue_lower ON ue USING sift_col ((lower(name)));
CREATE UNIQUE INDEX ue_live ON ue USING sift_col (id) WHERE live;
INSERT INTO ue VALUES (1, 'Alpha', true), (2, 'beta', true), (1, 'gamma', false), (1, 'delta', false);
INSERT INTO ue VALUES (3, 'ALPHA', true);
INSERT INTO ue VALUES (1, 'epsilon', true);
UPDATE ue SET live = true WHERE name = 'gamma';
UPDATE ue SET name = 'BETA' WHERE id = 1 AND live;
SELECT id, name, live FROM ue ORDER BY name;
DROP TABLE ue;

-- ---------------------------------------------------------------------------------------------------------
-- UNIQUE: after a build of the index in another way, and a table that has rows, merges and a reindex
-- ---------------------------------------------------------------------------------------------------------
CREATE TABLE ur (id int, v text);
INSERT INTO ur SELECT g, 'v' FROM generate_series(1, 20000) g;
CREATE UNIQUE INDEX CONCURRENTLY ur_id ON ur USING sift_col (id);
INSERT INTO ur VALUES (20000, 'dup');
INSERT INTO ur SELECT g, 'more' FROM generate_series(20001, 25000) g;
SELECT sift.maintain('ur_id') IS NOT NULL AS maintained;
INSERT INTO ur VALUES (24999, 'dup after merge');
REINDEX INDEX CONCURRENTLY ur_id;
INSERT INTO ur VALUES (1, 'dup after reindex');
INSERT INTO ur VALUES (25001, 'new');
SELECT count(*) FROM ur;
INSERT INTO ur SELECT g, 'copy' FROM generate_series(25001, 25002) g;
CREATE TABLE ur_dup AS SELECT * FROM ur WHERE id = 7;
INSERT INTO ur SELECT * FROM ur_dup;
SELECT count(*) FROM ur WHERE id = 7;
DROP TABLE ur, ur_dup, u;
DROP EXTENSION pg_sift;
