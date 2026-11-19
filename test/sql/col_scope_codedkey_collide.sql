-- col_scope_codedkey with the journal word of a string cut to 3 bits (sift.test_member_hash_bits): the tag a journal puts on a record of a pair is the word of its key, so the pairs of other keys are
-- read as changes of the key's own, which can only widen what a run gives up. The rows are the join's all the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;
SET sift.test_member_hash_bits = 3;

\ir test/col_scope_codedkey_states.sql
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
