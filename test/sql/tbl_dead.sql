-- Dead rows of the column groups: VACUUM counts live and dead rows as it does for a heap (reltuples, pg_stat_user_tables), rewrites
-- a column segment alone once more than sift.table_dead_rewrite_pct percent of it is dead.
CREATE EXTENSION pg_sift;
-- (vacuum_truncate off: moving a segment down for the cut would also drop its dead rows)
CREATE TABLE d (id int, v text) USING sift_table WITH (autovacuum_enabled = false, vacuum_truncate = false);
CREATE VIEW dseg AS SELECT rows, dead_rows FROM sift.table_segments('d') WHERE kind = 'columns';
CREATE VIEW dstat AS SELECT c.reltuples, s.n_live_tup, s.n_dead_tup FROM pg_class c JOIN pg_stat_user_tables s ON s.relid = c.oid WHERE c.relname = 'd';
INSERT INTO d SELECT g, 'v' || g FROM generate_series(1, 20000) g;
VACUUM d;
SELECT * FROM dseg;
SELECT * FROM dstat;

-- 30 percent dead: under the default threshold (50) the segment stays, its dead rows are counted out
DELETE FROM d WHERE id % 10 < 3;
SELECT pg_stat_force_next_flush();
VACUUM d;
SELECT * FROM dseg;
SELECT * FROM dstat;

-- a lower threshold rewrites it alone, without its dead rows
SET sift.table_dead_rewrite_pct = 20;
VACUUM d;
SELECT * FROM dseg;
RESET sift.table_dead_rewrite_pct;

-- 100 never rewrites; the default then does
SET sift.table_dead_rewrite_pct = 100;
DELETE FROM d WHERE id % 10 < 9;
VACUUM d;
SELECT * FROM dseg;
RESET sift.table_dead_rewrite_pct;
VACUUM d;
SELECT * FROM dseg;
SELECT * FROM dstat;
SELECT count(*), sum(id) FROM d;

-- every row deleted: the segment goes
DELETE FROM d;
VACUUM d;
SELECT count(*) AS segments FROM dseg;
SELECT * FROM dstat;
SHOW sift.table_dead_rewrite_pct;
SET sift.table_dead_rewrite_pct = 0;

DROP VIEW dseg, dstat;
DROP TABLE d;
DROP EXTENSION pg_sift;
