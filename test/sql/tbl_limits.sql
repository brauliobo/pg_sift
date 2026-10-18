-- A sift_table holds at most 127 columns: the limit is refused when the table is created, and a column added past it fails the next write.
CREATE EXTENSION pg_sift;
\set VERBOSITY terse
CREATE FUNCTION make_wide(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE wide%s (%s) USING sift_table', n, (SELECT string_agg('c' || g || ' int', ', ') FROM generate_series(1, n) g));
END $$;
SELECT make_wide(128);
SELECT make_wide(127);
INSERT INTO wide127 (c1) VALUES (1);
ALTER TABLE wide127 ADD COLUMN extra int;
INSERT INTO wide127 (c1) VALUES (2);
ALTER TABLE wide127 DROP COLUMN extra;
INSERT INTO wide127 (c1) VALUES (3);
VACUUM wide127;
SELECT count(*), sum(c1) FROM wide127;
DROP TABLE wide127;
DROP FUNCTION make_wide(int);
DROP EXTENSION pg_sift;
