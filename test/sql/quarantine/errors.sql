-- Only damage takes an index out of service: an error of a sift path that has another SQLSTATE (cancel, timeouts, resource limits, serialization and deadlock failures, permissions,
-- user errors) propagates as it is and records nothing, in quarantine mode and under the error context of the index. Damage in the same place records the index.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE q;
	RETURN 'ok';
EXCEPTION WHEN query_canceled OR OTHERS THEN
	RETURN SQLSTATE || ': ' || SQLERRM;
END $$;

SET client_min_messages = warning;
SET sift.on_corruption = quarantine;
CREATE TABLE t (id int, v int, body text);
INSERT INTO t SELECT g, g % 7, 'alpha beta' || g % 5 FROM generate_series(1, 20000) g;
CREATE INDEX t_col ON t USING sift_col (id, v);
CREATE INDEX t_bm25 ON t USING sift (body);
ANALYZE t;

SELECT i AS index, s AS sqlstate, expect(format('SELECT sift._test_raise(%L::regclass, %L)', i, s)) AS raised, (SELECT count(*) FROM sift.quarantine) AS recorded
FROM unnest(ARRAY['t_col', 't_bm25']) i, unnest(ARRAY['57014', '57P01', '53100', '53200', '53300', '53400', '40001', '40P01', '55P03', '42501', '22003', '23505', 'XX000']) s
ORDER BY i, s;

-- permissions: the maintenance functions refuse a role that does not own the index
CREATE ROLE quarantine_errors_reader;
GRANT SELECT ON t TO quarantine_errors_reader;
SET ROLE quarantine_errors_reader;
SELECT left(expect($$ SELECT sift.promote('t_bm25') $$), 5) AS promote_without_ownership;
SELECT left(expect($$ SELECT sift.col_merge('t_col') $$), 5) AS merge_without_ownership;
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS reads_go_on;
RESET ROLE;
DROP OWNED BY quarantine_errors_reader;
DROP ROLE quarantine_errors_reader;
SELECT count(*) AS recorded_after_permissions FROM sift.quarantine;

-- the same index, damaged: recorded, whatever the code the caller expects
SELECT sift._test_poke('t_col', min(first_block), 8176, 9, 2) FROM sift.col_extents('t_col') WHERE what IN ('segment extent', 'part');
SELECT expect($$ SELECT count(*) FROM t WHERE v = 3 $$) AS damage;
SELECT index, error_code, detections FROM sift.quarantine;
SELECT s AS sqlstate, expect(format('SELECT sift._test_raise(%L::regclass, %L)', 't_bm25', s)) AS raised, (SELECT count(*) FROM sift.quarantine) AS recorded
FROM unnest(ARRAY['57014', '53200', '40001']) s ORDER BY s;

DROP TABLE t;
DROP FUNCTION expect(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
