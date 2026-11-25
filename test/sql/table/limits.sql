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

-- declared paths follow a renamed column and count while it is jsonb: they are refused when added and come back when the column does
CREATE TABLE wide_doc (doc jsonb) USING sift_table;
SELECT sift.table_set_paths('wide_doc', 'doc', 'doc->a text, doc->b text');
ALTER TABLE wide_doc RENAME COLUMN doc TO moved;
SELECT add_cols('wide_doc', 126);
SELECT add_cols('wide_doc', 124);
ALTER TABLE wide_doc ADD COLUMN extra int;
ALTER TABLE wide_doc ALTER COLUMN moved TYPE text;
ALTER TABLE wide_doc ADD COLUMN extra int;
ALTER TABLE wide_doc ALTER COLUMN moved TYPE jsonb USING moved::jsonb;
ALTER TABLE wide_doc DROP COLUMN c1, DROP COLUMN c2, DROP COLUMN extra;
ALTER TABLE wide_doc ALTER COLUMN moved TYPE jsonb USING moved::jsonb;
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

-- pg_repack swaps a heap copy into the table: its trigger is refused on a sift_table, not on a heap table
CREATE SCHEMA repack;
CREATE FUNCTION repack.repack_trigger() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NULL; END $$;
CREATE FUNCTION repack.other_trigger() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NULL; END $$;
CREATE TABLE repack_heap (id int);
CREATE TABLE repack_tbl (id int) USING sift_table;
CREATE TRIGGER repack_trigger AFTER INSERT ON repack_heap FOR EACH ROW EXECUTE FUNCTION repack.repack_trigger();
CREATE TRIGGER repack_trigger AFTER INSERT ON repack_tbl FOR EACH ROW EXECUTE FUNCTION repack.repack_trigger();
CREATE TRIGGER other_trigger AFTER INSERT ON repack_tbl FOR EACH ROW EXECUTE FUNCTION repack.other_trigger();
SELECT tgname, tgrelid::regclass FROM pg_trigger WHERE tgrelid IN ('repack_heap'::regclass, 'repack_tbl'::regclass) AND NOT tgisinternal ORDER BY 2, 1;
DROP TABLE repack_heap, repack_tbl;
DROP SCHEMA repack CASCADE;

DROP TABLE wide127, wide_doc, wide_part;

