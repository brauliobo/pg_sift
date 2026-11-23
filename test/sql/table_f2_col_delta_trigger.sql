-- Format 2.0 sift_table column deltas and AFTER UPDATE triggers: an UPDATE of one column that no index holds keeps the row's address (ctid) and writes no index entry; a statement trigger
-- that reads no row keeps that; a row trigger or a transition table, which read OLD and NEW, still see both rows. The rows agree with a heap table at every step.
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE SCHEMA f2cdn;
SET search_path = f2cdn, public;

CREATE FUNCTION chk(name text, got anyelement, want anyelement) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	IF got IS DISTINCT FROM want THEN RAISE EXCEPTION 'FAILED %: got %, want %', name, got, want; END IF;
	RETURN name || ': ok';
END $$;

CREATE TABLE wide (id int PRIMARY KEY, v int, a int, b int, c int) USING sift_table;
CREATE TABLE wide_h (id int PRIMARY KEY, v int, a int, b int, c int) USING heap;
INSERT INTO wide SELECT i, i, i, i, i FROM generate_series(1, 3000) i;
INSERT INTO wide_h SELECT * FROM wide;
SELECT busy FROM sift.table_maintain('wide') \gset
CREATE INDEX wide_b ON wide (b);
CREATE INDEX wide_h_b ON wide_h (b);

CREATE FUNCTION same_rows() RETURNS bool LANGUAGE sql AS $$
	SELECT NOT EXISTS (SELECT * FROM wide EXCEPT SELECT * FROM wide_h) AND NOT EXISTS (SELECT * FROM wide_h EXCEPT SELECT * FROM wide) $$;

SELECT chk('the rows agree to begin with', same_rows(), true);

SELECT ctid::text AS c0 FROM wide WHERE id = 5 \gset
WITH u AS (UPDATE wide SET v = -5 WHERE id = 5 RETURNING ctid::text AS c) SELECT chk('one column that no index holds keeps the ctid', c, :'c0') FROM u;
UPDATE wide_h SET v = -5 WHERE id = 5;
SELECT ctid::text AS c1 FROM wide WHERE id = 7 \gset
WITH u AS (UPDATE wide SET b = -7 WHERE id = 7 RETURNING ctid::text AS c) SELECT chk('a column of an index moves the row', c <> :'c1', true) FROM u;
UPDATE wide_h SET b = -7 WHERE id = 7;
SELECT chk('the index finds the moved row', count(*)::int, 1) FROM wide WHERE b = -7;
SELECT chk('the rows agree after the updates', same_rows(), true);

BEGIN;
UPDATE wide SET v = v + 1 WHERE id <= 2000;
UPDATE wide_h SET v = v + 1 WHERE id <= 2000;
SELECT chk('a bulk update agrees in its transaction', (SELECT sum(v) FROM wide), (SELECT sum(v) FROM wide_h));
ROLLBACK;
UPDATE wide SET v = v + 1 WHERE id <= 2000;
UPDATE wide_h SET v = v + 1 WHERE id <= 2000;
SELECT chk('a bulk update agrees after the commit', same_rows(), true);
VACUUM wide;
SELECT chk('the rows agree after VACUUM', same_rows(), true);

-- AFTER UPDATE triggers: one that reads no row (FOR EACH STATEMENT, no transition table) keeps the delta; one that reads OLD and NEW, by row or by transition table, still sees both rows
CREATE TABLE trig_log (n int);
CREATE FUNCTION log_stmt() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO trig_log VALUES (1);
	RETURN NULL;
END $$;
CREATE TRIGGER wide_stmt AFTER UPDATE ON wide FOR EACH STATEMENT EXECUTE FUNCTION log_stmt();
SELECT ctid::text AS c4 FROM wide WHERE id = 9 \gset
WITH u AS (UPDATE wide SET v = -9 WHERE id = 9 RETURNING ctid::text AS c) SELECT chk('a statement trigger keeps the ctid', c, :'c4') FROM u;
UPDATE wide_h SET v = -9 WHERE id = 9;
SELECT chk('the statement trigger fired once', count(*)::int, 1) FROM trig_log;
DROP TRIGGER wide_stmt ON wide;

CREATE TABLE trig_rows (id int, old_v int, new_v int);
CREATE FUNCTION log_row() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO trig_rows VALUES (NEW.id, OLD.v, NEW.v);
	RETURN NULL;
END $$;
CREATE TRIGGER wide_row AFTER UPDATE ON wide FOR EACH ROW EXECUTE FUNCTION log_row();
SELECT ctid::text AS c5 FROM wide WHERE id = 10 \gset
WITH u AS (UPDATE wide SET v = -10 WHERE id = 10 RETURNING ctid::text AS c) SELECT chk('a row trigger moves the row', c <> :'c5', true) FROM u;
UPDATE wide_h SET v = -10 WHERE id = 10;
SELECT chk('the row trigger saw both rows', (old_v, new_v) = (11, -10), true) FROM trig_rows WHERE id = 10;
DROP TRIGGER wide_row ON wide;

CREATE TABLE trig_tr (old_sum bigint, new_sum bigint);
CREATE FUNCTION log_tr() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO trig_tr SELECT (SELECT sum(v) FROM o_t), (SELECT sum(v) FROM n_t);
	RETURN NULL;
END $$;
CREATE TRIGGER wide_tr AFTER UPDATE ON wide REFERENCING OLD TABLE AS o_t NEW TABLE AS n_t FOR EACH STATEMENT EXECUTE FUNCTION log_tr();
UPDATE wide SET v = v + 1000 WHERE id IN (11, 12);
UPDATE wide_h SET v = v + 1000 WHERE id IN (11, 12);
SELECT chk('the transition tables hold the old and the new rows', (old_sum + 2000 = new_sum), true) FROM trig_tr;
DROP TRIGGER wide_tr ON wide;
SELECT chk('the rows agree after the triggers', same_rows(), true);

RESET search_path;
DROP SCHEMA f2cdn CASCADE;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
