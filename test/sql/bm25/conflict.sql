-- INSERT ... ON CONFLICT on a UNIQUE BM25 index: the executor asks the index for the rows that hold the key of the row to insert (amgettuple), the index answers from the
-- key's own posting and the heap says which of them are live. DO NOTHING and DO UPDATE, with and without an inference target, for keys of any type, several columns, rows
-- in the write buffer and in segments, and keys that are free again after a delete or a rollback.
CREATE EXTENSION pg_sift;

CREATE TABLE oc (id int PRIMARY KEY, code text, n int DEFAULT 0);
CREATE UNIQUE INDEX oc_code ON oc USING sift (code);
INSERT INTO oc SELECT i, 'code' || i, 0 FROM generate_series(1, 200) i;

-- no target: every unique index arbitrates; with a target: that index
INSERT INTO oc VALUES (1000, 'code7', 0) ON CONFLICT DO NOTHING;
INSERT INTO oc VALUES (1001, 'code7', 0) ON CONFLICT (code) DO NOTHING;
INSERT INTO oc VALUES (1002, 'fresh', 0) ON CONFLICT (code) DO NOTHING RETURNING id, code;
INSERT INTO oc VALUES (1003, 'fresh', 0) ON CONFLICT (code) DO NOTHING RETURNING id, code;
SELECT count(*) AS rows, count(DISTINCT code) AS keys FROM oc;

-- DO UPDATE changes the row of the key, and its new version holds the key again
INSERT INTO oc VALUES (2000, 'code8', 1) ON CONFLICT (code) DO UPDATE SET n = oc.n + EXCLUDED.n + 10 RETURNING id, code, n;
INSERT INTO oc VALUES (2001, 'code8', 1) ON CONFLICT (code) DO UPDATE SET n = oc.n + EXCLUDED.n + 10 RETURNING id, code, n;
SELECT id, code, n FROM oc WHERE code @@> 'code8';

-- several rows, some in conflict: the new ones are inserted, the others are skipped or updated
INSERT INTO oc SELECT 3000 + i, 'code' || i * 40, 5 FROM generate_series(1, 8) i ON CONFLICT (code) DO NOTHING RETURNING id, code;
INSERT INTO oc SELECT 4000 + i, 'code' || i * 90, 5 FROM generate_series(1, 4) i ON CONFLICT (code) DO UPDATE SET n = oc.n + 100 RETURNING id, code, n;
SELECT count(*) AS rows, count(DISTINCT code) AS keys FROM oc;

-- a key twice in one statement
INSERT INTO oc VALUES (5000, 'twice', 0), (5001, 'twice', 0) ON CONFLICT (code) DO NOTHING RETURNING id;
INSERT INTO oc VALUES (5002, 'again', 0), (5003, 'again', 0) ON CONFLICT (code) DO UPDATE SET n = 1;

-- a plain insert still raises the violation
INSERT INTO oc VALUES (6000, 'code9', 0);

-- the rows of a segment, a vacuum and a promotion
SELECT sift.promote('oc_code') >= 0 AS promoted;
INSERT INTO oc VALUES (7000, 'code10', 0) ON CONFLICT (code) DO NOTHING RETURNING id;
INSERT INTO oc VALUES (7001, 'fresh', 0) ON CONFLICT (code) DO UPDATE SET n = oc.n + 1 RETURNING id, n;
DELETE FROM oc WHERE code = 'code10';
INSERT INTO oc VALUES (7002, 'code10', 0) ON CONFLICT (code) DO NOTHING RETURNING id;
VACUUM oc;
INSERT INTO oc VALUES (7003, 'code10', 0) ON CONFLICT (code) DO NOTHING RETURNING id;
INSERT INTO oc VALUES (7004, 'code11', 0) ON CONFLICT (code) DO NOTHING RETURNING id;
SELECT sift.verify('oc_code', true);

-- a rolled back insert leaves its key free
BEGIN;
INSERT INTO oc VALUES (8000, 'ghost', 0) ON CONFLICT (code) DO NOTHING RETURNING id;
ROLLBACK;
INSERT INTO oc VALUES (8001, 'ghost', 0) ON CONFLICT (code) DO NOTHING RETURNING id;
INSERT INTO oc VALUES (8002, 'ghost', 0) ON CONFLICT (code) DO NOTHING RETURNING id;

-- keys of any type, several columns, NULLs that are distinct and NULLs that are not
CREATE TABLE oc_k (id serial PRIMARY KEY, k int, a text, b int, v text);
CREATE UNIQUE INDEX oc_k_k ON oc_k USING sift (k);
CREATE UNIQUE INDEX oc_k_ab ON oc_k USING sift (a, b);
INSERT INTO oc_k (k, a, b, v) VALUES (1, 'x', 1, 'one') ON CONFLICT DO NOTHING;
INSERT INTO oc_k (k, a, b, v) VALUES (1, 'y', 2, 'dup k') ON CONFLICT (k) DO NOTHING;
INSERT INTO oc_k (k, a, b, v) VALUES (2, 'x', 1, 'dup ab') ON CONFLICT (a, b) DO NOTHING;
INSERT INTO oc_k (k, a, b, v) VALUES (2, 'x', 2, 'two') ON CONFLICT (a, b) DO UPDATE SET v = 'updated' RETURNING k, a, b, v;
INSERT INTO oc_k (k, a, b, v) VALUES (3, 'x', 2, 'conflict on ab') ON CONFLICT (a, b) DO UPDATE SET v = EXCLUDED.v RETURNING k, a, b, v;
INSERT INTO oc_k (k, a, b, v) VALUES (NULL, NULL, NULL, 'n1'), (NULL, NULL, NULL, 'n2') ON CONFLICT DO NOTHING;
SELECT k, a, b, v FROM oc_k ORDER BY id;
CREATE TABLE oc_n (id serial PRIMARY KEY, a text);
CREATE UNIQUE INDEX oc_n_a ON oc_n USING sift (a) NULLS NOT DISTINCT;
INSERT INTO oc_n (a) VALUES (NULL), (NULL), ('v'), ('v') ON CONFLICT (a) DO NOTHING;
SELECT count(*) AS rows, count(a) AS valued FROM oc_n;

-- the operator class equality is not a query operator, and no index scan is planned on a BM25 index
SELECT count(*) FROM oc WHERE code OPERATOR(sift.===) 'code7'::text;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM oc WHERE code @@> 'code7';
RESET enable_seqscan;
RESET enable_bitmapscan;
EXPLAIN (COSTS OFF) SELECT id FROM oc WHERE code @@> 'code7';

SELECT sift.verify('oc_code', true), sift.verify('oc_k_k', true), sift.verify('oc_k_ab', true), sift.verify('oc_n_a', true);
DROP TABLE oc, oc_k, oc_n;
DROP EXTENSION pg_sift;
