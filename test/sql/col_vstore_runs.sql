-- The values of a shared value store (docs/COL-VSTORE.md): a value has one code for ever, the codes are handed out in the order the values arrive,
-- the store is a chain of runs that is merged when it gets long, and a backend that forgets what it has read finds the same codes again.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.test_vstore_columns = 'url';
CREATE TABLE vr_t(id int PRIMARY KEY, url text);
CREATE INDEX vr_a ON vr_t USING sift_col (id, url);
CREATE INDEX vr_b ON vr_t USING sift_col (url, id);

-- a new value gets the next code; a value that is there keeps its code, in the same batch or in a later one
SELECT sift._test_vstore_intern('vr_t_url_sift_values', array['a', 'b', 'c']) AS codes;
SELECT sift._test_vstore_intern('vr_t_url_sift_values', array['b', 'd', 'a', 'e', 'd']) AS codes;
SELECT sift._test_vstore_intern('vr_t_url_sift_values', array['e', 'a']) AS codes;
SELECT runs, view_values FROM sift._test_vstore_info('vr_t_url_sift_values');

-- what a backend reads again is what it had: the codes, the values, and nothing for a code that is not there
SELECT sift._test_vstore_forget();
SELECT runs, view_values FROM sift._test_vstore_info('vr_t_url_sift_values');
SELECT c AS code, sift._test_vstore_value('vr_t_url_sift_values', c) AS value FROM generate_series(0, 5) c;

-- many small appends, each its own transaction: the chain is merged (at most 12 runs), the codes do not change, and the relation does not grow a run at a time
\o /dev/null
SELECT format('SELECT sift._test_vstore_intern(%L, array[%L, %L])', 'vr_t_url_sift_values', 'v' || i, 'w' || i) FROM generate_series(1, 60) i \gexec
\o
SELECT runs <= 12 AS merged, view_values, blocks < 64 AS compact FROM sift._test_vstore_info('vr_t_url_sift_values');
SELECT sift._test_vstore_forget();
SELECT sift._test_vstore_intern('vr_t_url_sift_values', array['a', 'v7', 'w60', 'e', 'new']) AS codes;
SELECT c AS code, sift._test_vstore_value('vr_t_url_sift_values', c) AS value FROM unnest(array[0, 4, 12, 124, 125]) c;

-- a rewrite of the relation keeps the pages, so the runs are still there
VACUUM FULL vr_t_url_sift_values;
SELECT sift._test_vstore_forget();
SELECT sift._test_vstore_intern('vr_t_url_sift_values', array['a', 'v7', 'w60', 'e', 'new']) AS codes;
SELECT view_values FROM sift._test_vstore_info('vr_t_url_sift_values');

DROP TABLE vr_t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
