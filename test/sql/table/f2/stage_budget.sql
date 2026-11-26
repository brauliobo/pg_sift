-- The stages of one transaction that writes to 256 sift_table partitions hold at most twice the budget of a bulk insert together (sift_stage_bulk_budget: here work_mem, 4MB): past
-- it the largest are written, so an INSERT ... SELECT and a COPY that spread 384MB of rows over every partition (1.5MB each, below the work_mem of one stage) do not keep them all until
-- the commit. What stays is the budget and the first string chunk of each buffer (256kB). Every row is read back.
CREATE EXTENSION IF NOT EXISTS pg_sift CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;
SET work_mem = '4MB';
SET maintenance_work_mem = '4MB';

CREATE TABLE sb (id int, v text DEFAULT repeat('x', 2000)) PARTITION BY HASH (id);
DO $$ BEGIN
	FOR i IN 0..255 LOOP
		EXECUTE format('CREATE TABLE sb_%s PARTITION OF sb FOR VALUES WITH (MODULUS 256, REMAINDER %s) USING sift_table', i, i);
	END LOOP;
END $$;
CREATE FUNCTION pg_temp.mem() RETURNS bigint LANGUAGE sql AS $$ SELECT sum(used_bytes)::bigint FROM pg_backend_memory_contexts $$;
-- the caches of the 256 partitions are loaded before the memory is taken
SELECT count(*) AS empty FROM sb;
SELECT pg_temp.mem() AS base \gset

BEGIN;
-- 192,000 rows x 2 kB = 384 MB across 256 partitions (1.5 MB/part, below work_mem=4MB) proves the cross-partition stage cap
INSERT INTO sb (id) SELECT g FROM generate_series(1, 192000) g;
SELECT pg_temp.mem() - :base < 128 * 1024 * 1024 AS insert_select_bounded;
COMMIT;

BEGIN;
\copy sb (id) FROM PROGRAM 'seq 192001 384000'
SELECT pg_temp.mem() - :base < 128 * 1024 * 1024 AS copy_bounded;
COMMIT;

SELECT count(*) AS rows, count(DISTINCT id) AS ids, min(id), max(id), bool_and(v = repeat('x', 2000)) AS values_kept FROM sb;

DROP TABLE sb;
DROP EXTENSION pg_sift;
