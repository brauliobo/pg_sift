-- The check of the quarantine before a write must never fail a healthy index: every statement kind and transaction state that writes to a table with a sift index runs
-- with a healthy index, with damage the index has not met yet, with an index that is out of service, and with sift.on_corruption = error, and the rows reach the heap each time
-- (the statement that meets the damage fails once).
-- A scenario is a row of `scenario`; a failing one ends the session or leaves a count that is not the rows written.
CREATE EXTENSION pg_sift_test CASCADE;
-- Format 1: the damaged pages are the segment data and write-buffer pages of Format 1, which the reasons name
SET sift.test_create_format1 = on;
SET client_min_messages = error;
SET check_function_bodies = off;
SET min_parallel_index_scan_size = 0;

CREATE TABLE scenario (n serial, label text, body text, continues boolean NOT NULL DEFAULT false);

CREATE PROCEDURE fixtures() LANGUAGE plpgsql AS $$
BEGIN
	DROP TABLE IF EXISTS tc, tt, tb, tu, tl CASCADE;
	DROP FUNCTION IF EXISTS tl_trg();
	CREATE TABLE tc (id int, v int, g int);
	INSERT INTO tc SELECT i, i % 7, i % 3 FROM generate_series(1, 4000) i;
	CREATE INDEX tc_idx ON tc USING sift_col (id, v, g);
	CREATE TABLE tt (id int, v int);
	INSERT INTO tt SELECT i, i % 7 FROM generate_series(1, 2000) i;
	CREATE INDEX tt_idx ON tt USING sift_col (id, v) WITH (track_changes = ctid);
	CREATE INDEX tt_bt1 ON tt (id);
	CREATE INDEX tt_bt2 ON tt (v);
	CREATE TABLE tb (id int, body text);
	INSERT INTO tb SELECT i, 'alpha beta ' || (i % 10) FROM generate_series(1, 2000) i;
	CREATE INDEX tb_idx ON tb USING sift (body);
	CREATE TABLE tu (id int PRIMARY KEY, v int);
	CREATE INDEX tu_idx ON tu USING sift_col (id, v);
	INSERT INTO tu SELECT i, i FROM generate_series(1, 500) i;
	CREATE TABLE tl (id int, v int);
	INSERT INTO tl SELECT i, i FROM generate_series(1, 100) i;
	CREATE INDEX tl_idx ON tl USING sift_col (id, v);
	CREATE FUNCTION tl_trg() RETURNS trigger LANGUAGE plpgsql AS $f$ BEGIN INSERT INTO tl VALUES (NEW.id, NEW.v); RETURN NULL; END $f$;
	CREATE TRIGGER tc_after AFTER INSERT ON tc FOR EACH ROW WHEN (NEW.g = 9) EXECUTE FUNCTION tl_trg();
	CREATE CONSTRAINT TRIGGER tt_deferred AFTER INSERT ON tt DEFERRABLE INITIALLY DEFERRED FOR EACH ROW WHEN (NEW.v = 9) EXECUTE FUNCTION tl_trg();
	ANALYZE;
END $$;

CREATE FUNCTION raise_x() RETURNS void LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'x'; END $$;
CREATE FUNCTION in_exception_blocks() RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO tc VALUES (6001, 1, 1);
	BEGIN
		INSERT INTO tc VALUES (6002, 1, 1);
		PERFORM raise_x();
	EXCEPTION WHEN OTHERS THEN NULL;
	END;
	FOR i IN 1..20 LOOP
		BEGIN
			INSERT INTO tc VALUES (6100 + i, 1, 1);
			INSERT INTO tb VALUES (6100 + i, 'q');
			IF i % 3 = 0 THEN PERFORM raise_x(); END IF;
		EXCEPTION WHEN OTHERS THEN NULL;
		END;
	END LOOP;
	INSERT INTO tc VALUES (6003, 1, 1);
END $$;
CREATE FUNCTION evt_ins() RETURNS event_trigger LANGUAGE plpgsql AS $$ BEGIN INSERT INTO tl VALUES (-1, 1); END $$;
CREATE FUNCTION insert_in_select() RETURNS int LANGUAGE sql AS $$ INSERT INTO tc SELECT 9000 + i, 1, 1 FROM generate_series(1, 20) i RETURNING 1 $$;

