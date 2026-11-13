-- The member of a scope may be text, varchar or uuid (docs/COL-SCOPE.md "Key types"): the fill gives the members of its batches and of the pairs one code for each value (a map of the statement), the
-- journal of the pairs records the word of a member (FNV-1a of its bytes), the stored units leave out the entries of the members whose word the journal names and the read patches their rows by value,
-- the rows of the write buffer and the members of several keys are read as those of an integer member are. The rows are those of the join in every state the integer member is exact in.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

-- the word a journal records: a fixed function of the bytes (FNV-1a, 64 bits), the same for text and varchar, and for the 16 bytes of a uuid
SELECT v, sift._test_member_hash(v) AS word FROM (VALUES (''::text), ('a'), ('foobar'), ('tenant-4'), ('ténant-4')) t (v);
SELECT sift._test_member_hash('foobar'::varchar) = sift._test_member_hash('foobar'::text) AS varchar_is_text,
       sift._test_member_hash('00000000-0000-0000-0000-000000000000'::uuid) AS zero_uuid, sift._test_member_hash('ffffffff-ffff-ffff-ffff-ffffffffffff'::uuid) AS full_uuid;

\ir test/col_scope_textmember_states.sql
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
