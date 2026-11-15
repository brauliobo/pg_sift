-- Every slot of the executor state's table lives in the query memory context, the one that outlives the nodes: ExecutorEnd resets the table after their contexts are deleted,
-- so a slot made in a node's context (a table slot of a scan made when it first runs, a cold backend's crash) is read freed. A testing build checks it at the end of every statement.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SELECT sift._test_slot_owner(false) AS query_context_accepted, sift._test_slot_owner(true) AS node_context_accepted;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
