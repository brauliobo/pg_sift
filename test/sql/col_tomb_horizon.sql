-- A tombstone names the transaction that deleted its row, and the commit log is truncated up to what the tables are frozen up to: a tombstone (and the transaction that made the log
-- complete) may outlive its transaction's entry in the log. A read never asks the log about such a transaction: it goes to the heap for the pages the tombstone is on, and VACUUM writes a
-- tombstone of a committed transaction older than every snapshot as a frozen one and drops the tombstones of the transactions that aborted.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100);
INSERT INTO events SELECT i, i % 10, i, 'x' FROM generate_series(1, 20000) i;
CREATE INDEX events_c ON events USING sift_col (id, tenant_id, counter_a) WITH (track_updates = 'id', track_deletes = on);
VACUUM (FREEZE) events;

CREATE FUNCTION answer(want_sift bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE q text := 'SELECT tenant_id, count(*) AS n, sum(counter_a) AS s FROM events GROUP BY tenant_id'; plan text := ''; l text; r text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF (plan LIKE '%SiftAgg%') <> want_sift THEN
		RAISE EXCEPTION 'the plan is % a SiftAgg plan: %', CASE WHEN want_sift THEN 'not' ELSE 'still' END, plan;
	END IF;
	EXECUTE 'SELECT coalesce(string_agg(format(''%s:%s:%s'', tenant_id, n, s), '' '' ORDER BY tenant_id), ''none'') FROM (' || q || ') q' INTO r;
	RETURN r;
END $$;

CREATE FUNCTION pages() RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT tenant_id, count(*), sum(counter_a) FROM events GROUP BY tenant_id' INTO j;
	RETURN coalesce(jsonb_path_query_first(j, 'strict $.**."Heap Pages"')::int, -1);
END $$;

-- the heap-free read and the oracle (no sift plan), and whether the heap was read
CREATE FUNCTION chk(what text, OUT test text, OUT same_as_no_sift bool, OUT reads_heap bool) LANGUAGE plpgsql AS $$
DECLARE a text; pf int; o text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	a := answer(true);
	pf := pages();
	PERFORM set_config('sift.enable_agg', 'off', true);
	o := answer(false);
	test := what;
	same_as_no_sift := a = o;
	reads_heap := pf > 0;
END $$;

-- the log entries by transaction: how many, and how many are frozen
CREATE FUNCTION log_xids(OUT entries int, OUT frozen int) LANGUAGE sql AS
	$$ SELECT count(*)::int, (count(*) FILTER (WHERE xid::text::bigint = 2))::int FROM sift._test_col_log_entries('events_c') $$;

-- what is known of a transaction: committed, a frozen one, one that aborted, one the server has not given out, and none
SELECT sift._test_xid_state(txid_current()::text::xid) AS own, sift._test_xid_state('2'::xid) AS frozen, sift._test_xid_state('0'::xid) AS invalid,
       sift._test_xid_state((txid_current() + 1000000)::text::xid) AS not_given_out;
CREATE TABLE gone (i int);
INSERT INTO gone VALUES (1);
SELECT sift._test_xid_state(xmin) AS committed FROM gone;
DROP TABLE gone;

SELECT * FROM chk('fresh');
UPDATE events SET counter_a = counter_a + 5 WHERE id % 11 = 0;
DELETE FROM events WHERE id % 13 = 0;
SELECT sift.col_promote('events_c') >= 0 AS promoted;
SELECT * FROM chk('churned, no vacuum');

-- every transaction is older than the commit log keeps: nothing is claimed of the tombstones or of the transaction that made the log complete, the heap answers, with no error
SELECT set_config('sift.test_clog_horizon', (txid_current() + 100)::text, false) IS NOT NULL AS horizon_set;
SELECT * FROM chk('beyond the commit log');
SELECT (SELECT count(*) FROM sift._test_col_log_entries('events_c')) > 0 AS the_log_has_entries;
RESET sift.test_clog_horizon;
SELECT * FROM chk('back');

-- VACUUM with the transactions beyond the log: it asks the commit log nothing of them, and removes the entries of the rows it frees with their tombstones
SELECT set_config('sift.test_clog_horizon', (txid_current() + 100)::text, false) IS NOT NULL AS horizon_set;
VACUUM events;
RESET sift.test_clog_horizon;
SELECT * FROM chk('vacuumed');
SELECT entries FROM log_xids();

-- the transactions that aborted: their tombstones go at the next VACUUM, the rows stay
BEGIN; DELETE FROM events WHERE id % 17 = 0; ROLLBACK;
BEGIN; DELETE FROM events WHERE id % 19 = 0; SAVEPOINT a; DELETE FROM events WHERE id % 23 = 0; ROLLBACK TO a; COMMIT;
SELECT * FROM chk('aborted and rolled back to a savepoint');
SELECT entries > 0 AS has_entries FROM log_xids();
-- VACUUM that skips the vacuuming of the indexes and finds the transactions beyond the log (the knob): it can tell nothing of the tombstones, the aborted one stays and none is frozen
VACUUM events;
DELETE FROM events WHERE id IN (7, 8);
BEGIN; DELETE FROM events WHERE id IN (9, 10); ROLLBACK;
SELECT set_config('sift.test_clog_horizon', (txid_current() + 100)::text, false) IS NOT NULL AS horizon_set;
VACUUM events;
RESET sift.test_clog_horizon;
SELECT entries, frozen FROM log_xids();
SELECT * FROM chk('vacuum beyond the log');
VACUUM events;
SELECT entries, frozen FROM log_xids();
SELECT * FROM chk('vacuum within the log');

-- VACUUM that skips the vacuuming of the indexes (dead rows on few pages) leaves the entries and their tombstones; the committed ones, older than every snapshot, are written as frozen ones
-- and the aborted ones are gone
VACUUM events;
DELETE FROM events WHERE id IN (3, 4);
BEGIN; DELETE FROM events WHERE id IN (5, 6); ROLLBACK;
SELECT entries > 0 AS has_entries FROM log_xids();
VACUUM events;
SELECT frozen > 0 AS tombstones_frozen, frozen = entries AS all_frozen FROM log_xids();
SELECT * FROM chk('frozen');
-- and the transaction that made the log complete is one: a read does not depend on the log's memory of it
SELECT heap_free FROM sift.col_tracking('events_c');
SELECT * FROM chk('log complete, frozen');

DROP FUNCTION log_xids();
DROP FUNCTION chk(text);
DROP FUNCTION pages();
DROP FUNCTION answer(bool);
DROP TABLE events;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
