-- dead_rewrite_pct of a sift_col index (default 0): VACUUM rewrites a lone segment when the scans of its dirty units cost more than the rewrite
-- (it re-encodes the live rows: more for the 55 bytes a row of dw than for the 3 of dn), and when half of it is dead. Dead rows spread over every unit or kept
-- in the first one make the difference. A percent set by ALTER INDEX is used as given.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
CREATE TABLE dn (id int PRIMARY KEY, kind int, amount int) WITH (autovacuum_enabled = off);
CREATE TABLE dc (id int PRIMARY KEY, kind int, amount int, pad text) WITH (autovacuum_enabled = off);
CREATE TABLE dw (id int PRIMARY KEY, kind int, tags text[]) WITH (autovacuum_enabled = off);
INSERT INTO dn SELECT i, i % 7, i FROM generate_series(1, 200000) i;
INSERT INTO dc SELECT i, i % 7, i, md5(i::text) FROM generate_series(1, 200000) i;
INSERT INTO dw SELECT i, i % 7, ARRAY(SELECT 't' || ((i * 31 + j * 977) % 20000) FROM generate_series(1, 8) j) FROM generate_series(1, 20000) i;
VACUUM (FREEZE, ANALYZE) dn, dc, dw;
SET max_parallel_maintenance_workers = 0;
CREATE INDEX dn_col ON dn USING sift_col (id, kind, amount);
CREATE INDEX dc_col ON dc USING sift_col (id, kind, amount, pad);
CREATE INDEX dw_col ON dw USING sift_col (id, kind, tags);
SELECT index, count(*) AS segments FROM (SELECT 'dn_col' AS index FROM sift.col_segments('dn_col') UNION ALL SELECT 'dc_col' FROM sift.col_segments('dc_col') UNION ALL SELECT 'dw_col' FROM sift.col_segments('dw_col')) s GROUP BY index ORDER BY index;

-- 15% dead: the narrow segment with a dead row in every unit is rewritten (no dead row left), the wide one is not, nor is the narrow one that has them in its first rows
DELETE FROM dc WHERE id <= 30000;
DELETE FROM dn WHERE id % 20 < 3;
DELETE FROM dw WHERE id % 20 < 3;
VACUUM dn, dc, dw;
SELECT index, sum(dead_rows) AS dead_rows FROM (SELECT 'dn_col'::regclass AS index, dead_rows FROM sift.col_segments('dn_col') UNION ALL SELECT 'dc_col', dead_rows FROM sift.col_segments('dc_col') UNION ALL SELECT 'dw_col', dead_rows FROM sift.col_segments('dw_col')) s GROUP BY index ORDER BY index;

-- the wide one with 10 set: rewritten; RESET returns to the bytes of a row
ALTER INDEX dw_col SET (dead_rewrite_pct = 10);
VACUUM dw;
SELECT index, sum(dead_rows) AS dead_rows FROM (SELECT 'dn_col'::regclass AS index, dead_rows FROM sift.col_segments('dn_col') UNION ALL SELECT 'dc_col', dead_rows FROM sift.col_segments('dc_col') UNION ALL SELECT 'dw_col', dead_rows FROM sift.col_segments('dw_col')) s GROUP BY index ORDER BY index;
ALTER INDEX dw_col RESET (dead_rewrite_pct);

-- 65% dead: the wide and the clustered segments are rewritten as well
DELETE FROM dc WHERE id % 20 < 13;
DELETE FROM dn WHERE id % 20 < 13;
DELETE FROM dw WHERE id % 20 < 14;
VACUUM dn, dc, dw;
SELECT index, sum(dead_rows) AS dead_rows FROM (SELECT 'dn_col'::regclass AS index, dead_rows FROM sift.col_segments('dn_col') UNION ALL SELECT 'dc_col', dead_rows FROM sift.col_segments('dc_col') UNION ALL SELECT 'dw_col', dead_rows FROM sift.col_segments('dw_col')) s GROUP BY index ORDER BY index;
SELECT count(*) AS dn_rows FROM dn WHERE kind >= 0;
SELECT count(*) AS dw_rows FROM dw WHERE kind >= 0;
SELECT sift.col_verify('dn_col', true);
SELECT sift.col_verify('dw_col', true);
DROP TABLE dn, dc, dw;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
