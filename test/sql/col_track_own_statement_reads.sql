-- A statement that ends row versions (DELETE, UPDATE, MERGE, INSERT .. ON CONFLICT DO UPDATE) is heard by the statement triggers of an index with track_changes, which fire after the
-- statement has run and write the tombstones of the versions it ended. A read of the same transaction that runs before them (an AFTER ROW trigger, a trigger of the table whose name
-- sorts before the ones of the index, a function the statement calls, a BEFORE trigger of the next statement) saw the log without those tombstones and counted the versions: a
-- read through the index (heap-free, a count by a group of cells, a filtered count, an order walk) gave other rows than the heap. The index reads the heap for the table while a
-- statement that changes it runs (sift_col_ends_rows). Every case runs the statement with a trigger or function that reads three ways (through the index, through the index with
-- the heap checked, the heap) inside the statement or the transaction, and the answers must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

-- the table as a VACUUM leaves it: the rows are in the stored segments, stamped, and the log is read without the heap
CREATE FUNCTION rebuild() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DROP TABLE IF EXISTS ev;
	CREATE TABLE ev (id int PRIMARY KEY, tenant int NOT NULL, kind int NOT NULL, amount int NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 100);
	INSERT INTO ev SELECT i, i % 5, i % 7, i FROM generate_series(1, 20000) i;
	CREATE INDEX ev_c ON ev USING sift_col (id, tenant, kind, amount);
END $$;
SELECT rebuild();
VACUUM (FREEZE, ANALYZE) ev;
CREATE SEQUENCE ins START 40001;
CREATE TABLE rd (label text, shape text, via_index text, via_index_checked text, via_heap text, sift_plan bool);

