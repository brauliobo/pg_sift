-- A table of 600 sift_table partitions (more than the 512 manifest slots, the 256 pinned partitions and the 128 column-delta map slots of a backend) is read by one SiftAggV6 node row for row as its heap twin is answered,
-- after narrow updates that go to the column delta: the memory the backend holds for sift stays under 64 MB, and the statement takes no lock on a relation outside the partitioned table (PostgreSQL takes one on each partition).
-- requires: build col
\set ON_ERROR_STOP on
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE TABLE sp (id int NOT NULL, k int NOT NULL, v int NOT NULL, tag text NOT NULL) PARTITION BY RANGE (k);
SELECT format('CREATE TABLE sp_%s PARTITION OF sp FOR VALUES FROM (%s) TO (%s) USING sift_table', i, i * 10, i * 10 + 10) FROM generate_series(0, 599) i \gexec
CREATE TABLE sph (LIKE sp);
-- 120000 rows are 200 for each of the 600 partitions: every partition holds a part that the node reads
INSERT INTO sp SELECT g, g % 6000, g % 97, 'tag' || g % 3 FROM generate_series(1, 120000) g;
INSERT INTO sph SELECT * FROM sp;
ANALYZE sp; ANALYZE sph;
UPDATE sp SET v = v + 1 WHERE id % 400 = 0;
UPDATE sph SET v = v + 1 WHERE id % 400 = 0;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; plan text; engine text := 'native';
BEGIN
	FOR plan IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF plan LIKE '%SiftAggV6%' THEN engine := 'v6'; END IF;
	END LOOP;
	EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || q || ') x' INTO a;
	EXECUTE 'SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), '''') FROM (' || regexp_replace(q, '\msp\M', 'sph', 'g') || ') x' INTO b;
	RETURN engine || ' ' || CASE WHEN a = b THEN 'same' ELSE 'DIFFERENT' END;
END $$;

SELECT same('SELECT k % 7, count(*), sum(v) FROM sp GROUP BY 1') AS all_partitions;
SELECT same('SELECT tag, count(*), sum(v), min(id), max(id) FROM sp WHERE v > 10 GROUP BY tag') AS filtered;
SELECT same('SELECT count(*), sum(v) FROM sp WHERE k BETWEEN 1000 AND 1999') AS pruned_to_100;
SELECT same('SELECT count(*), sum(v) FROM sp WHERE k = 4242') AS one_partition;
SELECT same('SELECT count(*), sum(v) FROM sp') AS total;

SELECT pg_size_pretty(sum(total_bytes)) AS sift_memory, sum(total_bytes) < 64 * 1024 * 1024 AS under_budget
FROM pg_backend_memory_contexts WHERE name LIKE 'sift%' \gset
SELECT :'under_budget' AS under_budget;

BEGIN;
SELECT count(*) > 0 AS read FROM (SELECT k % 7, count(*) FROM sp GROUP BY 1) r;
SELECT count(*) AS locks_outside_the_table
FROM pg_locks l WHERE l.pid = pg_backend_pid() AND l.locktype = 'relation' AND l.relation >= 16384
	AND l.relation NOT IN (SELECT oid FROM pg_class WHERE relname = 'sp' OR relname ~ '^sp_[0-9]+' OR relname ~ '^sp_[0-9]+_');
COMMIT;
DROP TABLE sp, sph;
DROP FUNCTION same(text);
DROP EXTENSION pg_sift;
