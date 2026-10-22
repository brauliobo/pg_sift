-- A sift_table holds at most 127 columns: the limit is refused when the table is created and when a column is added to it or the leaves of
-- declared paths come back, with the same message, so that it is not the next write that fails.
CREATE EXTENSION pg_sift;
\set VERBOSITY terse
CREATE FUNCTION make_wide(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE wide%s (%s) USING sift_table', n, (SELECT string_agg('c' || g || ' int', ', ') FROM generate_series(1, n) g));
END $$;
CREATE FUNCTION add_cols(t regclass, n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('ALTER TABLE %s %s', t, (SELECT string_agg('ADD COLUMN c' || g || ' int', ', ') FROM generate_series(1, n) g));
END $$;
SELECT make_wide(128);
SELECT make_wide(127);
INSERT INTO wide127 (c1) VALUES (1);
ALTER TABLE wide127 ADD COLUMN extra int;
ALTER TABLE wide127 ADD COLUMN extra int, ADD COLUMN extra2 int;
INSERT INTO wide127 (c1) VALUES (2);
ALTER TABLE wide127 DROP COLUMN c127;
ALTER TABLE wide127 ADD COLUMN extra int;
ALTER TABLE wide127 ADD COLUMN extra2 int;
INSERT INTO wide127 (c1, extra) VALUES (3, 3);
SELECT count(*), sum(c1), sum(extra) FROM wide127;

-- declared paths count again when their column is jsonb and has their name
CREATE TABLE wide_doc (doc jsonb) USING sift_table;
SELECT sift.table_set_paths('wide_doc', 'doc', 'doc->a text, doc->b text');
ALTER TABLE wide_doc RENAME COLUMN doc TO moved;
SELECT add_cols('wide_doc', 126);
ALTER TABLE wide_doc RENAME COLUMN moved TO doc;
ALTER TABLE wide_doc ALTER COLUMN moved TYPE text;
ALTER TABLE wide_doc RENAME COLUMN moved TO doc;
ALTER TABLE wide_doc ALTER COLUMN doc TYPE jsonb USING doc::jsonb;
ALTER TABLE wide_doc DROP COLUMN c1, DROP COLUMN c2;
ALTER TABLE wide_doc ALTER COLUMN doc TYPE jsonb USING doc::jsonb;
INSERT INTO wide_doc (c3) VALUES (3);

-- a partition is checked with its parent
CREATE TABLE wide_part (id int) PARTITION BY RANGE (id);
SELECT add_cols('wide_part', 125);
CREATE TABLE wide_part_1 PARTITION OF wide_part FOR VALUES FROM (0) TO (10) USING sift_table;
ALTER TABLE wide_part ADD COLUMN p1 int;
ALTER TABLE wide_part ADD COLUMN p2 int;
SELECT count(*) AS columns FROM pg_attribute WHERE attrelid = 'wide_part_1'::regclass AND attnum > 0 AND NOT attisdropped;

-- a table that 3.2 let grow past the limit fails its writes, and DROP COLUMN, which is not checked, narrows it
ALTER EVENT TRIGGER sift_table_ddl_check DISABLE;
ALTER TABLE wide127 ADD COLUMN extra3 int;
ALTER EVENT TRIGGER sift_table_ddl_check ENABLE;
INSERT INTO wide127 (c1) VALUES (4);
ALTER TABLE wide127 DROP COLUMN extra3;
INSERT INTO wide127 (c1) VALUES (5);
VACUUM wide127;
SELECT count(*), sum(c1) FROM wide127;
SELECT vblocks_left > 4000000000 AS tids_left FROM sift.table_space('wide127');

DROP TABLE wide127, wide_doc, wide_part;
DROP FUNCTION make_wide(int), add_cols(regclass, int);
DROP EXTENSION pg_sift;
