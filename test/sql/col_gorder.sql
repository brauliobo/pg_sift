-- The global order of a leaf: for each column of order_columns the segment keeps every row's key and heap
-- address sorted, in chunks. The array is the table sorted by (key, address) with the rows that have no value last, for every type it covers, over NULLs and ties, in a
-- segment written by a build, a promotion and a merge; col_verify checks it; an index without the option, or with segments written before it, has none.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET client_min_messages = notice;
\set VERBOSITY terse

CREATE TABLE go (id int, a int2, b int, c int8, d date, t timestamp, z timestamptz, pad text DEFAULT repeat('x', 200));
ALTER TABLE go ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO go (id, a, b, c, d, t, z)
SELECT i,
	   CASE WHEN i % 29 = 0 THEN NULL ELSE (i * 7) % 60 - 30 END,
	   CASE WHEN i % 31 = 0 THEN NULL ELSE (i * 7919) % 5000 END,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE ((i * 104729) % 100003)::int8 * 5000000 - 200000000000 END,
	   CASE WHEN i % 41 = 0 THEN NULL ELSE DATE '1990-01-01' + (i * 13) % 20000 END,
	   CASE WHEN i % 43 = 0 THEN NULL ELSE TIMESTAMP '2000-01-01' + ((i * 7919) % 900000) * interval '1 second' END,
	   CASE WHEN i % 47 = 0 THEN NULL ELSE TIMESTAMPTZ '2000-01-01 00:00:00+00' + ((i * 104729) % 900000) * interval '1 second' END
FROM generate_series(1, 6000) i;

-- the key an entry holds for each column, as the table says it
CREATE FUNCTION go_key(leaf int) RETURNS text LANGUAGE sql IMMUTABLE AS
$$ SELECT (ARRAY['id', 'a', 'b', 'c', '(d - DATE ''2000-01-01'')', '((extract(epoch FROM t) - 946684800) * 1000000)::int8',
				 '((extract(epoch FROM z) - 946684800) * 1000000)::int8'])[leaf + 1] $$;

