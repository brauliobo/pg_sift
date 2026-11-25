-- Rewrites (VACUUM FULL, CLUSTER) write the kept rows straight into column segments: CLUSTER in the order of a btree index by a sort,
-- of another index by an index scan; VACUUM FULL over column segments of two layouts; and in the transaction that deleted or updated
-- rows, which stay deleted.
CREATE EXTENSION pg_sift;
-- a rewrite in the transaction that deleted and updated rows: they stay deleted, with or without the commit
CREATE TABLE c (id int PRIMARY KEY, v text) USING sift_table;
INSERT INTO c SELECT g, 'v' || g FROM generate_series(1, 10) g;
BEGIN;
DELETE FROM c WHERE id <= 3;
UPDATE c SET v = 'updated' WHERE id = 4;
INSERT INTO c VALUES (11, 'inserted');
CLUSTER c USING c_pkey;
SELECT * FROM c ORDER BY id;
ROLLBACK;
SELECT count(*) FROM c;
BEGIN;
DELETE FROM c WHERE id <= 3;
UPDATE c SET v = 'updated' WHERE id = 4;
CLUSTER c USING c_pkey;
COMMIT;
SELECT * FROM c ORDER BY id;
SELECT kind, rows FROM sift.table_segments('c') WHERE kind <> 'tombstones';
DROP TABLE c;

-- VACUUM FULL over column segments of two layouts (ADD COLUMN): read by attribute, written in the current one
CREATE TABLE f (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO f SELECT g, 'v' || g FROM generate_series(1, 1000) g;
VACUUM f;
ALTER TABLE f ADD COLUMN w int DEFAULT 5;
INSERT INTO f SELECT g, 'w' || g, g FROM generate_series(1001, 1500) g;
VACUUM f;
DELETE FROM f WHERE id % 4 = 0;
VACUUM FULL f;
SELECT count(*), sum(id) = (SELECT sum(g) FROM generate_series(1, 1500) g WHERE g % 4 <> 0) AS ids,
	sum(w) = 750 * 5 + (SELECT sum(g) FROM generate_series(1001, 1500) g WHERE g % 4 <> 0) AS ws, count(DISTINCT v) FROM f;
SELECT kind, rows FROM sift.table_segments('f') WHERE kind <> 'tombstones';
DROP TABLE f;

-- CLUSTER by a btree index (a sort) writes the rows in its order; by a GiST index (an index scan) keeps them all
CREATE TABLE o (id int, p point, v text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO o SELECT g, point((g * 7919) % 1000, g % 17), 'v' || ((g * 7919) % 3001) FROM generate_series(1, 3000) g;
VACUUM o;
DELETE FROM o WHERE id % 3 = 0;
CREATE INDEX o_v ON o (v);
CLUSTER o USING o_v;
SELECT count(*), bool_and(ordered) AS ordered FROM (SELECT v >= lag(v) OVER (ORDER BY ctid) IS NOT FALSE AS ordered FROM o) x;
CREATE INDEX o_p ON o USING gist (p);
CLUSTER o USING o_p;
SELECT count(*), sum(id), count(DISTINCT v) FROM o;
SET enable_seqscan = off;
SELECT id, v FROM o WHERE v = 'v' || ((1000 * 7919) % 3001);
RESET enable_seqscan;
DROP TABLE o;
DROP EXTENSION pg_sift;
