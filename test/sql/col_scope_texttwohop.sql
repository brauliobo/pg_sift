-- A scope of two paths (the grants a tenant has on an account and on the groups of the account) with a text or uuid key and a text, varchar or uuid member (docs/COL-SCOPE.md "Key types"): the three
-- journals record the word of a member and the codes of the keys are those of the statement, the stored units leave out the members the journals name, the read patches their rows by value from the
-- entity segments and the rows are those of the join (the union of the direct grants and the members of the groups granted) in every state the integer form is exact in.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET default_statistics_target = 1000;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

\ir test/col_scope_texttwohop_states.sql
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
