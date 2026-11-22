-- The quarantine set of format major 2 lives in shared memory and no subtransaction touches it: adding an index to it assigns no transaction id, and it outlives the transaction.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE t (v int);
CREATE INDEX t_idx ON t USING sift_store_test (v);
SELECT sift._test_f2_init('t_idx');

SELECT sift._test_f2_qset('t_idx', 'count') AS before_count, sift._test_f2_qset('t_idx', 'has') AS before_has;
SELECT sift._test_f2_qset('t_idx', 'generation') AS g0 \gset
BEGIN;
SELECT sift._test_f2_qset('t_idx', 'add');
SELECT sift._test_f2_qset('t_idx', 'has') AS has, sift._test_f2_qset('t_idx', 'count') AS cnt, sift._test_f2_qset('t_idx', 'generation') > :g0 AS generation_moved;
SELECT txid_current_if_assigned() IS NULL AS no_xid;
ROLLBACK;
SELECT sift._test_f2_qset('t_idx', 'has') AS has_after_rollback_the_set_is_not_transactional;
SELECT sift._test_f2_qset('t_idx', 'remove');
SELECT sift._test_f2_qset('t_idx', 'has') AS has, sift._test_f2_qset('t_idx', 'count') AS cnt;

DROP TABLE t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
