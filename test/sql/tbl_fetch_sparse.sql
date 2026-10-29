-- Fetching a row of a column group by its TID bisects the page directory of the unit (docs/COL-TAM.md): the rows are found, and the ones deleted,
-- updated or vacuumed away are not, in units with gaps in their pages (a sparse page keeps a bitmap of its offsets), across several superblocks
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET sift.table_inline_promote = on;
CREATE TABLE t (id int PRIMARY KEY, k int, v int, note text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE h (id int PRIMARY KEY, k int, v int, note text) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT g, g % 1000, g, 'n' || g FROM generate_series(1, 200000) g;
INSERT INTO h SELECT * FROM t;
SELECT sift.table_maintain('t') IS NOT NULL AS maintained;
SELECT (SELECT count(*) FROM sift.table_segments('t') WHERE kind = 'columns') > 0 AS in_groups;
-- gaps: every 3rd and every 7th row, then the rewrite that drops them from the pages of the units
DELETE FROM t WHERE id % 3 = 0 OR id % 7 = 0;
DELETE FROM h WHERE id % 3 = 0 OR id % 7 = 0;
VACUUM t, h;
-- changes after the rewrite: an update chain on some rows and a few more deletes
UPDATE t SET v = v + 1 WHERE id % 11 = 1;
UPDATE h SET v = v + 1 WHERE id % 11 = 1;
UPDATE t SET v = v + 1 WHERE id % 11 = 1 AND id % 2 = 1;
UPDATE h SET v = v + 1 WHERE id % 11 = 1 AND id % 2 = 1;
DELETE FROM t WHERE id % 101 = 5;
DELETE FROM h WHERE id % 101 = 5;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT q.name, q.mine = q.heap AS same
FROM (VALUES
  ('rows by id',     (SELECT sum(v)::text FROM t WHERE id BETWEEN 1 AND 200000 AND id % 5 = 0),         (SELECT sum(v)::text FROM h WHERE id BETWEEN 1 AND 200000 AND id % 5 = 0)),
  ('deleted ids',    (SELECT count(*)::text FROM generate_series(1, 200000, 3) g, t WHERE t.id = g),    (SELECT count(*)::text FROM generate_series(1, 200000, 3) g, h WHERE h.id = g)),
  ('updated rows',   (SELECT sum(v)::text FROM generate_series(1, 200000, 11) g, t WHERE t.id = g),     (SELECT sum(v)::text FROM generate_series(1, 200000, 11) g, h WHERE h.id = g)),
  ('first and last', (SELECT string_agg(note, ',' ORDER BY id) FROM t WHERE id IN (1, 2, 4, 199999, 200000)), (SELECT string_agg(note, ',' ORDER BY id) FROM h WHERE id IN (1, 2, 4, 199999, 200000)))
) q(name, mine, heap);
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT (SELECT count(*) FROM t) = (SELECT count(*) FROM h) AS same_rows;
DROP TABLE t, h;
DROP EXTENSION pg_sift;
