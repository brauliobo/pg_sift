-- VACUUM FULL and CLUSTER of a sift_table under a maintenance_work_mem past 4 GB: the versions the rewrite holds before it writes their deletes stay in one palloc
-- (MaxAllocSize), and a CLUSTER inside the transaction that deleted rows keeps them for its own snapshots.
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;

CREATE TABLE cb (id int PRIMARY KEY, v text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO cb SELECT g, 'v' || g FROM generate_series(1, 2000) g;
DELETE FROM cb WHERE id % 10 = 0;
SET maintenance_work_mem = '5GB';
VACUUM FULL cb;
SELECT count(*), sum(id) FROM cb;
BEGIN;
DELETE FROM cb WHERE id % 3 = 0;
CLUSTER cb USING cb_pkey;
SELECT count(*), sum(id) FROM cb;
COMMIT;
SET maintenance_work_mem = '8GB';
CLUSTER cb USING cb_pkey;
SELECT count(*), sum(id) FROM cb;
SELECT count(*) AS check_problems FROM sift.check('cb'::regclass);
RESET maintenance_work_mem;

DROP TABLE cb;
DROP EXTENSION pg_sift;