-- the array, read back, holds the live rows of the table sorted by (key, address), the rows with no value last by address (the entries of rows that died stay until the segment is rewritten: the heap says which are live)
CREATE FUNCTION go_check(idx regclass, leaf int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format($q$
		WITH want AS (
			SELECT row_number() OVER (ORDER BY (%1$s) IS NULL, %1$s, ctid) - 1 AS pos, ctid AS tid, %1$s AS key, (%1$s) IS NULL AS null_row FROM go),
		have AS (SELECT row_number() OVER (ORDER BY pos) - 1 AS pos, tid, key, null_row FROM sift._test_col_gorder(%2$L, %3$s) WHERE tid IN (SELECT ctid FROM go))
		SELECT CASE WHEN (SELECT count(*) FROM want) <> (SELECT count(*) FROM have) THEN 'count'
					WHEN EXISTS (SELECT 1 FROM want w JOIN have h USING (pos) WHERE w.tid <> h.tid OR w.null_row <> h.null_row OR (NOT w.null_row AND w.key <> h.key)) THEN 'differs'
					ELSE 'ok' END $q$, go_key(leaf), idx, leaf) INTO r;
	RETURN r;
END $$;

CREATE INDEX go_i ON go USING sift_col (id sift.col_int4_ops, a sift.col_int2_ops, b sift.col_int4_ops, c sift.col_int8_ops, d sift.col_date_ops, t sift.col_timestamp_ops,
	z sift.col_timestamptz_ops) WITH (order_columns = 'id, a, b, c, d, t, z', order_array_columns = 'id, a, b, c, d, t, z');
SELECT leaf, go_check('go_i', leaf) FROM generate_series(0, 6) leaf;
SELECT sift.col_verify('go_i');

SELECT leaf, count(*) = (SELECT count(*) FROM go) AS every_row
FROM generate_series(0, 6) leaf, LATERAL sift._test_col_gorder('go_i', leaf) GROUP BY leaf ORDER BY leaf;

-- DML, a promotion and a merge: the segments written afterwards have arrays too, and the live rows are in order in the merged one
INSERT INTO go (id, a, b, c, d, t, z) SELECT 6000 + i, (i * 3) % 40, NULLIF(i % 17, 0), i::int8 * 1000, DATE '2021-01-01' + i % 300, TIMESTAMP '2021-01-01' + i * interval '1 minute', NULL FROM generate_series(1, 1000) i;
DELETE FROM go WHERE id % 11 = 0;
UPDATE go SET b = coalesce(b, 0) + 1000, c = coalesce(c, 0) + 1 WHERE id % 7 = 0;
VACUUM go;
SELECT sift.col_promote('go_i'::regclass) >= 0 AS promoted;
SELECT sift.col_merge('go_i'::regclass, true) >= 0 AS merged;
SELECT count(*) AS segments FROM sift.col_segments('go_i') WHERE kind = 'immutable';
SELECT leaf, go_check('go_i', leaf) FROM generate_series(0, 6) leaf;
SELECT sift.col_verify('go_i');

-- an index without the option has no array, and the option on an index that has segments without one writes it for the segments written afterwards only
CREATE INDEX go_n ON go USING sift_col (id sift.col_int4_ops, b sift.col_int4_ops) WITH (order_columns = 'id, b');
SELECT count(*) AS entries FROM sift._test_col_gorder('go_n', 0);
ALTER INDEX go_n SET (order_array_columns = 'id, b');
INSERT INTO go (id, b) SELECT 30000 + i, i FROM generate_series(1, 100) i;
SELECT sift.col_promote('go_n'::regclass) >= 0 AS promoted;
SELECT count(*) FILTER (WHERE format < 21) AS old_segments, count(*) FILTER (WHERE format >= 21) AS new_segments FROM sift.col_segments('go_n') WHERE kind = 'immutable';
SELECT count(*) AS entries FROM sift._test_col_gorder('go_n', 0);
SELECT sift.col_verify('go_n');
-- sift.upgrade_commands() lists the REINDEX that gives the older segments their array
SELECT kind, object, command FROM sift.upgrade_commands() WHERE reason ~ 'order_array';
REINDEX INDEX go_n;
SELECT count(*) AS commands FROM sift.upgrade_commands() WHERE reason ~ 'order_array';
DROP INDEX go_n;

-- the array extends the sorted run: a subset of order_columns gets it, a column outside order_columns or of another type is refused
CREATE INDEX go_x ON go USING sift_col (id sift.col_int4_ops, b sift.col_int4_ops, pad sift.col_text_ops) WITH (order_columns = 'id, b', order_array_columns = 'id');
SELECT leaf, (SELECT count(*) FROM sift._test_col_gorder('go_x', leaf)) AS entries FROM generate_series(0, 2) leaf;
DROP INDEX go_x;
CREATE INDEX go_x ON go USING sift_col (id sift.col_int4_ops, b sift.col_int4_ops) WITH (order_columns = 'id', order_array_columns = 'b');

-- the setting 14 keeps the sorted runs and no array
SET sift.col_format_minor = 14;
CREATE INDEX go_m ON go USING sift_col (id sift.col_int4_ops, b sift.col_int4_ops) WITH (order_columns = 'id, b', order_array_columns = 'id, b');
RESET sift.col_format_minor;
SELECT count(*) AS entries FROM sift._test_col_gorder('go_m', 0);
SELECT max(format) < 21 AS older_format FROM sift.col_segments('go_m') WHERE kind = 'immutable';
DROP INDEX go_m;

-- a segment too small for a chunk, and an empty table
CREATE TABLE go_e (a int, b int);
CREATE INDEX go_e_i ON go_e USING sift_col (a sift.col_int4_ops, b sift.col_int4_ops) WITH (order_columns = 'a, b', order_array_columns = 'a, b');
INSERT INTO go_e VALUES (3, NULL), (1, 2), (NULL, 2), (1, 1);
SELECT sift.col_promote('go_e_i'::regclass) >= 0 AS promoted;
SELECT seg, pos, key, tid, null_row FROM sift._test_col_gorder('go_e_i', 0) ORDER BY seg, pos;
SELECT sift.col_verify('go_e_i');

-- a heap address directory (point_tid_columns) and a global order in one segment: the point frame holds both
CREATE INDEX go_p ON go USING sift_col (id sift.col_int4_ops, b sift.col_int4_ops, t sift.col_timestamp_ops) WITH (point_tid_columns = 'id, b', order_columns = 't', order_array_columns = 't');
SELECT sift.col_verify('go_p');
SELECT count(*) = (SELECT count(*) FROM go) AS every_row FROM sift._test_col_gorder('go_p', 2);
DROP INDEX go_p;

DROP TABLE go, go_e CASCADE;
DROP FUNCTION go_key(int), go_check(regclass, int);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
