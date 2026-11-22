-- The switch contract of the deletion records (REPORT, col/v1/wbuf.h): a step that gives a column index a segment generation in place of another writes the records that name the old one
-- again under the new one (sift_col_dv_rekey), for the units it copies as they are, and carries the records of the units it cuts again to marks. The step here is a relocation, which copies
-- every page of a segment (the units of the new generation are those of the old with the same numbers); the hook cuts unit 1 again, as a split of a partition that re-cuts one unit does.
-- Every state is compared with the answer of the heap: records of a statement in progress, committed ones, and after the fold.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, note text) WITH (autovacuum_enabled = false, fillfactor = 60);
INSERT INTO events SELECT i, i % 10, i, repeat('n', 150) FROM generate_series(1, 30000) i;
CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_changes = 'id');
VACUUM (FREEZE) events;
INSERT INTO events SELECT 100000 + i, i % 10, i, repeat('n', 150) FROM generate_series(1, 4000) i;
SELECT sift.col_promote('events_c') >= 0 AS promoted;
-- the merge leaves the two segments it replaced as free blocks below its output: room for the relocation
SELECT sift.col_merge('events_c', true) >= 0 AS merged;
SELECT count(*) = 1 AS one_segment, sum(units) > 3 AS several_units FROM sift.col_segments('events_c') WHERE kind = 'immutable';

CREATE FUNCTION chk(what text, OUT test text, OUT same_as_heap bool) LANGUAGE plpgsql AS $$
DECLARE a text; o text; q text;
BEGIN
	test := what;
	same_as_heap := true;
	FOREACH q IN ARRAY ARRAY[
		'SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY 1 ORDER BY 1',
		'SELECT count(*), sum(counter_a), min(id), max(id) FROM events',
		'SELECT count(*) FROM events WHERE id BETWEEN 1000 AND 9000 AND tenant_id IN (2, 5)',
		'SELECT id FROM events WHERE id BETWEEN 5000 AND 5100 ORDER BY id']
	LOOP
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO a;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO o;
		same_as_heap := same_as_heap AND a IS NOT DISTINCT FROM o;
	END LOOP;
END $$;
CREATE FUNCTION marks() RETURNS int LANGUAGE sql AS
	$$ SELECT count(*)::int FROM sift._test_col_log_entries('events_c') WHERE kind = 1 AND flags & 32768 = 0 $$;

SELECT * FROM chk('fresh');
-- committed and old: its records name the generation that the relocation replaces
DELETE FROM events WHERE id % 20 = 3;
SELECT count(DISTINCT part) > 3 AS records_in_units FROM sift._test_col_log_records('events_c');
SELECT * FROM chk('committed records');

-- in progress: the statement is not done when the segment moves
BEGIN;
DELETE FROM events WHERE id % 20 = 7;
SELECT gen AS old_gen FROM sift.col_segments('events_c') WHERE kind = 'immutable' \gset
SELECT marks() AS marks_before \gset
SELECT array_agg(DISTINCT part ORDER BY part) AS old_parts FROM sift._test_col_log_records('events_c') WHERE gen = :old_gen \gset
SELECT sift._test_col_relocate('events_c', '{1}') AS moved;
SELECT gen <> :old_gen AS generation_replaced FROM sift.col_segments('events_c') WHERE kind = 'immutable';
SELECT gen AS new_gen FROM sift.col_segments('events_c') WHERE kind = 'immutable' \gset
-- the records of the units copied as they are are written again under the new generation, with the same unit numbers and rows; the cut-again unit 1 went to marks
SELECT array_agg(DISTINCT part ORDER BY part) = array_remove(:'old_parts'::int[], 1) AS rekeyed_units_but_the_cut_one FROM sift._test_col_log_records('events_c') WHERE gen = :new_gen;
SELECT (SELECT sum(nrows) FROM sift._test_col_log_records('events_c') WHERE gen = :new_gen) =
       (SELECT sum(nrows) FROM sift._test_col_log_records('events_c') WHERE gen = :old_gen AND part <> 1) AS same_rows_rekeyed;
SELECT marks() - :marks_before = (SELECT sum(nrows) FROM sift._test_col_log_records('events_c') WHERE gen = :old_gen AND part = 1) AS cut_unit_carried_to_marks;
SELECT * FROM chk('in progress, after the switch');
COMMIT;
SELECT * FROM chk('committed, after the switch');

-- the fold clears the rows of the re-keyed records in the new segment and of the marks, and drops them
SELECT sift._test_col_fold_marks('events_c') > 0 AS folded;
SELECT count(*) FILTER (WHERE gen = :old_gen) AS old_gen_records_left FROM sift._test_col_log_records('events_c');
SELECT * FROM chk('folded');
SELECT sift.col_verify('events_c', true) IS NOT NULL AS verified;

-- the whole segment copied as it is: every record is written again, no mark
DELETE FROM events WHERE id % 20 = 11;
SELECT gen AS old_gen FROM sift.col_segments('events_c') WHERE kind = 'immutable' \gset
SELECT marks() AS marks_before \gset
SELECT sift._test_col_relocate('events_c') AS moved;
SELECT gen AS new_gen FROM sift.col_segments('events_c') WHERE kind = 'immutable' \gset
SELECT marks() = :marks_before AS no_marks_written, count(*) > 0 AS records_under_new_gen FROM sift._test_col_log_records('events_c') WHERE gen = :new_gen;
SELECT * FROM chk('whole segment re-keyed');
SELECT sift._test_col_fold_marks('events_c') > 0 AS folded;
SELECT * FROM chk('folded after the second switch');

DROP TABLE events;
DROP FUNCTION chk(text), marks();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
