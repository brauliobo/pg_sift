-- Format 2.0 sift_table: a key_order that is not the primary key, set before the rows, after them, or in the transaction that writes them, leaves parts whose key bounds are those
-- of their rows: sift.check() finds nothing after the writes, the maintenance and VACUUM, and every table answers as its heap twin.
CREATE EXTENSION IF NOT EXISTS pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET client_min_messages = warning;

CREATE TABLE twin (id int PRIMARY KEY, grp int NOT NULL, v int) USING heap;
INSERT INTO twin SELECT i, i % 9, i % 101 FROM generate_series(1, 6000) i;
UPDATE twin SET v = v + 1 WHERE id % 5 = 0;
DELETE FROM twin WHERE id % 7 = 0;

CREATE TABLE cases (tbl text PRIMARY KEY, key_order text NOT NULL, mode text NOT NULL, after_writes int, after_maintain int, after_vacuum int);

CREATE PROCEDURE run_case(n int, ko text, mode text) LANGUAGE plpgsql AS $$
DECLARE t text := format('st_%s', n);
BEGIN
	EXECUTE format('CREATE TABLE %I (id int PRIMARY KEY, grp int NOT NULL, v int) USING sift_table', t);
	IF mode = 'before' THEN PERFORM sift.table_set_option(t, 'key_order', ko); END IF;
	EXECUTE format('INSERT INTO %I SELECT i, i %% 9, i %% 101 FROM generate_series(1, 3000) i', t);
	IF mode <> 'in_xact' THEN COMMIT; END IF;
	IF mode = 'after' THEN PERFORM sift.table_set_option(t, 'key_order', ko); END IF;
	IF mode = 'in_xact' THEN PERFORM sift.table_maintain(t); PERFORM sift.table_set_option(t, 'key_order', ko); END IF;
	EXECUTE format('INSERT INTO %I SELECT i, i %% 9, i %% 101 FROM generate_series(3001, 6000) i', t);
	EXECUTE format('UPDATE %I SET v = v + 1 WHERE id %% 5 = 0', t);
	EXECUTE format('DELETE FROM %I WHERE id %% 7 = 0', t);
	IF mode <> 'in_xact' THEN COMMIT; END IF;
	INSERT INTO cases VALUES (t, ko, mode, (SELECT count(*) FROM sift.check(t::regclass, true)));
	PERFORM sift.table_maintain(t);
	UPDATE cases SET after_maintain = (SELECT count(*) FROM sift.check(t::regclass, true)) WHERE tbl = t;
END $$;

CALL run_case(1, 'id', 'before');
CALL run_case(2, 'grp', 'before');
CALL run_case(3, 'v', 'before');
CALL run_case(4, 'grp, id', 'before');
CALL run_case(5, 'id, grp', 'before');
CALL run_case(6, 'grp desc, id', 'before');
CALL run_case(7, 'grp', 'after');
CALL run_case(8, 'v', 'after');
CALL run_case(9, 'grp, id', 'after');
CALL run_case(10, 'id, grp', 'after');
CALL run_case(11, 'grp desc, id', 'after');
CALL run_case(12, 'grp', 'in_xact');
CALL run_case(13, 'v', 'in_xact');
CALL run_case(14, 'grp, id', 'in_xact');
CALL run_case(15, 'id, grp', 'in_xact');
CALL run_case(16, 'grp desc, id', 'in_xact');

VACUUM st_1, st_2, st_3, st_4, st_5, st_6, st_7, st_8, st_9, st_10, st_11, st_12, st_13, st_14, st_15, st_16;

CREATE FUNCTION same_as_twin(t regclass) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE mine text; want text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, '','' ORDER BY id) FROM %s r', t) INTO mine;
	SELECT string_agg(r::text, ',' ORDER BY id) INTO want FROM twin r;
	RETURN mine = want;
END $$;

UPDATE cases SET after_vacuum = (SELECT count(*) FROM sift.check(tbl::regclass, true));

SELECT tbl, key_order, mode, after_writes, after_maintain, after_vacuum, same_as_twin(tbl::regclass) AS same_as_twin
FROM cases ORDER BY length(tbl), tbl;
SELECT count(*) AS problems FROM sift.check();

DROP PROCEDURE run_case(int, text, text);
DROP FUNCTION same_as_twin(regclass);
DROP TABLE cases, twin, st_1, st_2, st_3, st_4, st_5, st_6, st_7, st_8, st_9, st_10, st_11, st_12, st_13, st_14, st_15, st_16;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
