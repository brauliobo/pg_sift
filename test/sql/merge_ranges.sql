-- A merge of segments by several processes: the participants write ranges of the terms straight from the dictionaries of the
-- input segments and the process of the merge stitches them. The merged segment is the one a merge by one process writes, byte for byte.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pageinspect;
SET maintenance_work_mem = '64MB';
SET sift.test_range_bytes = 4000;

CREATE FUNCTION idx_digest(r regclass) RETURNS text LANGUAGE sql AS $$
	SELECT md5(string_agg(substring(get_raw_page(r::text, n::int) FROM 11), ''::bytea ORDER BY n))
	FROM generate_series(0, pg_relation_size(r) / 8192 - 1) n
$$;

CREATE TABLE mr (id int, body text);

-- two indexes made alike, the segments of one merged by the backend alone, the other with w workers
CREATE FUNCTION prep(opts text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DROP INDEX IF EXISTS mr_a;
	DROP INDEX IF EXISTS mr_b;
	TRUNCATE mr;
	EXECUTE format('CREATE INDEX mr_a ON mr USING sift (body) WITH (%s)', opts);
	EXECUTE format('CREATE INDEX mr_b ON mr USING sift (body) WITH (%s)', opts);
	FOR b IN 1..4 LOOP
		INSERT INTO mr (id, body)
		SELECT i, 'k' || (i % 7) || ' g' || (i % 300) || ' z' || (i * i % 997) || ' common word' || (i % 50) || ' ' || repeat('filler ', i % 5)
			|| CASE WHEN i % 3 = 0 THEN ' rare' || (i % 500) ELSE '' END
		FROM generate_series(b * 10000 - 9999, b * 10000) i;
		PERFORM sift.promote('mr_a'), sift.promote('mr_b');
	END LOOP;
END
$$;

CREATE FUNCTION merged(w int, force bool DEFAULT false) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('max_parallel_maintenance_workers', '0', false);
	PERFORM sift.merge('mr_a', force);
	PERFORM set_config('max_parallel_maintenance_workers', w::text, false);
	PERFORM set_config('sift.test_par_workers', '0', false);
	PERFORM set_config('sift.test_range_pieces', '0', false);
	PERFORM sift.merge('mr_b', force);
END
$$;

CREATE FUNCTION same() RETURNS TABLE (same_bytes bool, workers int, ranges int, segments_left int) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift.verify('mr_a', true);
	PERFORM sift.verify('mr_b', true);
	same_bytes := idx_digest('mr_a') = idx_digest('mr_b');
	workers := current_setting('sift.test_par_workers')::int;
	ranges := current_setting('sift.test_range_pieces')::int;
	SELECT count(*)::int INTO segments_left FROM sift.segments('mr_b') WHERE kind = 'immutable';
	RETURN NEXT;
END
$$;

SELECT prep('merge_fanin = 4');
SELECT merged(1);
SELECT * FROM same();
SELECT prep('merge_fanin = 4');
SELECT merged(2);
SELECT * FROM same();
SELECT prep('merge_fanin = 4');
SELECT merged(4);
SELECT * FROM same();
-- the merge of every segment, and an index without positions
SELECT prep('merge_fanin = 8');
SELECT merged(3, true);
SELECT * FROM same();
SELECT prep('merge_fanin = 4, positions = off');
SELECT merged(2);
SELECT * FROM same();
-- segments with vacuumed docs, merged by the VACUUM of the backend alone and then with workers
SELECT prep('merge_fanin = 8');
DELETE FROM mr WHERE id % 7 = 0;
SET max_parallel_maintenance_workers = 0;
VACUUM mr;
SELECT idx_digest('mr_a') AS serial_digest \gset
SELECT prep('merge_fanin = 8');
DELETE FROM mr WHERE id % 7 = 0;
SET max_parallel_maintenance_workers = 2;
SET sift.test_par_workers = 0;
SET sift.test_range_pieces = 0;
VACUUM mr;
RESET max_parallel_maintenance_workers;
SELECT idx_digest('mr_a') = :'serial_digest' AS same_bytes, current_setting('sift.test_par_workers')::int AS workers,
	current_setting('sift.test_range_pieces')::int AS ranges;
-- a segment of an older format
SET sift.test_segfmt = 2;
SELECT prep('merge_fanin = 4');
RESET sift.test_segfmt;
SELECT merged(2);
SELECT * FROM same();

-- a participant, a worker or the process of the merge, that fails in a range: the merge ends, nothing changed, the next one is whole
DROP INDEX IF EXISTS mr_b;
DROP INDEX IF EXISTS mr_a;
TRUNCATE mr;
CREATE INDEX mr_b ON mr USING sift (body) WITH (merge_fanin = 4);
INSERT INTO mr (body) SELECT 'k' || (i % 7) || ' g' || (i % 300) || ' common' FROM generate_series(1, 40000) i;
SELECT sift.promote('mr_b');
INSERT INTO mr (body) SELECT 'k' || (i % 7) || ' g' || (i % 300) || ' common' FROM generate_series(1, 40000) i;
SELECT sift.promote('mr_b');
INSERT INTO mr (body) SELECT 'k' || (i % 7) || ' g' || (i % 300) || ' common' FROM generate_series(1, 40000) i;
SELECT sift.promote('mr_b');
INSERT INTO mr (body) SELECT 'k' || (i % 7) || ' g' || (i % 300) || ' common' FROM generate_series(1, 40000) i;
SELECT sift.promote('mr_b');
SELECT count(*) AS segments FROM sift.segments('mr_b') WHERE kind = 'immutable';
SET max_parallel_maintenance_workers = 3;
SET sift.test_fail_merge = 1;
SET sift.test_fail_who = 2;
SELECT sift.merge('mr_b');
SET sift.test_fail_who = 1;
SELECT sift.merge('mr_b');
RESET sift.test_fail_merge;
RESET sift.test_fail_who;
SELECT count(*) AS segments_after_failures FROM sift.segments('mr_b') WHERE kind = 'immutable';
SELECT sift.verify('mr_b', true);
SELECT count(*) AS workers_left FROM pg_stat_activity WHERE backend_type = 'parallel worker';
SET sift.test_par_workers = 0;
SELECT sift.merge('mr_b') AS merged;
SELECT current_setting('sift.test_par_workers')::int > 0 AS parallel;
SELECT count(*) AS segments_after FROM sift.segments('mr_b') WHERE kind = 'immutable';
SELECT sift.verify('mr_b', true);
SELECT count(*) FROM mr WHERE body @@> 'common';

DROP TABLE mr;
DROP FUNCTION same();
DROP FUNCTION merged(int, bool);
DROP FUNCTION prep(text);
DROP FUNCTION idx_digest(regclass);
DROP EXTENSION pageinspect;
DROP EXTENSION pg_sift;
