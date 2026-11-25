-- The caches of a Format 2.0 relation follow the relation: the part pins, the address indexes and the decoded granules a statement left are in the budget (sift.backend_cache_kb = 0 empties
-- them), a relation that is dropped or detached takes its own, and the row locks of a transaction are the transaction's, whatever the cache holds.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE FUNCTION named(n text) RETURNS bigint LANGUAGE sql AS
$$ SELECT count(*) FROM pg_backend_memory_contexts WHERE name = n $$;
CREATE FUNCTION kept() RETURNS text LANGUAGE sql AS
$$ SELECT (named('sift cache f2 part pin') > 0)::text || '/' || (named('sift cache f2 tid index') > 0)::text || '/' || (named('sift cache f2 granule') > 0)::text $$;

CREATE TABLE ct (id int PRIMARY KEY, v int) USING sift_table;
INSERT INTO ct SELECT i, i % 97 FROM generate_series(1, 20000) i;
VACUUM ct;
SELECT sum(v) FROM ct WHERE id BETWEEN 100 AND 200;
UPDATE ct SET v = v + 1 WHERE id = 150;
SELECT kept() AS pins_indexes_granules_after_reads_and_a_write;

-- the setting that keeps nothing empties all three
SET sift.backend_cache_kb = 0;
SELECT named('sift cache f2 part pin') AS pins, named('sift cache f2 tid index') AS tid_indexes, named('sift cache f2 granule') AS granules;
RESET sift.backend_cache_kb;

-- a dropped relation takes its pins, address index and granules
SELECT sum(v) FROM ct WHERE id BETWEEN 100 AND 200;
UPDATE ct SET v = v + 1 WHERE id = 150;
SELECT kept() AS kept_before_drop;
DROP TABLE ct;
SELECT named('sift cache f2 part pin') AS pins, named('sift cache f2 tid index') AS tid_indexes, named('sift cache f2 granule') AS granules;

-- a detached and dropped partition takes its own pins and leaves the other's
CREATE TABLE ev (id int, bucket int, amount int) PARTITION BY RANGE (bucket);
CREATE TABLE ev1 PARTITION OF ev FOR VALUES FROM (0) TO (100) USING sift_table;
CREATE TABLE ev2 PARTITION OF ev FOR VALUES FROM (100) TO (200) USING sift_table;
INSERT INTO ev SELECT i, i % 200, i FROM generate_series(1, 4000) i;
VACUUM ev1, ev2;
SELECT count(*), sum(amount) FROM ev;
SELECT named('sift cache f2 part pin') AS pins_of_two_partitions;
ALTER TABLE ev DETACH PARTITION ev2;
DROP TABLE ev2;
SELECT named('sift cache f2 part pin') AS pins_of_one_partition;
SELECT count(*), sum(amount) FROM ev;
DROP TABLE ev;
SELECT named('sift cache f2 part pin') AS pins_of_none;

-- the row locks of a transaction are in a context of the transaction, gone at its end, committed or not
CREATE TABLE lk (id int PRIMARY KEY, v int) USING sift_table;
INSERT INTO lk SELECT i, i FROM generate_series(1, 100) i;
BEGIN;
SELECT v FROM lk WHERE id = 7 FOR UPDATE;
SELECT named('sift f2 row locks of the transaction') AS row_lock_context_in_transaction;
COMMIT;
SELECT named('sift f2 row locks of the transaction') AS after_commit;
BEGIN;
SELECT v FROM lk WHERE id = 8 FOR UPDATE;
ROLLBACK;
SELECT named('sift f2 row locks of the transaction') AS after_rollback;
SELECT count(*) AS under_the_cache_root FROM pg_backend_memory_contexts c
WHERE c.name LIKE 'sift f2 row locks%' OR (c.name = 'sift cache f2 row locks of the transaction');
DROP TABLE lk;

DROP FUNCTION kept(), named(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
