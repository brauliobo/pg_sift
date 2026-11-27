-- A row a released savepoint locked or inserted, then locked or updated by its parent (S120-04): the end of the transaction gives back the holders of both xids, so that no
-- entry of the row locks outlives it and VACUUM FREEZE clears the xmax of every row. An insert of the savepoint is the transaction's once released: the parent's update of it is recorded.
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
SET client_min_messages = warning;
CREATE TABLE lsp (id int PRIMARY KEY, n int) USING sift_table;
INSERT INTO lsp SELECT g, 0 FROM generate_series(1, 4) g;

BEGIN;
SAVEPOINT s;
SELECT id FROM lsp WHERE id = 1 FOR UPDATE;
RELEASE s;
SELECT id FROM lsp WHERE id = 1 FOR UPDATE;
ROLLBACK;

BEGIN;
SAVEPOINT s;
SELECT id FROM lsp WHERE id = 2 FOR KEY SHARE;
RELEASE s;
UPDATE lsp SET n = 2 WHERE id = 2;
ROLLBACK;

BEGIN;
SAVEPOINT s;
SELECT id FROM lsp WHERE id = 3 FOR SHARE;
RELEASE s;
UPDATE lsp SET n = 3 WHERE id = 3;
COMMIT;

BEGIN;
SAVEPOINT s;
INSERT INTO lsp VALUES (10, 0);
RELEASE s;
UPDATE lsp SET n = 10 WHERE id = 10;
COMMIT;

SELECT id, n FROM lsp ORDER BY id FOR UPDATE NOWAIT;
VACUUM (FREEZE) lsp;
SELECT id, n, xmax = '0'::xid AS no_xmax FROM lsp ORDER BY id;

DROP TABLE lsp;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