INSERT INTO scenario (label, body, continues) VALUES
('multi-row INSERT, autocommit',
 $s$INSERT INTO tc SELECT 1000 + i, 1, 1 FROM generate_series(1, 100) i$s$, false),
('multi-row INSERT, several tables in one statement string',
 $s$INSERT INTO tc SELECT 1100 + i, 1, 1 FROM generate_series(1, 100) i; INSERT INTO tt SELECT 1100 + i, 1 FROM generate_series(1, 100) i; INSERT INTO tb SELECT 1100 + i, 'x y' FROM generate_series(1, 100) i$s$, false),
('single-row INSERT, autocommit',
 $s$INSERT INTO tc VALUES (1201, 1, 1)$s$, false),
('BEGIN, INSERT, COMMIT',
 $s$BEGIN; INSERT INTO tc SELECT 1300 + i, 1, 1 FROM generate_series(1, 100) i; INSERT INTO tt VALUES (1301, 1), (1302, 2); COMMIT$s$, false),
('SAVEPOINT, ROLLBACK TO after rows were held',
 $s$BEGIN; INSERT INTO tc SELECT 1400 + i, 1, 1 FROM generate_series(1, 100) i; SAVEPOINT a; INSERT INTO tc VALUES (1501, 1, 1); ROLLBACK TO a; INSERT INTO tc VALUES (1502, 1, 1); COMMIT$s$, false),
('SAVEPOINT, RELEASE after rows were held',
 $s$BEGIN; INSERT INTO tc VALUES (1601, 1, 1), (1602, 1, 1); INSERT INTO tt VALUES (1601, 1); SAVEPOINT a; INSERT INTO tc VALUES (1603, 1, 1); RELEASE a; COMMIT$s$, false),
('nested SAVEPOINTs',
 $s$BEGIN; INSERT INTO tc VALUES (1701, 1, 1), (1702, 1, 1); SAVEPOINT a; INSERT INTO tc VALUES (1703, 1, 1), (1704, 1, 1); SAVEPOINT b; INSERT INTO tc VALUES (1705, 1, 1); ROLLBACK TO a; RELEASE a; COMMIT$s$, false),
('plpgsql EXCEPTION blocks and a loop of them',
 $s$SELECT in_exception_blocks()$s$, false),
('DO block with an EXCEPTION block in a loop',
 $s$DO $do$ BEGIN FOR i IN 1..10 LOOP BEGIN INSERT INTO tc VALUES (1800 + i, 1, 1), (1900 + i, 1, 1); IF i % 2 = 0 THEN PERFORM raise_x(); END IF; EXCEPTION WHEN OTHERS THEN NULL; END; END LOOP; END $do$$s$, false),
('UPDATE and DELETE of several rows, tracked and not',
 $s$UPDATE tt SET v = v + 1 WHERE id <= 300; DELETE FROM tt WHERE id > 1900; UPDATE tc SET v = v + 1 WHERE id <= 300; DELETE FROM tc WHERE id > 3900$s$, false),
('UPDATE and DELETE in a transaction block',
 $s$BEGIN; UPDATE tt SET v = 0 WHERE id < 10; DELETE FROM tt WHERE id BETWEEN 100 AND 110; SAVEPOINT a; UPDATE tt SET v = 1 WHERE id < 5; ROLLBACK TO a; COMMIT$s$, false),
('PREPARE TRANSACTION',
 $s$BEGIN; INSERT INTO tc SELECT 2100 + i, 1, 1 FROM generate_series(1, 50) i; INSERT INTO tt VALUES (2101, 1); PREPARE TRANSACTION 'qtx1'$s$, false),
('COMMIT PREPARED', $s$COMMIT PREPARED 'qtx1'$s$, false),
('PREPARE TRANSACTION with a SAVEPOINT',
 $s$BEGIN; INSERT INTO tc VALUES (2201, 1, 1); SAVEPOINT s; INSERT INTO tc VALUES (2202, 1, 1); PREPARE TRANSACTION 'qtx2'$s$, false),
('ROLLBACK PREPARED', $s$ROLLBACK PREPARED 'qtx2'$s$, false),
('COPY',
 $s$COPY tc FROM PROGRAM 'seq 3001 3200 | sed "s/$/,1,1/"' CSV$s$, false),
('COPY in a transaction block',
 $s$BEGIN; COPY tc FROM PROGRAM 'seq 3301 3400 | sed "s/$/,1,1/"' CSV; COMMIT$s$, false),
