-- sift.drop_parts_before: the sealed parts whose newest row is older than the cutoff leave the table at once; a part that spans the cutoff stays whole; the arguments are checked. Every check prints t.
-- One part for each day: a day is inserted and sealed by its own call of sift.table_maintain, each in a transaction of its own. Run: psql -X -v ON_ERROR_STOP=1 -f this file, on a build with sift_table and sql/17-drop-parts.sql.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
-- a testing build creates Format 1 relations unless told otherwise; a release build has no such setting
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;

CREATE TABLE dp (id int, at timestamptz NOT NULL, payload text) USING sift_table WITH (autovacuum_enabled = false);
\o /dev/null
SELECT stmt FROM generate_series(0, 19) d,
	LATERAL unnest(ARRAY[
		format('INSERT INTO dp SELECT %s * 200 + g, timestamptz ''2030-01-01'' + %s * interval ''1 day'' + g * interval ''1 minute'', repeat(''x'', 40) FROM generate_series(0, 199) g', d, d),
		'SELECT sift.table_maintain(''dp'')']) WITH ORDINALITY AS u(stmt, ord) ORDER BY d, ord
\gexec
\o
SELECT count(*) = 4000 AS all_rows_present FROM dp;

SELECT parts_dropped = 5 AND rows_dropped = 1000 AND bytes_reclaimed > 0 AS days_before_the_cutoff_dropped
FROM sift.drop_parts_before('dp', timestamptz '2030-01-06');
SELECT count(*) = 3000 AND min(at) >= timestamptz '2030-01-06' AS only_the_retention_remains FROM dp;
SELECT parts_dropped = 0 AS second_call_finds_nothing FROM sift.drop_parts_before('dp', timestamptz '2030-01-06');
SELECT parts_dropped = 2 AND parts_partial = 1 AS the_part_spanning_the_cutoff_stays
FROM sift.drop_parts_before('dp', timestamptz '2030-01-08 01:00');
SELECT count(*) = 2600 AS rows_after_the_next_cutoff FROM dp;

CREATE TABLE dp_heap (id int);
CREATE TABLE dp_nottime (id int, n int) USING sift_table;
CREATE FUNCTION raises(q text) RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN EXECUTE q; RETURN false; EXCEPTION WHEN OTHERS THEN RETURN true; END $$;
SELECT raises($$SELECT * FROM sift.drop_parts_before('dp_heap', now())$$) AS refuses_a_heap_table;
SELECT raises($$SELECT * FROM sift.drop_parts_before('dp_nottime', now())$$) AS refuses_a_table_without_a_time_column;
SELECT raises($$SELECT * FROM sift.drop_parts_before('dp', 'infinity')$$) AS refuses_an_infinite_cutoff;
SELECT raises($$SELECT * FROM sift.drop_parts_before('dp', now(), 'payload')$$) AS refuses_a_column_that_is_not_a_time;

DROP TABLE dp, dp_heap, dp_nottime;
DROP FUNCTION raises(text);
