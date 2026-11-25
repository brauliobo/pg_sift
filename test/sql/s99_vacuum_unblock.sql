-- Spec 99 WS-99-1: VACUUM is never cut short. sift.vacuum_rewrite_limit never caps VACUUM FREEZE or a VACUUM someone runs, 0 means unlimited in every format, and a VACUUM that
-- someone runs compacts, folds the column-delta chain and cuts the file before it returns, whatever sift.async_maintenance says.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;

CREATE FUNCTION load(tbl regclass, first_id int, n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format($q$INSERT INTO %s SELECT g, g %% 8, md5(g::text) || md5((g * 7)::text) || repeat('seg=' || lpad((g %% 16)::text, 2, '0') || '|pad=0123456789abcdef|', 8)
		FROM generate_series(%s, %s) g$q$, tbl, first_id, first_id + n - 1);
END $$;
CREATE FUNCTION unfrozen(tbl regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT count(*) FILTER (WHERE unfrozen) FROM sift.parts(tbl) $$;
CREATE FUNCTION dead(tbl regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce(sum(dead_rows), 0) FROM sift.parts(tbl) $$;
CREATE FUNCTION frozen_xid(tbl regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT relfrozenxid::text::bigint FROM pg_class WHERE oid = tbl $$;
CREATE FUNCTION newest_gen(tbl regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT max(part_gen) FROM sift.parts(tbl) $$;

-- 1. VACUUM FREEZE under a limit of 1 MB freezes every part in one pass and moves relfrozenxid (the table is several times the limit)
CREATE TABLE aged (id bigint, k_lo int, payload text) USING sift_table WITH (autovacuum_enabled = false);
SET maintenance_work_mem = '64MB';
SELECT load('aged', 1, 20000);
SELECT load('aged', 20001, 20000);
SELECT load('aged', 40001, 20000);
SELECT load('aged', 60001, 20000);
SELECT load('aged', 80001, 20000);
SELECT load('aged', 100001, 20000);
SELECT frozen_xid('aged') AS xid0, (SELECT sum(bytes) FROM sift.parts('aged')) > 4 * 1024 * 1024 AS over_four_limits FROM (SELECT 1) s \gset
SELECT :'over_four_limits' AS table_over_four_limits;
SET maintenance_work_mem = '2MB';
SET sift.vacuum_rewrite_limit = 1;
VACUUM FREEZE aged;
SELECT unfrozen('aged') AS unfrozen_after_one_pass, frozen_xid('aged') > :xid0 AS relfrozenxid_moved, (SELECT count(*) FROM sift.check('aged')) AS problems;
SELECT count(*) AS rows_kept FROM aged;
RESET sift.vacuum_rewrite_limit;

-- 2. 0 is unlimited in Format 2 and Format 1 alike
SET sift.vacuum_rewrite_limit = 0;
SET sift.async_maintenance = off;
SET maintenance_work_mem = '64MB';

-- Format 2 sift_table: every dead row is rewritten
CREATE TABLE f2t (id bigint, k_lo int, payload text) USING sift_table WITH (autovacuum_enabled = false);
SELECT load('f2t', 1, 40000);
DELETE FROM f2t WHERE k_lo < 4;
VACUUM f2t;
SELECT dead('f2t') AS f2_table_dead_rows;

-- Format 2 sift_col index
CREATE TABLE f2c (id bigint, k_lo int, payload text) WITH (autovacuum_enabled = false);
SELECT load('f2c', 1, 40000);
CREATE INDEX f2c_idx ON f2c USING sift_col (id, k_lo, payload);
DELETE FROM f2c WHERE k_lo < 4;
VACUUM f2c;
SELECT dead('f2c_idx') AS f2_col_dead_rows;

-- Format 1 sift_table: one VACUUM rewrites every dirty segment, as a limit of 0 asks
SET sift.test_create_format1 = on;
CREATE TABLE f1t (id int, pad text) USING sift_table;
SELECT sift.table_set_option('f1t', 'auto_defrag', 'off');
INSERT INTO f1t SELECT i, md5(i::text) || md5((i + 1)::text) FROM generate_series(1, 40000) i;
SELECT busy FROM sift.table_maintain('f1t');
INSERT INTO f1t SELECT i, md5(i::text) || md5((i + 1)::text) FROM generate_series(40001, 80000) i;
SELECT busy FROM sift.table_maintain('f1t');
INSERT INTO f1t SELECT i, md5(i::text) || md5((i + 1)::text) FROM generate_series(80001, 120000) i;
SELECT busy FROM sift.table_maintain('f1t');
CREATE TEMP TABLE f1_before AS SELECT gen FROM sift.table_segments('f1t') WHERE kind = 'columns';
DELETE FROM f1t WHERE id % 2 = 0;
VACUUM f1t;
SELECT count(*) FILTER (WHERE gen IN (SELECT gen FROM f1_before)) AS f1_table_segments_left FROM sift.table_segments('f1t') WHERE kind = 'columns';
SELECT count(*), sum(id) FROM f1t;
SET sift.test_create_format1 = off;

-- Format 1 BM25 index: one VACUUM merges to the fixed point
SET enable_seqscan = off;
CREATE TABLE f1b (id serial, body text) WITH (autovacuum_enabled = off);
INSERT INTO f1b (body) SELECT 'k' || (i % 7) || ' g' || (i % 1000) || ' common word' || (i % 50) FROM generate_series(1, 30000) i;
CREATE INDEX f1b_idx ON f1b USING sift (body) WITH (build_segments = 60);
SELECT count(*) >= 60 AS built_in_pieces FROM sift.segments('f1b_idx');
VACUUM f1b;
SELECT count(*) <= 32 AS merged_by_one_vacuum FROM sift.segments('f1b_idx');
RESET enable_seqscan;
RESET sift.vacuum_rewrite_limit;

-- 3. a VACUUM with sift.async_maintenance = on does its work before it returns
SET sift.async_maintenance = on;
SET maintenance_work_mem = '64MB';
CREATE TABLE o (id int PRIMARY KEY, customer_id int, region int, status int, amount int, n6 int, n7 int, n8 int, notes text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO o SELECT i, i % 100, i % 10, 0, i, i, i, i, 'note ' || i FROM generate_series(1, 60000) i;
VACUUM FREEZE o;
UPDATE o SET status = 1 WHERE id % 50 = 0;
DELETE FROM o WHERE id % 5 < 2;
SELECT newest_gen('o') AS gen_before, dead('o') > 0 AS dead_before, pg_relation_size('o') AS size_before \gset
SELECT :'dead_before' AS dead_before;
VACUUM o;
SELECT newest_gen('o') > :gen_before AS parts_rewritten, dead('o') AS table_dead_after, pg_relation_size('o') < :size_before AS file_cut;
SELECT count(*), sum(amount) FROM o;

-- a short column-delta chain of a table nothing was deleted from is folded into its parts by the VACUUM
CREATE TABLE od (LIKE o INCLUDING ALL) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO od SELECT i, i % 100, i % 10, 0, i, i, i, i, 'note ' || i FROM generate_series(1, 60000) i;
VACUUM FREEZE od;
UPDATE od SET status = 1 WHERE id % 50 = 0;
SELECT newest_gen('od') AS gen_before \gset
VACUUM od;
SELECT newest_gen('od') > :gen_before AS chain_folded, count(*) FILTER (WHERE status = 1) AS updated FROM od;

CREATE TABLE oc (id bigint, k_lo int, payload text) WITH (autovacuum_enabled = false);
SELECT load('oc', 1, 60000);
CREATE INDEX oc_idx ON oc USING sift_col (id, k_lo, payload);
DELETE FROM oc WHERE k_lo < 3;
SELECT dead('oc_idx') > 0 AS col_dead_before, pg_relation_size('oc_idx') AS col_size_before \gset
SELECT :'col_dead_before' AS col_dead_before;
VACUUM oc;
SELECT dead('oc_idx') AS col_dead_after, pg_relation_size('oc_idx') < :col_size_before AS col_file_cut;
SELECT count(*) FROM oc;

DROP TABLE aged, f2t, f2c, f1t, f1b, o, od, oc;
DROP FUNCTION load(regclass, int, int), unfrozen(regclass), dead(regclass), frozen_xid(regclass), newest_gen(regclass);
DROP EXTENSION pg_sift CASCADE;
