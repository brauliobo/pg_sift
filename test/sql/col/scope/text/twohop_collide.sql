-- The journal word of a string member cut to 3 bits (sift.test_member_hash_bits), set before any index exists so that every record and every stored entry is read under it: members that differ in their
-- bytes share a word, the leave-out of the units and the patch widen to them, and the rows are still those of the join in every read shape and every state of col_scope_texttwohop.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET sift.test_member_hash_bits = 3;

SELECT max(sift._test_member_hash(v)) < 8 AS cut, count(DISTINCT sift._test_member_hash(v)) < 8 AS collide FROM (VALUES (''::text), ('a'), ('foobar'), ('tenant-4'), ('ténant-4'), ('acct-1'), ('acct-2'), ('acct-3'), ('acct-4'), ('acct-5'), ('acct-6')) t (v);

\ir test/col_scope_texttwohop_states.sql
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
