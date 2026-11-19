-- col_scope_codedmember with the journal word of a member cut to 3 bits (sift.test_member_hash_bits): every changed member is in the filter of nearly every run, so the runs give up more and the
-- rows are the join's all the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;
SET sift.test_member_hash_bits = 3;

\ir test/col_scope_codedmember_states.sql
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
