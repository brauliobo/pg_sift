-- REINDEX CONCURRENTLY swaps every dependency of the index it built with the index it replaces, and a member of a shared value store is a member through a
-- dependency. The new index of a column that the old one held in its own segments (it was built before the store) shares the store, so the dependency has to be
-- the new index's after the swap: the store goes on when the index that made it is dropped, and the new index is read, verified and counted as a member.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;

CREATE TABLE vr (id int PRIMARY KEY, uid text, url text) WITH (autovacuum_enabled = false);
INSERT INTO vr SELECT i, md5(i::text), md5((i * 7)::text) FROM generate_series(1, 5000) i;
VACUUM (FREEZE, ANALYZE) vr;
CREATE FUNCTION vr_states() RETURNS TABLE (index_name name, column_name name, state text) LANGUAGE sql AS
$$ SELECT m.index_name, m.column_name, m.state FROM sift.col_value_store_members() m WHERE m.table_name = 'vr'::regclass ORDER BY 1, 2 $$;

-- vr_a keeps its own strings: the store is made by vr_b, which is built after it
CREATE INDEX vr_a ON vr USING sift_col (id, uid, url);
CREATE INDEX vr_b ON vr USING sift_col (url, uid, id);
SELECT * FROM vr_states();

-- the one that holds its own strings, and then the one that shares
SELECT format('REINDEX INDEX CONCURRENTLY %s', ix) FROM (VALUES ('vr_a'), ('vr_b')) AS v(ix) \gexec
SELECT * FROM vr_states();
SELECT member FROM sift.col_value_store_usage('vr_uid_sift_values') WHERE member IS NOT NULL ORDER BY 1;

-- the index that made the store goes: the other one still reads and verifies it
DROP INDEX vr_b;
SELECT * FROM vr_states();
SELECT count(*) AS vstores FROM sift.col_value_store_status WHERE table_name = 'vr'::regclass;
SELECT sift.col_verify('vr_a', true);
SET sift.enable_agg = on;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SELECT count(*), min(id) FROM vr WHERE uid = md5('7');
SET sift.enable_agg = off;
SET enable_seqscan = on;
SELECT count(*), min(id) FROM vr WHERE uid = md5('7');

DROP FUNCTION vr_states();
DROP TABLE vr;
DROP EXTENSION pg_sift;
