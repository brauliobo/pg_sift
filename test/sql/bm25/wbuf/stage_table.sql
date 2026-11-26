-- A statement that inserts into a sift_table writes the docs of its rows to the write buffer in batches, as on a heap: WAL records of the
-- index grow with the batches, not one per row; under ON CONFLICT each doc is written at once
CREATE EXTENSION pg_sift;
CREATE TABLE a (id int, body text) USING sift_table;
CREATE TABLE b (id int, body text) USING sift_table;
CREATE INDEX b_idx ON b USING sift (body);
CREATE TABLE u (id int PRIMARY KEY, body text) USING sift_table;
CREATE INDEX u_idx ON u USING sift (body);
CREATE FUNCTION wal_records(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, WAL, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ 'WAL: records=' THEN RETURN substring(l from 'records=(\d+)')::bigint; END IF;
	END LOOP;
	RETURN 0;
END $$;
SELECT wal_records('INSERT INTO b SELECT i, ''w'' || i % 7 FROM generate_series(1, 2000) i')
	- wal_records('INSERT INTO a SELECT i, ''w'' || i % 7 FROM generate_series(1, 2000) i') < 200 AS batched;
SELECT kind, sum(ndocs) AS docs FROM sift.segments('b_idx') GROUP BY 1;
SELECT count(*) FROM b WHERE body @@> 'w3';
INSERT INTO u SELECT i, 'w' || i % 7 FROM generate_series(1, 300) i;
INSERT INTO u SELECT i, 'v' || i % 7 FROM generate_series(201, 400) i ON CONFLICT (id) DO NOTHING;
SELECT count(*) FILTER (WHERE body @@> 'w3') AS w3, count(*) FILTER (WHERE body @@> 'v3') AS v3 FROM u;
SELECT * FROM sift.check('u_idx');
DROP TABLE a, b, u;
DROP FUNCTION wal_records;
DROP EXTENSION pg_sift;