-- the answer of a query in one of three ways: 0 through the index, 1 through the index with the heap checked, 2 the heap
CREATE FUNCTION answer(q text, mode int, OUT r text, OUT plan text) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	plan := '';
	PERFORM set_config('sift.enable_agg', (mode < 2)::text, true);
	PERFORM set_config('sift.enable_topn', (mode < 2)::text, true);
	PERFORM set_config('sift.test_no_heapfree', (mode = 1)::text, true);
	EXECUTE 'SELECT coalesce(string_agg(x::text, '';'' ORDER BY x::text), '''') FROM (' || q || ') x' INTO r;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
END $$;

-- the answers of the shapes at this point of the transaction, logged under the label
CREATE FUNCTION read_shapes(label text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE s text; a record; b record; c record; shapes text[] := ARRAY[
	'SELECT tenant, kind, count(*), sum(amount) FROM ev GROUP BY tenant, kind',
	'SELECT count(*) FROM ev',
	'SELECT count(*) FROM ev WHERE tenant = 1 AND kind = 1',
	'SELECT tenant, count(*) FROM ev GROUP BY tenant ORDER BY count(*) DESC, tenant LIMIT 2'];
BEGIN
	FOREACH s IN ARRAY shapes LOOP
		SELECT * INTO a FROM answer(s, 0);
		SELECT * INTO b FROM answer(s, 1);
		SELECT * INTO c FROM answer(s, 2);
		INSERT INTO rd VALUES (label, substr(s, 8, 40), a.r, b.r, c.r, a.plan LIKE '%Custom Scan%');
	END LOOP;
END $$;

CREATE FUNCTION read_trigger() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	IF NOT EXISTS (SELECT 1 FROM rd WHERE label = TG_ARGV[0]) THEN
		PERFORM read_shapes(TG_ARGV[0]);
	END IF;
	RETURN NULL;
END $$;

-- a function the statement calls: it reads at its third call, after two rows of the statement have been changed
CREATE FUNCTION read_third_call(label text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE n int := coalesce(nullif(current_setting('rd.calls', true), '')::int, 0) + 1;
BEGIN
	PERFORM set_config('rd.calls', n::text, true);
	IF n = 3 THEN
		PERFORM read_shapes(label);
	END IF;
	RETURN true;
END $$;

-- the statements: each ends row versions (a key update ends the entry of the old key, the others the row); with_function reads at its third row
CREATE TABLE stmts (dml text PRIMARY KEY, event text NOT NULL, sql text NOT NULL, with_function text NOT NULL);
INSERT INTO stmts VALUES
	('delete',       'DELETE', 'DELETE FROM ev WHERE id % 10 = 3',                      'DELETE FROM ev WHERE id % 10 = 3 AND read_third_call($LABEL$)'),
	('update',       'UPDATE', 'UPDATE ev SET amount = amount + 1 WHERE id % 10 = 4',   'UPDATE ev SET amount = amount + 1 WHERE id % 10 = 4 AND read_third_call($LABEL$)'),
	('update key',   'UPDATE', 'UPDATE ev SET id = id + 100000 WHERE id % 10 = 5',      'UPDATE ev SET id = id + 100000 WHERE id % 10 = 5 AND read_third_call($LABEL$)'),
	('update kind',  'UPDATE', 'UPDATE ev SET kind = (kind + 1) % 7 WHERE id % 10 = 6', 'UPDATE ev SET kind = (kind + 1) % 7 WHERE id % 10 = 6 AND read_third_call($LABEL$)'),
	('merge delete', 'DELETE', 'MERGE INTO ev USING (SELECT id FROM ev WHERE id % 10 = 7) m ON ev.id = m.id WHEN MATCHED THEN DELETE',
	                           'MERGE INTO ev USING (SELECT id FROM ev WHERE id % 10 = 7) m ON ev.id = m.id WHEN MATCHED AND read_third_call($LABEL$) THEN DELETE'),
	('upsert',       'UPDATE', 'INSERT INTO ev SELECT i, 1, 1, 1 FROM generate_series(8, 20000, 10) i ON CONFLICT (id) DO UPDATE SET amount = 7',
	                           'INSERT INTO ev SELECT i, 1, 1, 1 FROM generate_series(8, 20000, 10) i WHERE read_third_call($LABEL$) ON CONFLICT (id) DO UPDATE SET amount = 7'),
	('insert',       'INSERT', 'INSERT INTO ev SELECT nextval(''ins''), 1, 1, 1 FROM generate_series(1, 20)', 'INSERT INTO ev SELECT nextval(''ins''), 1, 1, 1 FROM generate_series(1, 20)');

-- the readers: where the transaction reads (a trigger name sorts before or after the ones of the index, sift_track_*)
CREATE TABLE readers (reader text PRIMARY KEY, trigger_sql text);
INSERT INTO readers VALUES
	('after statement, name before the index''s', 'CREATE TRIGGER a_reader AFTER %s ON ev FOR EACH STATEMENT EXECUTE FUNCTION read_trigger(%L)'),
	('after statement, name after the index''s',  'CREATE TRIGGER z_reader AFTER %s ON ev FOR EACH STATEMENT EXECUTE FUNCTION read_trigger(%L)'),
	('after statement with a transition table',   'CREATE TRIGGER a_reader AFTER %s ON ev REFERENCING OLD TABLE AS o FOR EACH STATEMENT EXECUTE FUNCTION read_trigger(%L)'),
	('after row',                                 'CREATE TRIGGER z_reader AFTER %s ON ev FOR EACH ROW EXECUTE FUNCTION read_trigger(%L)'),
	('function of the statement',                 NULL);

-- every statement with every reader (no transition table for the INSERTs, which have no old rows), each step of a case as a statement of its own at the top level, on a table
-- that has just been vacuumed: the changing statement is the first write of its transaction
\o /dev/null
WITH c AS (
	SELECT row_number() OVER (ORDER BY dml, reader) AS n, dml || ' / ' || reader AS label, s.event, s.sql, s.with_function, r.trigger_sql
	FROM stmts s, readers r WHERE NOT (s.dml IN ('insert', 'upsert') AND r.reader LIKE '%transition%')
)
SELECT q FROM c, LATERAL (VALUES
	(1, 'SELECT rebuild()'),
	(2, 'VACUUM (FREEZE, ANALYZE) ev'),
	(3, CASE WHEN trigger_sql IS NULL THEN 'SELECT set_config(''rd.calls'', ''0'', false)' ELSE format(trigger_sql, event, label) END),
	(4, CASE WHEN trigger_sql IS NULL THEN replace(with_function, '$LABEL$', quote_literal(label)) ELSE sql END)) v(step, q)
ORDER BY n, step \gexec
\o
SELECT rebuild();
VACUUM (FREEZE, ANALYZE) ev;
SELECT label, count(*) AS shapes, bool_and(via_index = via_heap) AS index_equals_heap, bool_and(via_index_checked = via_heap) AS checked_equals_heap,
       bool_and(sift_plan) AS sift_plan
FROM rd GROUP BY label ORDER BY label;

-- the same statements in a transaction that reads after each of them, and a function that deletes and reads at once
TRUNCATE rd;
BEGIN;
DELETE FROM ev WHERE id % 10 = 9;
SELECT read_shapes('delete, next statement');
UPDATE ev SET id = id + 200000 WHERE id % 10 = 1;
SELECT read_shapes('update key, next statement');
COMMIT;
SELECT read_shapes('after the commit');

CREATE FUNCTION delete_then_read() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	DELETE FROM ev WHERE id % 10 = 2;
	PERFORM read_shapes('function, same command');
	PERFORM 1;
	PERFORM read_shapes('function, next command');
END $$;
BEGIN;
SELECT delete_then_read();
COMMIT;

-- a BEFORE trigger of the next statement and a deferred trigger of the commit
CREATE TRIGGER a_before BEFORE DELETE ON ev FOR EACH STATEMENT EXECUTE FUNCTION read_trigger('before the next delete');
CREATE CONSTRAINT TRIGGER z_deferred AFTER DELETE ON ev DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION read_trigger('deferred, at the commit');
BEGIN;
DELETE FROM ev WHERE id BETWEEN 30001 AND 30010;
DELETE FROM ev WHERE id % 10 = 0;
COMMIT;
DROP TRIGGER a_before ON ev;
DROP TRIGGER z_deferred ON ev;
SELECT label, count(*) AS shapes, bool_and(via_index = via_heap) AS index_equals_heap, bool_and(via_index_checked = via_heap) AS checked_equals_heap,
       bool_and(sift_plan) AS sift_plan
FROM rd GROUP BY label ORDER BY label;

DROP FUNCTION rebuild(), delete_then_read(), read_third_call(text), read_trigger(), read_shapes(text), answer(text, int);
DROP SEQUENCE ins;
DROP TABLE stmts, readers, rd, ev;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