-- Format 2.0 holds the 1,600 columns of PostgreSQL: the first it refuses is the one PostgreSQL does, and a part of a wide table keeps its schema out of the header page
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SELECT make_wide(1601);
-- columns c1, c1 + step, ... of an n column table: the ones the rows set
CREATE FUNCTION wide_cols(n int) RETURNS SETOF int LANGUAGE sql AS $$ SELECT k FROM generate_series(1, n, GREATEST(n / 8, 1)) k $$;
CREATE FUNCTION wide_rows(t regclass, n int, nrows int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('INSERT INTO %s (%s) SELECT %s FROM generate_series(1, %s) g', t,
		(SELECT string_agg('c' || k, ', ') FROM wide_cols(n) k), (SELECT string_agg('g * ' || k, ', ') FROM wide_cols(n) k), nrows);
END $$;
CREATE FUNCTION wide_check(t regclass, n int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || '' rows, c1 '' || sum(c1) || '', c%s '' || sum(c%s) FROM %s', (SELECT max(k) FROM wide_cols(n) k), (SELECT max(k) FROM wide_cols(n) k), t) INTO r;
	RETURN r || ', ' || (SELECT count(*) FROM sift.check(t)) || ' problems';
END $$;
CREATE FUNCTION add_named(t regclass, prefix text, n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('ALTER TABLE %s %s', t, (SELECT string_agg('ADD COLUMN ' || prefix || g || ' int', ', ') FROM generate_series(1, n) g));
END $$;
SELECT make_wide(200);
SELECT make_wide(500);
SELECT make_wide(1600);
SELECT n, (SELECT major FROM sift.index_format(('wide' || n)::regclass)) AS format FROM unnest(ARRAY[200, 500, 1600]) n;
SELECT wide_rows('wide200', 200, 3000), wide_rows('wide500', 500, 3000), wide_rows('wide1600', 1600, 3000);
SELECT wide_check('wide200', 200), wide_check('wide500', 500), wide_check('wide1600', 1600);
-- COPY with a column list, the first, the second and the last column of each table
COPY wide200 (c1, c2, c200) FROM STDIN;
900001	1	200
900002	2	400
900003	3	600
\.
COPY wide500 (c1, c2, c500) FROM STDIN;
900001	1	500
900002	2	1000
900003	3	1500
\.
COPY wide1600 (c1, c2, c1600) FROM STDIN;
900001	1	1600
900002	2	3200
900003	3	4800
\.
SELECT (SELECT count(*) FROM wide200 WHERE c1 >= 900000) AS rows200, (SELECT count(*) FROM wide500 WHERE c1 >= 900000) AS rows500, (SELECT sum(c1600) FROM wide1600 WHERE c1 >= 900000) AS last1600;
-- rows on top of the first part: a second one, then a delta of columns, a full row and a delete
SELECT wide_rows('wide200', 200, 1000), wide_rows('wide500', 500, 1000), wide_rows('wide1600', 1600, 1000);
UPDATE wide200 SET c2 = c2 + 1 WHERE c1 % 7 = 0;
UPDATE wide500 SET c2 = c2 + 1 WHERE c1 % 7 = 0;
UPDATE wide1600 SET c2 = c2 + 1 WHERE c1 % 7 = 0;
UPDATE wide200 SET c1 = c1, c2 = 0, c3 = 1, c4 = 1, c5 = 1, c6 = 1, c7 = 1, c8 = 1, c9 = 1, c10 = 1, c11 = 1 WHERE c1 % 11 = 0;
UPDATE wide500 SET c1 = c1, c2 = 0, c3 = 1, c4 = 1, c5 = 1, c6 = 1, c7 = 1, c8 = 1, c9 = 1, c10 = 1, c11 = 1 WHERE c1 % 11 = 0;
UPDATE wide1600 SET c1 = c1, c2 = 0, c3 = 1, c4 = 1, c5 = 1, c6 = 1, c7 = 1, c8 = 1, c9 = 1, c10 = 1, c11 = 1 WHERE c1 % 11 = 0;
DELETE FROM wide200 WHERE c1 % 13 = 0;
DELETE FROM wide500 WHERE c1 % 13 = 0;
DELETE FROM wide1600 WHERE c1 % 13 = 0;
SELECT wide_check('wide200', 200), wide_check('wide500', 500), wide_check('wide1600', 1600);
SELECT sift.maintain('wide200') IS NOT NULL, sift.maintain('wide500') IS NOT NULL, sift.maintain('wide1600') IS NOT NULL;
SELECT wide_check('wide200', 200), wide_check('wide500', 500), wide_check('wide1600', 1600);
VACUUM wide200;
VACUUM wide500;
VACUUM wide1600;
SELECT wide_check('wide200', 200), wide_check('wide500', 500), wide_check('wide1600', 1600);
VACUUM FULL wide200;
VACUUM FULL wide500;
VACUUM FULL wide1600;
SELECT wide_check('wide200', 200), wide_check('wide500', 500), wide_check('wide1600', 1600);
-- a column more than the table holds is PostgreSQL's refusal, a dropped column keeps its number, and a table that grows to the limit by ALTER TABLE keeps working
ALTER TABLE wide1600 ADD COLUMN one_more int;
SELECT add_named('wide500', 'x', 1100);
SELECT add_named('wide500', 'y', 1);
INSERT INTO wide500 (c1, x1100) VALUES (-1, 1100);
ALTER TABLE wide500 DROP COLUMN x1, DROP COLUMN c2;
SELECT count(*), sum(c1), sum(x1100) FROM wide500;
VACUUM FULL wide500;
SELECT wide_check('wide200', 200) AS wide200, (SELECT count(*) FROM sift.check('wide500')) AS wide500_problems, (SELECT count(*) FROM sift.check('wide1600')) AS wide1600_problems;
SELECT make_wide(1598);
ALTER TABLE wide1598 ADD COLUMN extra1 int DEFAULT 7, ADD COLUMN extra2 int;
INSERT INTO wide1598 (c1, extra2) VALUES (1, 2);
SELECT count(*), sum(extra1), sum(extra2) FROM wide1598;
ALTER TABLE wide1598 ADD COLUMN extra3 int;
SELECT count(*) FROM sift.check('wide1598');
DROP TABLE wide200, wide500, wide1600, wide1598;
RESET sift.test_create_format1;

-- the resource managers of a published sift_table are registered by a preloaded library only; this server loads it on first use
SELECT sift._tbl_rmgr_registered();

DROP FUNCTION make_wide(int), add_cols(regclass, int), wide_cols(int), wide_rows(regclass, int, int), wide_check(regclass, int), add_named(regclass, text, int);
DROP EXTENSION pg_sift;