('INSERT ON CONFLICT DO NOTHING',
 $s$INSERT INTO tu SELECT i, 1 FROM generate_series(400, 700) i ON CONFLICT DO NOTHING$s$, false),
('INSERT ON CONFLICT DO UPDATE',
 $s$INSERT INTO tu SELECT i, 2 FROM generate_series(450, 800) i ON CONFLICT (id) DO UPDATE SET v = excluded.v$s$, false),
('MERGE',
 $s$MERGE INTO tu t USING (SELECT i AS id, 9 AS v FROM generate_series(750, 900) i) s ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = s.v WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.v)$s$, false),
('INSERT in a function called by SELECT',
 $s$SELECT insert_in_select() FROM generate_series(1, 3)$s$, false),
('writable CTE',
 $s$WITH w AS (INSERT INTO tc VALUES (9501, 1, 1) RETURNING id) SELECT count(*) FROM w$s$, false),
('AFTER ROW trigger that writes another indexed table',
 $s$INSERT INTO tc SELECT 9600 + i, 1, 9 FROM generate_series(1, 30) i$s$, false),
('deferred constraint trigger that writes at the commit',
 $s$BEGIN; INSERT INTO tt SELECT 9700 + i, 9 FROM generate_series(1, 30) i; COMMIT$s$, false),
('event trigger that writes an indexed table: create', $s$CREATE EVENT TRIGGER evt_w ON ddl_command_end EXECUTE FUNCTION evt_ins()$s$, false),
('event trigger that writes an indexed table: a DDL statement fires it', $s$CREATE TABLE evt_t (a int)$s$, false),
('event trigger that writes an indexed table: drop', $s$DROP TABLE evt_t$s$, false),
('event trigger that writes an indexed table: the end', $s$DROP EVENT TRIGGER evt_w$s$, false),
('read of the rows written in the same transaction',
 $s$BEGIN; INSERT INTO tc SELECT 9800 + i, 1, 1 FROM generate_series(1, 50) i; SELECT count(*) FROM tc WHERE v = 1; SELECT count(*) FROM tc WHERE id > 9800; UPDATE tt SET v = 9 WHERE id < 100; SELECT count(*) FROM tt WHERE v = 9; COMMIT$s$, false),
('cursor held over the commit, SAVEPOINT while a cursor is open',
 $s$BEGIN; INSERT INTO tc SELECT 9900 + i, 1, 1 FROM generate_series(1, 50) i; DECLARE c CURSOR WITH HOLD FOR SELECT id FROM tc WHERE v = 1; FETCH 3 FROM c; SAVEPOINT s; INSERT INTO tc VALUES (9951, 1, 1); ROLLBACK TO s; COMMIT; FETCH 3 FROM c; CLOSE c$s$, false),
('error in a transaction block: SAVEPOINT',
 $s$BEGIN; INSERT INTO tc VALUES (9961, 1, 1); SAVEPOINT a$s$, false),
('error in a transaction block: the error', $s$SELECT 1 / 0$s$, true),
('error in a transaction block: ROLLBACK TO and COMMIT',
 $s$ROLLBACK TO a; INSERT INTO tc VALUES (9962, 1, 1); COMMIT$s$, true),
('error in a transaction block: ROLLBACK', $s$BEGIN; INSERT INTO tc VALUES (9963, 1, 1); SELECT 1 / 0$s$, false),
('error in a transaction block: the end', $s$ROLLBACK$s$, true),
('COMMIT AND CHAIN',
 $s$BEGIN; INSERT INTO tc SELECT 9970 + i, 1, 1 FROM generate_series(1, 5) i; COMMIT AND CHAIN; INSERT INTO tc VALUES (9980, 1, 1); COMMIT$s$, false),
('parallel query after a write in the transaction',
 $s$BEGIN; SET LOCAL parallel_setup_cost = 0; SET LOCAL parallel_tuple_cost = 0; SET LOCAL min_parallel_table_scan_size = 0; SET LOCAL max_parallel_workers_per_gather = 2; SET LOCAL debug_parallel_query = on; INSERT INTO tc SELECT 9990 + i, 1, 1 FROM generate_series(1, 5) i; SELECT count(*) FROM tc WHERE v = 1; SELECT g, count(*) FROM tc GROUP BY g ORDER BY g; COMMIT$s$, false),
