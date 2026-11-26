-- A scope of two paths (the grants a tenant has on an account and on the groups of the account) with a text or uuid key and a text, varchar or uuid member (docs/COL-SCOPE.md "Key types"): the three
-- journals record the word of a member, the scope summary takes the members the journals name as changed, and the rows are those of the join (the union of the direct grants and the members of
-- the groups granted) in every state, read by the scope node, by sift with sift.agg_scope off and by the normal plan.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

\ir test/common/sql/col/scope/texttwohop_states.sql
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
