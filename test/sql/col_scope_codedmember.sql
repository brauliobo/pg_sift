-- A scope with a text, varchar or uuid member has the scope sidecar of an integer one (col/side.h): a run keeps a Bloom filter of the journal words of the members its units hold, and a read of one
-- key takes the runs none of whose members' pairs changed, reads the other units by reference with the members listed as strings, and gives the join's rows in every state of the pairs and of
-- the facts: a pair added, removed, re-keyed, a member renamed or leaving every pair, row edits, VACUUM, promotion, merge and REINDEX, for accounts that differ in a case, a space, an accent,
-- the empty one and one of 2,000 bytes.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;

\ir test/col_scope_codedmember_states.sql
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
