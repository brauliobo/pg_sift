-- The codes of one epoch of a shared value store are bounded (docs/LIMITS.md, value_store_codes): a code is 32 bits and a backend keeps two slots for each. The batch that
-- would pass the bound is refused before any of its strings is appended, the strings the store has need no code, and the store is as it was after the refusal.
-- A code that the store does not hold is refused where it is read.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.test_vstore_columns = 'url';
CREATE TABLE vl_t(id int PRIMARY KEY, url text);
CREATE INDEX vl_a ON vl_t USING sift_col (id, url);
CREATE INDEX vl_b ON vl_t USING sift_col (url, id);
SET sift.test_limits = 'value_store_codes=5';

SELECT sift._test_vstore_intern('vl_t_url_sift_values', array['a', 'b', 'c']) AS codes;
SELECT sift._test_vstore_intern('vl_t_url_sift_values', array['d', 'e', 'a']) AS codes;
SELECT sift._test_vstore_intern('vl_t_url_sift_values', array['e', 'a', 'c']) AS codes_known;
SELECT sift._test_vstore_intern('vl_t_url_sift_values', array['f']) AS codes;
SELECT sift._test_vstore_intern('vl_t_url_sift_values', array['b', 'g', 'h']) AS codes;
SELECT runs, view_values FROM sift._test_vstore_info('vl_t_url_sift_values');

RESET sift.test_limits;
SELECT sift._test_vstore_intern('vl_t_url_sift_values', array['f', 'a', 'g']) AS codes;
SELECT runs, view_values FROM sift._test_vstore_info('vl_t_url_sift_values');

-- a code that a segment names and the store does not hold is an error, not a read past the strings of the store
SELECT c AS code, sift._test_vstore_string_at('vl_t_url_sift_values', c) AS value FROM generate_series(5, 6) c;
SELECT sift._test_vstore_string_at('vl_t_url_sift_values', 7);
SELECT sift._test_vstore_string_at('vl_t_url_sift_values', 2147483648);
SELECT sift._test_vstore_string_at('vl_t_url_sift_values', 4294967295);

DROP TABLE vl_t;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
