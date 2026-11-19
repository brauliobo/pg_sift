-- A scope with a text, varchar or uuid key has the scope sidecar of an integer one (col/side.h): its runs are keyed by the code of the key in the value store of the key column of the pairs, the
-- build appends the keys the store lacks as one run, and a scope read of the entity rows takes the runs of the key it is given. The rows are the join's in every state of the pairs and of the facts,
-- for the keys that differ in their bytes only (a case, a trailing space, an accent), for a key the store has not and for a key that left every pair.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.col_unit_rows = 150;
SET timezone = 'UTC';
SET jit = off;
SET client_min_messages = warning;

\ir test/col_scope_codedkey_states.sql
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
