-- key_cut with point_tid_columns: the units of a key-cut segment are not block ranges, so a heap address names no unit, and the
-- point directories of such a segment list the rows of the segment instead. The access method's answers (an index scan, an index-only scan, a bitmap scan) are
-- the heap's for an equality and an IN list on the key-cut column and on the others, through the write buffer, DML, VACUUM, a merge and a REINDEX, and
-- sift.col_verify accepts the segments.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE kpt (id int, cust int, k int, s text, pad text DEFAULT repeat('x', 200));
ALTER TABLE kpt ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO kpt (id, cust, k, s)
SELECT i, (i * 7919) % 97, CASE WHEN i % 53 = 0 THEN NULL ELSE (i * 31) % 9000 END, 's' || (i * 13) % 5000
FROM generate_series(1, 40000) i;
CREATE INDEX kpt_idx ON kpt USING sift_col (id, cust, k, s) WITH (count_columns = 'off', key_cut = 'cust', point_tid_columns = 'id, cust, k, s');
VACUUM (ANALYZE) kpt;
SELECT DISTINCT least(format, 15) AS format FROM sift.col_segments('kpt_idx') WHERE kind = 'immutable';
SELECT sift.col_verify('kpt_idx', true);

-- the answer of the access method three ways against the heap's
CREATE FUNCTION kp_run(w text, cols text, mode text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	IF mode = 'seq' THEN
		SET LOCAL sift.enable_col_scan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = off;
	ELSE
		SET LOCAL sift.enable_col_scan = on; SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = off;
		IF mode = 'index' THEN SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off; END IF;
		IF mode = 'indexonly' THEN SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexscan = off; END IF;
		IF mode = 'bitmap' THEN SET LOCAL enable_indexscan = off; SET LOCAL enable_indexonlyscan = off; END IF;
	END IF;
	SET LOCAL max_parallel_workers_per_gather = 0;
	EXECUTE format('SELECT coalesce(string_agg(x::text, '';'' ORDER BY x::text), '''') FROM (SELECT %s FROM kpt WHERE %s) x', cols, w) INTO r;
	RESET sift.enable_col_scan; RESET sift.enable_agg; RESET enable_seqscan; RESET enable_indexscan; RESET enable_indexonlyscan; RESET enable_bitmapscan;
	RETURN r;
END $$;
CREATE FUNCTION kp_check(step text) RETURNS TABLE (after_step text, shape text, result text) LANGUAGE plpgsql AS $$
DECLARE q record; seq text; m text; bad text := '';
BEGIN
	FOR q IN SELECT * FROM (VALUES
		('id = 777', 'id, cust'), ('id = 39999', 'id, cust'), ('id = 40001', 'id, cust'),
		('id IN (5, 500, 5000, 15000, 25000, 35000)', 'id, cust'),
		('cust = 42', 'id, cust'), ('cust IN (1, 42, 96)', 'id, cust'),
		('k = 1234', 'id, k'), ('k IN (10, 20, 30, 40, 50)', 'id, k'), ('k IS NULL', 'id'),
		('s = ''s4321''', 'id, s'), ('s IN (''s1'', ''s2'', ''s4999'')', 'id, s'),
		('cust = 42 AND k > 4500', 'id, cust, k'), ('id = 777 AND cust = 777 * 7919 % 97', 'id, cust')) AS v(w, cols)
	LOOP
		seq := kp_run(q.w, q.cols, 'seq');
		bad := '';
		FOREACH m IN ARRAY ARRAY['index', 'indexonly', 'bitmap'] LOOP
			IF kp_run(q.w, q.cols, m) IS DISTINCT FROM seq THEN bad := bad || ' ' || m; END IF;
		END LOOP;
		after_step := step; shape := q.w; result := CASE WHEN bad = '' THEN 'same' ELSE 'DIFFERENT:' || bad END;
		RETURN NEXT;
	END LOOP;
END $$;
SELECT * FROM kp_check('built');

INSERT INTO kpt (id, cust, k, s) SELECT 40000 + i, 42, i, 's' || i FROM generate_series(1, 50) i;
SELECT * FROM kp_check('write buffer');
SELECT sift.col_promote('kpt_idx') > 0 AS promoted;
SELECT * FROM kp_check('promoted');
DELETE FROM kpt WHERE id % 11 = 0;
UPDATE kpt SET k = k + 1, cust = 42 WHERE id % 1009 = 0;
SELECT * FROM kp_check('deleted and updated');
VACUUM kpt;
SELECT * FROM kp_check('vacuumed');
SELECT sift.col_merge('kpt_idx', true) >= 0 AS merged;
SELECT * FROM kp_check('merged');
SELECT sift.col_verify('kpt_idx', true);
REINDEX INDEX kpt_idx;
SELECT * FROM kp_check('reindexed');
SELECT sift.col_verify('kpt_idx', true);
SELECT DISTINCT least(format, 15) AS format FROM sift.col_segments('kpt_idx') WHERE kind = 'immutable';

-- 'off' lists nothing: no notice about the rows of a key-cut segment
CREATE INDEX kpt_off ON kpt USING sift_col (id, cust) WITH (key_cut = 'cust', point_tid_columns = 'off');
REINDEX INDEX kpt_off;
DROP INDEX kpt_off;

DROP FUNCTION kp_run(text, text, text), kp_check(text);
DROP TABLE kpt;
DROP EXTENSION pg_sift;
