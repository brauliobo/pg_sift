-- CREATE INDEX on a sift_table decodes only the columns the index uses (keys, included columns, expressions, the predicate): the index of a table
-- of column groups answers as the heap twin's does, for keys on late columns, an expression, a partial index and an INCLUDE column
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE t (id int, a int, b text, c text, d numeric(8,2), e timestamp, f text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE h (LIKE t) WITH (autovacuum_enabled = false);
INSERT INTO t SELECT g, g % 100, 'B' || g % 37, 'c' || g % 11, (g % 500) / 7.0, timestamp '2024-01-01' + g * interval '1 minute', CASE WHEN g % 13 = 0 THEN NULL ELSE 'f' || g END
FROM generate_series(1, 30000) g;
INSERT INTO h SELECT * FROM t;
SELECT sift.table_maintain('t') IS NOT NULL AS maintained;
SELECT (SELECT count(*) FROM sift.table_segments('t') WHERE kind = 'columns') > 0 AS in_groups;
CREATE INDEX t_f ON t (f);
CREATE INDEX t_e ON t (e);
CREATE INDEX t_expr ON t (lower(b), a);
CREATE INDEX t_part ON t (a) WHERE c = 'c3' AND d > 20;
CREATE INDEX t_incl ON t (c) INCLUDE (d, f);
CREATE INDEX h_f ON h (f);
CREATE INDEX h_e ON h (e);
CREATE INDEX h_expr ON h (lower(b), a);
CREATE INDEX h_part ON h (a) WHERE c = 'c3' AND d > 20;
CREATE INDEX h_incl ON h (c) INCLUDE (d, f);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT q.name, q.mine = q.heap AS same
FROM (VALUES
  ('late text key',  (SELECT sum(id) FROM t WHERE f = 'f4242'),                                      (SELECT sum(id) FROM h WHERE f = 'f4242')),
  ('nulls',          (SELECT count(*) FROM t WHERE f IS NULL),                                       (SELECT count(*) FROM h WHERE f IS NULL)),
  ('timestamp range',(SELECT sum(id) FROM t WHERE e >= '2024-01-05' AND e < '2024-01-06'),           (SELECT sum(id) FROM h WHERE e >= '2024-01-05' AND e < '2024-01-06')),
  ('expression',     (SELECT sum(id) FROM t WHERE lower(b) = 'b7' AND a = 7),                        (SELECT sum(id) FROM h WHERE lower(b) = 'b7' AND a = 7)),
  ('partial',        (SELECT sum(id) FROM t WHERE c = 'c3' AND d > 20 AND a = 14),                   (SELECT sum(id) FROM h WHERE c = 'c3' AND d > 20 AND a = 14)),
  ('included',       (SELECT sum(d) FROM t WHERE c = 'c5'),                                          (SELECT sum(d) FROM h WHERE c = 'c5'))
) q(name, mine, heap);
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT (SELECT count(*) FROM pg_index WHERE indrelid = 't'::regclass AND indisvalid) = 5 AS five_valid_indexes;
DROP TABLE t, h;
DROP EXTENSION pg_sift;
