-- A rewrite of a Format 2.0 sift_table (VACUUM FULL, CLUSTER) writes its parts under the key order of the relation that survives it: its primary key, which the new heap does not have yet.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;

CREATE TABLE kv (id int PRIMARY KEY, grp int, label text) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO kv SELECT g, g % 10, 'v' || g FROM generate_series(1, 30000) g;
UPDATE kv SET label = 't' || id WHERE id % 7 = 0;
UPDATE kv SET grp = grp + 1 WHERE id % 5 = 0;
DELETE FROM kv WHERE id % 11 = 0;

SELECT count(*), sum(grp), count(DISTINCT label) FROM kv;
SELECT count(*) AS check_problems FROM sift.check('kv'::regclass);

VACUUM FULL kv;
SELECT count(*), sum(grp), count(DISTINCT label) FROM kv;
SELECT count(*) AS check_problems_after_vacuum_full FROM sift.check('kv'::regclass);
SELECT id, grp, label FROM kv WHERE id IN (1, 7, 15, 29999) ORDER BY id;

CLUSTER kv USING kv_pkey;
SELECT count(*), sum(grp) FROM kv;
SELECT count(*) AS check_problems_after_cluster FROM sift.check('kv'::regclass);

CREATE TABLE kt (at timestamptz, n int) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO kt SELECT timestamptz '2030-01-01' + g * interval '1 minute', g FROM generate_series(1, 20000) g;
VACUUM FULL kt;
SELECT count(*) AS check_problems_time_key FROM sift.check('kt'::regclass);

DROP TABLE kv, kt;
DROP EXTENSION pg_sift;
