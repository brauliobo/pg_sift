-- A segment written before the record of the cut does not record the leaves it was cut by (before the development builds of 5.0 it did not even say whether it was cut by a
-- key or by TID range). A merge that meets one checks its units, and streams it when they are in the order the merge reads, whatever the key_cut was when it was written: it
-- loads it whole and sorts it when they are not, and never fails with "is not in the order of the key" or "of the TIDs".
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
CREATE EXTENSION pg_sift_helpers;
-- Format 1: key_cut segments are Format 1; Format 2.0 ignores key_cut.
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE ku (id int, tenant_id int, a1 int, a2 int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO ku SELECT i, CASE WHEN random() < 0.15 THEN (random() * 100)::int ELSE i / 600 END, (i * 31) % 1000, (i * 37) % 1000, repeat('x', 100)
FROM generate_series(1, 60000) i;
VACUUM (FREEZE, ANALYZE) ku;

CREATE FUNCTION ku_check() RETURNS TABLE (differences bigint) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift.col_verify('ku_col', true);
	RETURN QUERY SELECT sift_helpers.differences_agg('SELECT tenant_id, count(*) AS n, sum(a1) AS s, max(id) AS m FROM ku GROUP BY tenant_id')
		+ sift_helpers.differences_agg('SELECT a1, count(*) AS n, sum(a2) AS s FROM ku WHERE a1 BETWEEN 100 AND 400 GROUP BY a1');
END $$;

CREATE FUNCTION ku_churn(base int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO ku SELECT base + i, (random() * 100)::int, (i * 7) % 1000, i % 999, 'x' FROM generate_series(1, 2000) i;
	DELETE FROM ku WHERE id % 13 = base % 13 AND id < 60000;
$$;

CREATE FUNCTION ku_cuts() RETURNS TABLE (recorded bigint, unrecorded bigint, probes bigint) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT count(*) FILTER (WHERE cardinality(cut) > 0), count(*) FILTER (WHERE cardinality(cut) = 0), sift._test_col_cut_probes() FROM sift._test_col_cuts('ku_col');
END $$;

-- the segments are written as the three kinds of release did (0: with the record, 1: the flag of the cut only, 2: nothing); the counts are the segments with and without the record, and the segments checked unit by unit so far, then the key_cut changes in each of the ways
CREATE PROCEDURE ku_case(record int, before text, after text, base int) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('SET sift.test_col_cut_record = %s', record);
	EXECUTE format('CREATE INDEX ku_col ON ku USING sift_col (tenant_id, a1, a2) WITH (key_cut = %L, order_array_columns = ''off'')', before);
	PERFORM ku_churn(base);
	PERFORM sift.col_merge('ku_col', true);
	PERFORM ku_churn(base + 10000);
	SET sift.test_col_cut_record = 0;
	EXECUTE format('ALTER INDEX ku_col SET (key_cut = %L)', after);
END $$;

-- the cut stays the same: the old segments are the stream the merge reads
CALL ku_case(2, 'tenant_id', 'tenant_id', 100000);
SELECT * FROM ku_cuts();
SELECT sift.col_merge('ku_col', true) > 0 AS merged;
SELECT * FROM ku_cuts();
SELECT * FROM ku_check();
DROP INDEX ku_col;

-- another key, none, and another key again
CALL ku_case(2, 'tenant_id', 'a1', 200000);
SELECT * FROM ku_cuts();
SELECT sift.col_merge('ku_col', true) > 0 AS merged;
SELECT * FROM ku_cuts();
SELECT * FROM ku_check();
DROP INDEX ku_col;
CALL ku_case(2, 'tenant_id', 'off', 300000);
SELECT * FROM ku_cuts();
SELECT sift.col_merge('ku_col', true) > 0 AS merged;
SELECT * FROM ku_cuts();
VACUUM ku;
SELECT * FROM ku_check();
DROP INDEX ku_col;
CALL ku_case(1, 'tenant_id', 'a1', 400000);
SELECT * FROM ku_cuts();
SELECT sift.col_merge('ku_col', true) > 0 AS merged;
SELECT * FROM ku_cuts();
VACUUM ku;
SELECT * FROM ku_check();
DROP INDEX ku_col;
CALL ku_case(1, 'a1', 'tenant_id', 500000);
SELECT * FROM ku_cuts();
SELECT sift.col_merge('ku_col', true) > 0 AS merged;
SELECT * FROM ku_cuts();
SELECT * FROM ku_check();
DROP INDEX ku_col;
CALL ku_case(1, 'off', 'tenant_id', 600000);
SELECT * FROM ku_cuts();
SELECT sift.col_merge('ku_col', true) > 0 AS merged;
SELECT * FROM ku_cuts();
SELECT * FROM ku_check();

DROP TABLE ku;
DROP PROCEDURE ku_case(int, text, text, int);
DROP FUNCTION ku_check(), ku_churn(int), ku_cuts();
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
