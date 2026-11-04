-- The flush of everything inserted (the maintenance worker before the VACUUM it runs, and the crash injected into a space step) asked XLogFlush for the position GetXLogInsertRecPtr names. Where the
-- insert position is the start of a page it names the position after the page header, which no record has reached: ERROR xlog flush request is not satisfied, the injected crash did not happen
-- and a test waited for a restart that never came. pg_switch_wal puts the insert position at the start of the next segment.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SELECT pg_switch_wal() IS NOT NULL AS switched;
SELECT sift._test_flush_wal();
SELECT sift._test_flush_wal();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
