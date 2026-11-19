-- A chunk of a long-lived context that nothing points to any more: the leak scripts/valgrind.sh --leaks-selftest plants, which the memcheck leak gate has to fail on (a plain run only returns)
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SELECT sift._test_leak('palloc');
-- a sift memory context that outlives the backend, only when the selftest asks (PGOPTIONS='-c planted.context=on'): the per-test hooks have to fail the test
DO $$ BEGIN IF current_setting('planted.context', true) = 'on' THEN PERFORM sift._test_leak('context'); END IF; END $$;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
