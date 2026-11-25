-- sift_table: the scans of the foreground feed the pacer of the maintenance workers (core/maint_pacer.h), one operation in 16 of a backend: the average of their time is empty before and
-- is not after 32 scans of a table of Format 2.0. The fast average is the first value of the pair.
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE paced USING sift_table AS SELECT g AS id, repeat('x', 40) AS pad FROM generate_series(1, 2000) g;
SELECT sift._test_pacer_reset();
SELECT split_part(sift._test_pacer_foreground(0, 0), ' ', 1)::bigint AS average_before;
DO $$ BEGIN FOR i IN 1..32 LOOP PERFORM count(*) FROM paced; END LOOP; END $$;
SELECT split_part(sift._test_pacer_foreground(0, 0), ' ', 1)::bigint > 0 AS the_scans_were_sampled;
SELECT sift._test_pacer_reset();
DROP TABLE paced;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
