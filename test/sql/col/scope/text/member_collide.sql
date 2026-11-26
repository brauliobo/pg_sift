-- The journal word of a string member cut to 3 bits (sift.test_member_hash_bits), set before any index exists so that every record and every stored entry is read under it: members that differ in their
-- bytes share a word, the members a read takes as changed widen to them, and the rows are still those of the join in every read shape and every state of col_scope_textmember.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.test_member_hash_bits = 3;

SELECT max(sift._test_member_hash(v)) < 8 AS cut, count(DISTINCT sift._test_member_hash(v)) < 8 AS collide FROM (VALUES (''::text), ('a'), ('foobar'), ('tenant-4'), ('ténant-4'), ('acct-1'), ('acct-2'), ('acct-3'), ('acct-4'), ('acct-5'), ('acct-6')) t (v);

\ir test/common/sql/col/scope/textmember_states.sql
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