('temporary and unlogged table',
 $s$CREATE TEMP TABLE tmp1 (id int, v int); CREATE INDEX tmp1_idx ON tmp1 USING sift_col (id, v); INSERT INTO tmp1 SELECT i, i FROM generate_series(1, 200) i; CREATE UNLOGGED TABLE ul (id int, v int); CREATE INDEX ul_idx ON ul USING sift_col (id, v); INSERT INTO ul SELECT i, i FROM generate_series(1, 200) i; DROP TABLE tmp1, ul$s$, false),
('CREATE INDEX CONCURRENTLY after writes', $s$CREATE INDEX CONCURRENTLY tc_idx2 ON tc USING sift_col (id, v)$s$, false),
('DROP INDEX', $s$DROP INDEX tc_idx2$s$, false),
('REINDEX INDEX CONCURRENTLY', $s$REINDEX INDEX CONCURRENTLY tt_idx$s$, false),
('REINDEX TABLE', $s$REINDEX TABLE tb$s$, false),
('DELETE for VACUUM', $s$DELETE FROM tc WHERE id % 5 = 0; DELETE FROM tb WHERE id % 5 = 0; DELETE FROM tt WHERE id % 5 = 0$s$, false),
('VACUUM', $s$VACUUM tc$s$, false),
('VACUUM of a BM25 index', $s$VACUUM tb$s$, false),
('VACUUM with parallel workers requested', $s$VACUUM (PARALLEL 2) tt$s$, false),
('VACUUM without index cleanup', $s$VACUUM (INDEX_CLEANUP off) tc$s$, false),
('VACUUM ANALYZE', $s$VACUUM (ANALYZE) tb$s$, false),
('VACUUM FULL', $s$VACUUM (FULL) tu$s$, false);


CREATE FUNCTION totals() RETURNS text LANGUAGE sql AS $$
	SELECT concat_ws(' ', (SELECT count(*) || '/' || sum(id) FROM tc), (SELECT count(*) || '/' || sum(id) FROM tt), (SELECT count(*) || '/' || sum(id) FROM tb),
					 (SELECT count(*) || '/' || sum(id) FROM tu), (SELECT count(*) || '/' || sum(id) FROM tl))
$$;
CREATE FUNCTION expect(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN EXECUTE q; RETURN 'ok'; EXCEPTION WHEN OTHERS THEN RETURN SQLSTATE; END $$;
CREATE PROCEDURE poke_all() LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift._test_poke(i, 2, 8176, 9, 2) FROM unnest(ARRAY['tc_idx', 'tt_idx', 'tb_idx', 'tu_idx', 'tl_idx']) i;
END $$;
CREATE PROCEDURE meet_damage() LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('enable_seqscan', 'off', false);
	PERFORM expect($e$ SELECT count(*) FROM tc WHERE v = 3 $e$);
	PERFORM expect($e$ SELECT count(*) FROM tt WHERE v = 3 $e$);
	PERFORM expect($e$ SELECT count(*) FROM tb WHERE body @@> 'alpha 3' $e$);
	PERFORM expect($e$ SELECT count(*) FROM tu WHERE v = 3 $e$);
	PERFORM expect($e$ SELECT count(*) FROM tl WHERE v = 3 $e$);
	PERFORM set_config('enable_seqscan', 'on', false);
END $$;
CREATE PROCEDURE heap_only(off boolean) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config(g, CASE WHEN off THEN 'off' ELSE 'on' END, false) FROM unnest(ARRAY['sift.enable_scan', 'enable_indexscan', 'enable_indexonlyscan', 'enable_bitmapscan']) g;
END $$;

-- the statements of a phase, each after a ROLLBACK that ends the transaction block an earlier failure left open
CREATE VIEW stream AS SELECT s FROM (SELECT n, 0 AS k, 'ROLLBACK' AS s FROM scenario WHERE NOT continues UNION ALL SELECT n, 1, body FROM scenario) x ORDER BY n, k;

SET sift.on_corruption = quarantine;

-- healthy indexes: the guard is not a step of the write, nothing is recorded, and the index holds what the heap holds
CALL fixtures();
\set ECHO none
\o /dev/null
SELECT s FROM stream \gexec
\o
\set ECHO all
CALL heap_only(true);
SELECT totals() AS heap \gset
CALL heap_only(false);
SELECT :'heap' AS heap, totals() = :'heap' AS index_agrees, (SELECT count(*) FROM sift.quarantine) AS quarantined;
SELECT count(*) AS problems FROM (SELECT sift.check(i) FROM unnest(ARRAY['tc_idx', 'tt_idx', 'tb_idx', 'tu_idx', 'tl_idx']) i) c;

-- damage no statement has met yet: a statement that meets it fails once with the error of the damage, ends no session, and the index is out of service for the statements after it
CALL fixtures();
CALL poke_all();
\set ECHO none
\o /dev/null
SELECT s FROM stream \gexec
\o
\set ECHO all
SELECT count(*) AS indexes_out_of_service FROM sift.quarantine;

-- indexes that are out of service: every statement runs, and the heap gets every row
CALL fixtures();
CALL poke_all();
CALL meet_damage();
SELECT index FROM sift.quarantine ORDER BY 1;
\set ECHO none
\o /dev/null
SELECT s FROM stream \gexec
\o
\set ECHO all
CALL heap_only(true);
SELECT totals() = :'heap' AS same_rows_as_healthy;
SELECT count(*) FILTER (WHERE skipped_writes > 0) AS indexes_that_skipped_writes FROM sift.quarantine;

-- sift.on_corruption = error is the behaviour before the guard
SET sift.on_corruption = error;
CALL heap_only(false);
CALL fixtures();
\set ECHO none
\o /dev/null
SELECT s FROM stream \gexec
\o
\set ECHO all
CALL heap_only(true);
SELECT totals() = :'heap' AS same_rows_as_healthy;
SELECT count(*) AS quarantined FROM sift.quarantine;

-- damage that appears between a write and its commit: the flush of an autocommit statement meets it, the statement fails once with the error of the damage, and the index is out of service for the next one
SET sift.on_corruption = quarantine;
CALL fixtures();
CREATE FUNCTION poke_tc() RETURNS int LANGUAGE sql AS $$ SELECT 1 FROM sift._test_poke('tc_idx', 0, 24, 0, 4) $$;
INSERT INTO tc SELECT 80000 + i, 1, CASE WHEN i = 2 THEN poke_tc() ELSE 1 END FROM generate_series(1, 50) i;
SELECT count(*) AS committed_rows FROM tc WHERE id > 80000;
SELECT index FROM sift.quarantine;
DROP FUNCTION poke_tc();

-- the same at the COMMIT of a transaction block: that COMMIT fails once with the error of the damage and the transaction is rolled back, the index is out of service for the next one
CALL fixtures();
BEGIN;
INSERT INTO tc SELECT 80000 + i, 1, 1 FROM generate_series(1, 50) i;
SELECT sift._test_poke('tc_idx', 0, 24, 0, 4) IS NOT NULL AS poked;
COMMIT;
SELECT count(*) AS committed_rows FROM tc WHERE id > 80000;
SELECT index FROM sift.quarantine;
BEGIN;
INSERT INTO tc SELECT 80000 + i, 1, 1 FROM generate_series(1, 50) i;
COMMIT;
SELECT count(*) AS committed_rows FROM tc WHERE id > 80000;

-- the same before a SAVEPOINT: the flush that starts it cannot start a subtransaction of its own, so that statement fails once with the error of the damage, the transaction block ends,
-- and the next transaction writes to the heap with the index skipped
CALL fixtures();
BEGIN;
INSERT INTO tc SELECT 80000 + i, 1, 1 FROM generate_series(1, 50) i;
SELECT sift._test_poke('tc_idx', 0, 24, 0, 4) IS NOT NULL AS poked;
SAVEPOINT s;
ROLLBACK;
SELECT index FROM sift.quarantine;
BEGIN;
INSERT INTO tc SELECT 80000 + i, 1, 1 FROM generate_series(1, 50) i;
SAVEPOINT s;
INSERT INTO tc SELECT 80100 + i, 1, 1 FROM generate_series(1, 50) i;
RELEASE s;
COMMIT;
SELECT count(*) AS committed_rows FROM tc WHERE id > 80000;

DROP VIEW stream;
DROP PROCEDURE fixtures(), poke_all(), meet_damage(), heap_only(boolean);
DROP TABLE scenario, tc, tt, tb, tu, tl CASCADE;
DROP FUNCTION totals(), expect(text), raise_x(), in_exception_blocks(), insert_in_select(), evt_ins(), tl_trg();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
