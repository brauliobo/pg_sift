-- Transactions that write through many commands: a loop of single-row INSERTs in one transaction, savepoints and subtransactions, rows
-- written and read by the same transaction, and the other writers' rows in the same write buffers. The rows of a running transaction cannot be
-- final, so once the directory is half full the write buffers are promoted with the version (xmin and cmin) of every row, whoever wrote
-- it and with whichever command, and any mix of them merges, freezes when the writers are done and is dropped when they aborted.
-- The directory limit is lowered here (a testing setting) so that a few dozen write buffers of 2 MB are enough to reach the pressure.
-- Every statement runs on a heap twin as well: the answers must be the same in every state.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET sift.table_max_segments = 16;
SET sift.table_write_buffer_limit = 2048;
SET max_parallel_workers_per_gather = 0;

-- order independent, so that no sort of the rows (about a kilobyte each) is needed
CREATE FUNCTION digest(t regclass) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT count(*) || '':'' || coalesce(sum(id), 0) || '':'' || coalesce(sum(hashtextextended(id || v, 0)::numeric), 0) FROM %s', t) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION same(a regclass, b regclass) RETURNS bool LANGUAGE sql AS $$ SELECT digest(a) = digest(b) $$;
-- the rows of one table that are not in the other, both ways
CREATE FUNCTION differ(a regclass, b regclass) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE r bigint;
BEGIN
	EXECUTE format('SELECT (SELECT count(*) FROM (SELECT * FROM %1$s EXCEPT ALL SELECT * FROM %2$s) x) + (SELECT count(*) FROM (SELECT * FROM %2$s EXCEPT ALL SELECT * FROM %1$s) y)', a, b) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION nsegs(t regclass, OUT total int, OUT columns int, OUT sealed int) LANGUAGE sql AS
$$ SELECT count(*)::int, (count(*) FILTER (WHERE kind = 'columns'))::int, (count(*) FILTER (WHERE kind = 'sealed buffer'))::int FROM sift.table_segments(t) $$;
-- the rows hold about a kilobyte: 40000 of them are about 20 write buffers, more than the directory has entries for
-- one INSERT of one row per command: every row has its own command id
CREATE FUNCTION ins1(t regclass, lo int, hi int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE q text := format('INSERT INTO %s VALUES ($1, repeat(''x'', 1000) || $1)', t);
BEGIN
	FOR i IN lo..hi LOOP EXECUTE q USING i; END LOOP;
END $$;
-- the same inside a block with an exception handler: every row has its own subtransaction, and every fail_every-th is rolled back
CREATE FUNCTION ins1x(t regclass, lo int, hi int, fail_every int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE q text := format('INSERT INTO %s VALUES ($1, repeat(''x'', 1000) || $1)', t);
BEGIN
	FOR i IN lo..hi LOOP
		BEGIN
			EXECUTE q USING i;
			IF i % fail_every = 0 THEN RAISE EXCEPTION 'undo %', i; END IF;
		EXCEPTION WHEN raise_exception THEN NULL;
		END;
	END LOOP;
END $$;
-- INSERTs, and an UPDATE and a DELETE of rows written a few commands before (the table has an index on id and seqscan is off)
CREATE FUNCTION mixed(t regclass, lo int, hi int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	ins text := format('INSERT INTO %s VALUES ($1, repeat(''x'', 1000) || $1)', t);
	upd text := format('UPDATE %s SET v = v || ''u'' WHERE id = $1', t);
	del text := format('DELETE FROM %s WHERE id = $1', t);
BEGIN
	FOR i IN lo..hi LOOP
		EXECUTE ins USING i;
		IF i % 4 = 0 THEN EXECUTE upd USING i - 2; END IF;
		IF i % 7 = 0 THEN EXECUTE del USING i - 3; END IF;
	END LOOP;
END $$;
-- INSERT ... ON CONFLICT (id) of a row per command: DO NOTHING, or DO UPDATE that appends to the row
CREATE FUNCTION upsert(t regclass, lo int, hi int, doupdate bool) RETURNS void LANGUAGE plpgsql AS $$
DECLARE q text := format('INSERT INTO %1$s VALUES ($1, repeat(''x'', 1000) || $1) ON CONFLICT (id) DO %2$s', t,
						 CASE WHEN doupdate THEN format('UPDATE SET v = %s.v || ''c''', t) ELSE 'NOTHING' END);
BEGIN
	FOR i IN lo..hi LOOP EXECUTE q USING i; END LOOP;
END $$;
-- the ids of the next n rows of a cursor
CREATE FUNCTION take(cur refcursor, n int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r record;
	ids int[] := '{}';
BEGIN
	FOR i IN 1..n LOOP
		FETCH cur INTO r;
		ids := ids || r.id;
	END LOOP;
	RETURN array_to_string(ids, ',');
END $$;
-- the rows a cursor gives back: how many, the sum of the ids and of the lengths of the values
CREATE FUNCTION drain(cur refcursor) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r record;
	n int := 0;
	s bigint := 0;
	l bigint := 0;
BEGIN
	LOOP
		FETCH cur INTO r;
		EXIT WHEN NOT FOUND;
		n := n + 1;
		s := s + r.id;
		l := l + length(r.v);
	END LOOP;
	RETURN n || ':' || s || ':' || l;
END $$;
-- the answers of a table under every plan the scans have: sequential, btree, bitmap, parallel, and the aggregate pushdown (the
-- settings of the session are restored). The queries read every column of all the rows (sequential, parallel), of a slice of them (the
-- scans through an index fetch the rows one by one) or are a grouped aggregate (agg); sliceseq is the slice through a sequential scan
CREATE FUNCTION plan_sql(variant text, t regclass) RETURNS text LANGUAGE sql AS $$
	SELECT CASE variant
		WHEN 'agg' THEN format('SELECT id %% 3 AS g, count(*) AS c, sum(id) AS s FROM %s GROUP BY 1 ORDER BY 1', t)
		WHEN 'index' THEN format('SELECT count(*) AS c, sum(id) AS s, sum(length(v)) AS l FROM %s WHERE id BETWEEN 1000 AND 2999', t)
		WHEN 'bitmap' THEN format('SELECT count(*) AS c, sum(id) AS s, sum(length(v)) AS l FROM %s WHERE id BETWEEN 1000 AND 2999', t)
		WHEN 'sliceseq' THEN format('SELECT count(*) AS c, sum(id) AS s, sum(length(v)) AS l FROM %s WHERE id BETWEEN 1000 AND 2999', t)
		ELSE format('SELECT count(*) AS c, sum(id) AS s, sum(length(v)) AS l FROM %s WHERE id > 0', t) END
$$;
-- the plan of the twin that gives the answer variant must give
CREATE FUNCTION plan_ref(variant text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE variant WHEN 'index' THEN 'sliceseq' WHEN 'bitmap' THEN 'sliceseq' WHEN 'agg' THEN 'agg' ELSE 'seq' END
$$;
CREATE FUNCTION plan_settings(variant text) RETURNS text[] LANGUAGE sql AS $$
	SELECT CASE variant
		WHEN 'seq' THEN ARRAY['enable_seqscan', 'on', 'enable_indexscan', 'off', 'enable_bitmapscan', 'off', 'sift.enable_agg', 'off']
		WHEN 'sliceseq' THEN ARRAY['enable_seqscan', 'on', 'enable_indexscan', 'off', 'enable_bitmapscan', 'off', 'sift.enable_agg', 'off']
		WHEN 'index' THEN ARRAY['enable_seqscan', 'off', 'enable_indexscan', 'on', 'enable_bitmapscan', 'off', 'sift.enable_agg', 'off']
		WHEN 'bitmap' THEN ARRAY['enable_seqscan', 'off', 'enable_indexscan', 'off', 'enable_bitmapscan', 'on', 'sift.enable_agg', 'off']
		WHEN 'parallel' THEN ARRAY['enable_seqscan', 'on', 'enable_indexscan', 'off', 'enable_bitmapscan', 'off', 'sift.enable_agg', 'off',
			'max_parallel_workers_per_gather', '2', 'parallel_setup_cost', '0', 'parallel_tuple_cost', '0', 'min_parallel_table_scan_size', '0']
		WHEN 'agg' THEN ARRAY['enable_seqscan', 'on', 'enable_indexscan', 'off', 'enable_bitmapscan', 'off', 'sift.enable_agg', 'on', 'cpu_tuple_cost', '0.5']
	END
$$;
CREATE FUNCTION plan_node(variant text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE variant WHEN 'seq' THEN 'Seq Scan' WHEN 'index' THEN 'Index Scan' WHEN 'bitmap' THEN 'Bitmap Heap Scan'
		WHEN 'parallel' THEN 'Parallel Seq Scan' WHEN 'agg' THEN 'SiftAgg' END
$$;
-- variant and its answer (the plan text when explain is set)
CREATE FUNCTION plan_run(t regclass, variant text, explain bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	st text[] := plan_settings(variant);
	old text[] := '{}';
	r text;
	l text;
BEGIN
	FOR i IN 1..array_length(st, 1) BY 2 LOOP
		old := old || st[i] || current_setting(st[i]);
		PERFORM set_config(st[i], st[i + 1], true);
	END LOOP;
	IF explain THEN
		r := '';
		FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || plan_sql(variant, t) LOOP r := r || l || E'\n'; END LOOP;
	ELSE
		EXECUTE format('SELECT string_agg(x::text, '';'') FROM (%s) x', plan_sql(variant, t)) INTO r;
	END IF;
	FOR i IN 1..array_length(old, 1) BY 2 LOOP PERFORM set_config(old[i], old[i + 1], true); END LOOP;
	RETURN r;
END $$;
-- 'ok', or the plans of t that did not use the node they were set up to use
CREATE FUNCTION plans(t regclass) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce(nullif(string_agg(v, ',' ORDER BY v), ''), 'ok')
		FROM unnest(ARRAY['seq', 'index', 'bitmap', 'parallel', 'agg']) v WHERE plan_run(t, v, true) NOT LIKE '%' || plan_node(v) || '%'
$$;

-- 'ok', or the plans of t that answer differently than the twin h
CREATE FUNCTION agree(t regclass, h regclass) RETURNS text LANGUAGE sql AS $$
	SELECT coalesce(nullif(string_agg(v, ',' ORDER BY v), ''), 'ok')
		FROM unnest(ARRAY['seq', 'index', 'bitmap', 'parallel', 'agg']) v WHERE plan_run(t, v, false) IS DISTINCT FROM plan_run(h, plan_ref(v), false)
$$;

-- ---------- a loop of single-row INSERTs, committed ----------
CREATE TABLE lp_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE lp_h (id int, v text) WITH (autovacuum_enabled = false);
CREATE INDEX lp_t_id ON lp_t (id);
CREATE INDEX lp_h_id ON lp_h (id);
BEGIN;
SELECT ins1('lp_t', 1, 40000);
SELECT ins1('lp_h', 1, 40000);
SELECT count(*), sum(id), count(*) FILTER (WHERE v <> repeat('x', 1000) || id) AS wrong FROM lp_t;
SELECT total <= 14 AS within_the_limit, columns > 0 AS promoted_while_running, sift._test_tbl_tagged('lp_t') > 0 AS carries_its_writers FROM nsegs('lp_t');
SELECT same('lp_t', 'lp_h') AS the_twin_agrees_while_running;
SELECT agree('lp_t', 'lp_h') AS plans_agree_while_running;
COMMIT;
SELECT total <= 14 AS within_the_limit FROM nsegs('lp_t');
SELECT same('lp_t', 'lp_h') AS the_twin_agrees;
SELECT agree('lp_t', 'lp_h') AS plans_agree;
SELECT plans('lp_t') AS plans_used;
SELECT differ('lp_t', 'lp_h') AS rows_that_differ;
-- the rows are final: VACUUM freezes the groups and the table answers the same
VACUUM lp_t;
SELECT same('lp_t', 'lp_h') AS the_twin_agrees_after_vacuum;
SELECT agree('lp_t', 'lp_h') AS plans_agree_after_vacuum;
SELECT sealed = 0 AS nothing_waits, sift._test_tbl_tagged('lp_t') = 0 AS frozen FROM nsegs('lp_t');
SELECT plans('lp_t') AS plans_used_after_vacuum;
-- a second loop on top of the frozen groups
BEGIN;
SELECT ins1('lp_t', 40001, 80000);
SELECT ins1('lp_h', 40001, 80000);
SELECT same('lp_t', 'lp_h') AS the_twin_agrees_with_a_second_loop;
COMMIT;
VACUUM lp_t;
SELECT same('lp_t', 'lp_h') AS the_twin_agrees_after_the_second_vacuum, sift._test_tbl_tagged('lp_t') = 0 AS frozen;

DROP TABLE lp_t, lp_h;

-- ---------- a loop that is rolled back: nothing is left, and the space comes back after VACUUM ----------
CREATE TABLE rb_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE INDEX rb_t_id ON rb_t (id);
BEGIN;
SELECT ins1('rb_t', 1, 40000);
SELECT count(*) FROM rb_t;
SELECT total <= 14 AS within_the_limit, columns > 0 AS promoted_while_running FROM nsegs('rb_t');
ROLLBACK;
SELECT count(*) FROM rb_t;
SET enable_seqscan = off;
SELECT count(*) FROM rb_t WHERE id > 0;
RESET enable_seqscan;
SELECT sift._test_tbl_tagged('rb_t') > 0 AS groups_wait_for_vacuum;
SELECT relation_blocks AS rolled_back_blocks FROM sift.table_space('rb_t') \gset
VACUUM rb_t;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
VACUUM rb_t;
SELECT count(*) FROM rb_t;
SELECT sift._test_tbl_tagged('rb_t') = 0 AS groups_gone, relation_blocks < :rolled_back_blocks / 4 AS space_returned
	FROM nsegs('rb_t'), sift.table_space('rb_t');
-- the table takes a new load as if it were new, and rounds of the same stay as large as the first
BEGIN;
SELECT ins1('rb_t', 1, 40000);
COMMIT;
SELECT count(*), sum(id) FROM rb_t;
SET enable_seqscan = off;
SELECT count(*), sum(id) FROM rb_t WHERE id > 20000;
RESET enable_seqscan;
DELETE FROM rb_t;
VACUUM rb_t;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
VACUUM rb_t;
SELECT high_water AS first_round FROM sift.table_space('rb_t') \gset
SELECT count(*) FROM rb_t;
BEGIN;
SELECT ins1('rb_t', 1, 40000);
ROLLBACK;
VACUUM rb_t;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
VACUUM rb_t;
BEGIN;
SELECT ins1('rb_t', 1, 40000);
ROLLBACK;
VACUUM rb_t;
SELECT pg_current_xact_id() IS NOT NULL AS xid_passed;
VACUUM rb_t;
SELECT count(*) FROM rb_t;
SELECT high_water <= 2 * :first_round AS space_is_reused, sift._test_tbl_tagged('rb_t') = 0 AS groups_gone FROM sift.table_space('rb_t');

DROP TABLE rb_t;

-- ---------- the rows of the transaction read by the transaction itself ----------
CREATE TABLE ow_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE ow_h (id int, v text) WITH (autovacuum_enabled = false);
CREATE INDEX ow_t_id ON ow_t (id);
CREATE INDEX ow_h_id ON ow_h (id);
BEGIN;
SELECT ins1('ow_t', 1, 20000);
SELECT ins1('ow_h', 1, 20000);
SELECT count(*), sum(id) FROM ow_t;
SELECT agree('ow_t', 'ow_h') AS plans_agree_in_the_middle_of_the_loop;
-- three cursors opened in the middle of the loop: they see the 20000 rows and nothing that comes after, whatever it does to them
DECLARE c_seq CURSOR FOR SELECT id, v FROM ow_t;
DECLARE c_seq_h CURSOR FOR SELECT id, v FROM ow_h;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
DECLARE c_idx CURSOR FOR SELECT id, v FROM ow_t WHERE id > 0 ORDER BY id;
DECLARE c_idx_h CURSOR FOR SELECT id, v FROM ow_h WHERE id > 0 ORDER BY id;
SET enable_indexscan = off;
SET enable_bitmapscan = on;
DECLARE c_bm CURSOR FOR SELECT id, v FROM ow_t WHERE id > 0;
DECLARE c_bm_h CURSOR FOR SELECT id, v FROM ow_h WHERE id > 0;
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;
SELECT take('c_idx', 3) AS first_rows, take('c_idx_h', 3) AS first_rows_of_the_twin;
SELECT ins1('ow_t', 20001, 40000);
SELECT ins1('ow_h', 20001, 40000);
UPDATE ow_t SET v = 'changed' WHERE id % 2 = 0;
UPDATE ow_h SET v = 'changed' WHERE id % 2 = 0;
DELETE FROM ow_t WHERE id % 7 = 0;
DELETE FROM ow_h WHERE id % 7 = 0;
SELECT ins1('ow_t', 40001, 45000);
SELECT ins1('ow_h', 40001, 45000);
SELECT c AS seq_cursor, c = drain('c_seq_h') AS seq_cursor_agrees FROM (SELECT drain('c_seq') AS c) x;
SELECT take('c_idx', 2) AS next_rows, take('c_idx_h', 2) AS next_rows_of_the_twin;
SELECT c AS index_cursor, c = drain('c_idx_h') AS index_cursor_agrees FROM (SELECT drain('c_idx') AS c) x;
SELECT c AS bitmap_cursor, c = drain('c_bm_h') AS bitmap_cursor_agrees FROM (SELECT drain('c_bm') AS c) x;
SELECT same('ow_t', 'ow_h') AS the_twin_agrees_while_running;
-- INSERT ... SELECT from the table it writes, repeated: every statement reads what the ones before wrote and none of its own rows
-- (the second reads the rows of the first through the index, and the third the rows of both)
INSERT INTO ow_t SELECT id + 1000000, v FROM ow_t;
INSERT INTO ow_h SELECT id + 1000000, v FROM ow_h;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
INSERT INTO ow_t SELECT id + 2000000, v FROM ow_t WHERE id BETWEEN 1 AND 10000;
INSERT INTO ow_h SELECT id + 2000000, v FROM ow_h WHERE id BETWEEN 1 AND 10000;
INSERT INTO ow_t SELECT id + 3000000, v FROM ow_t WHERE id BETWEEN 1 AND 10000;
INSERT INTO ow_h SELECT id + 3000000, v FROM ow_h WHERE id BETWEEN 1 AND 10000;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT count(*), count(*) FILTER (WHERE id < 1000000) AS original FROM ow_t;
SELECT total <= 14 AS within_the_limit, sift._test_tbl_tagged('ow_t') > 0 AS carries_its_writer FROM nsegs('ow_t');
SELECT same('ow_t', 'ow_h') AS the_twin_agrees_after_self_inserts;
COMMIT;
SELECT same('ow_t', 'ow_h') AS the_twin_agrees;
SELECT agree('ow_t', 'ow_h') AS plans_agree;
VACUUM ow_t;
SELECT same('ow_t', 'ow_h') AS the_twin_agrees_after_vacuum;
SELECT agree('ow_t', 'ow_h') AS plans_agree_after_vacuum;

-- a trigger that reads the table: it sees the row that fired it and all the ones before
CREATE TABLE tr_log (tbl text, id int, n bigint);
CREATE FUNCTION tr_fn() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	IF NEW.id % 5000 = 0 THEN
		EXECUTE format('INSERT INTO tr_log SELECT %L, %s, count(*) FROM %s', TG_TABLE_NAME, NEW.id, TG_RELID::regclass);
	END IF;
	RETURN NULL;
END $$;
CREATE TABLE tr_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE tr_h (id int, v text) WITH (autovacuum_enabled = false);
CREATE TRIGGER tr_t_read AFTER INSERT ON tr_t FOR EACH ROW EXECUTE FUNCTION tr_fn();
CREATE TRIGGER tr_h_read AFTER INSERT ON tr_h FOR EACH ROW EXECUTE FUNCTION tr_fn();
BEGIN;
SELECT ins1('tr_t', 1, 40000);
SELECT ins1('tr_h', 1, 40000);
COMMIT;
SELECT tbl, count(*) AS reads, bool_and(n = id) AS saw_all_rows_so_far FROM tr_log GROUP BY tbl ORDER BY tbl;
SELECT same('tr_t', 'tr_h') AS the_twin_agrees;

DROP TABLE ow_t, ow_h, tr_t, tr_h, tr_log;

-- ---------- UPDATE and DELETE of rows the transaction wrote a few commands before, and of rows that are already promoted ----------
CREATE TABLE ud_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE ud_h (id int, v text) WITH (autovacuum_enabled = false);
CREATE INDEX ud_t_id ON ud_t (id);
CREATE INDEX ud_h_id ON ud_h (id);
SET enable_seqscan = off;
BEGIN;
SELECT mixed('ud_t', 1, 40000);
SELECT mixed('ud_h', 1, 40000);
SELECT count(*), count(*) FILTER (WHERE v LIKE '%u') AS updated FROM ud_t;
SELECT total <= 14 AS within_the_limit, sift._test_tbl_tagged('ud_t') > 0 AS carries_its_writer FROM nsegs('ud_t');
SELECT same('ud_t', 'ud_h') AS the_twin_agrees_while_running;
-- the rows promoted so far, by statements of many rows
UPDATE ud_t SET v = v || 'w' WHERE id % 3 = 0;
UPDATE ud_h SET v = v || 'w' WHERE id % 3 = 0;
DELETE FROM ud_t WHERE id % 5 = 0;
DELETE FROM ud_h WHERE id % 5 = 0;
SELECT same('ud_t', 'ud_h') AS the_twin_agrees_after_the_statements;
COMMIT;
RESET enable_seqscan;
SELECT same('ud_t', 'ud_h') AS the_twin_agrees;
-- after the commit, before the groups are frozen
UPDATE ud_t SET v = v || 'k' WHERE id % 11 = 0;
UPDATE ud_h SET v = v || 'k' WHERE id % 11 = 0;
DELETE FROM ud_t WHERE id % 13 = 0;
DELETE FROM ud_h WHERE id % 13 = 0;
SELECT same('ud_t', 'ud_h') AS the_twin_agrees_after_more_statements;
SELECT agree('ud_t', 'ud_h') AS plans_agree;
VACUUM ud_t;
VACUUM ud_h;
SELECT same('ud_t', 'ud_h') AS the_twin_agrees_after_vacuum;
SELECT agree('ud_t', 'ud_h') AS plans_agree_after_vacuum;
SELECT differ('ud_t', 'ud_h') AS rows_that_differ;

DROP TABLE ud_t, ud_h;

-- ---------- savepoints: rows of a rolled back savepoint go, the ones around them stay ----------
CREATE TABLE sp_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE sp_h (id int, v text) WITH (autovacuum_enabled = false);
CREATE INDEX sp_t_id ON sp_t (id);
BEGIN;
SELECT ins1('sp_t', 1, 15000);
SAVEPOINT s;
SELECT ins1('sp_t', 100001, 140000);
SELECT count(*) FROM sp_t;
SELECT total <= 14 AS within_the_limit, sift._test_tbl_tagged('sp_t') > 0 AS the_subtransaction_has_groups FROM nsegs('sp_t');
ROLLBACK TO s;
SELECT count(*) FROM sp_t;
-- the savepoint is still there: a second try goes to the same buffers
SELECT ins1('sp_t', 150001, 170000);
ROLLBACK TO s;
SELECT ins1('sp_t', 15001, 40000);
SAVEPOINT a;
SELECT ins1('sp_t', 200001, 215000);
SAVEPOINT b;
SELECT ins1('sp_t', 300001, 330000);
ROLLBACK TO b;
SELECT ins1('sp_t', 215001, 220000);
RELEASE a;
SELECT count(*), count(*) FILTER (WHERE id > 300000) AS in_the_rolled_back FROM sp_t;
COMMIT;
SELECT ins1('sp_h', 1, 40000);
SELECT ins1('sp_h', 200001, 220000);
SELECT same('sp_t', 'sp_h') AS the_twin_agrees;
SELECT agree('sp_t', 'sp_h') AS plans_agree;
VACUUM sp_t;
SELECT same('sp_t', 'sp_h') AS the_twin_agrees_after_vacuum;
SELECT agree('sp_t', 'sp_h') AS plans_agree_after_vacuum;
SELECT sift._test_tbl_tagged('sp_t') = 0 AS frozen;
-- released savepoints do not outlive a rolled back transaction
BEGIN;
SAVEPOINT s;
SELECT ins1('sp_t', 400001, 430000);
RELEASE s;
SELECT ins1('sp_t', 430001, 450000);
SELECT count(*) FROM sp_t WHERE id > 400000;
ROLLBACK;
SELECT count(*) FROM sp_t WHERE id > 400000;
VACUUM sp_t;
VACUUM sp_t;
SELECT same('sp_t', 'sp_h') AS the_twin_agrees_after_the_rollback;
SELECT sift._test_tbl_tagged('sp_t') = 0 AS groups_gone;

DROP TABLE sp_t, sp_h;

-- ---------- one subtransaction per row (a block with an exception handler): half of them roll back, or none ----------
CREATE TABLE sx_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE sx_h (id int, v text) WITH (autovacuum_enabled = false);
CREATE INDEX sx_t_id ON sx_t (id);
BEGIN;
SELECT ins1x('sx_t', 1, 40000, 2);
SELECT ins1x('sx_h', 1, 40000, 2);
SELECT count(*), sum(id) FROM sx_t;
SELECT total <= 14 AS within_the_limit, sift._test_tbl_tagged('sx_t') > 0 AS carries_its_writers FROM nsegs('sx_t');
SELECT same('sx_t', 'sx_h') AS the_twin_agrees_while_running;
COMMIT;
SELECT same('sx_t', 'sx_h') AS the_twin_agrees;
SELECT agree('sx_t', 'sx_h') AS plans_agree;
-- none rolled back: the rows of 40000 subtransactions
BEGIN;
SELECT ins1x('sx_t', 100001, 140000, 1000000);
SELECT ins1x('sx_h', 100001, 140000, 1000000);
SELECT same('sx_t', 'sx_h') AS the_twin_agrees_with_every_subtransaction_kept;
COMMIT;
SELECT same('sx_t', 'sx_h') AS the_twin_agrees_after_the_commit;
VACUUM sx_t;
SELECT same('sx_t', 'sx_h') AS the_twin_agrees_after_vacuum;
SELECT agree('sx_t', 'sx_h') AS plans_agree_after_vacuum;
SELECT sift._test_tbl_tagged('sx_t') = 0 AS frozen;
-- the subtransactions commit, the transaction does not
BEGIN;
SELECT ins1x('sx_t', 200001, 240000, 3);
SELECT count(*) FROM sx_t WHERE id > 200000;
ROLLBACK;
SELECT count(*) FROM sx_t WHERE id > 200000;
VACUUM sx_t;
VACUUM sx_t;
SELECT same('sx_t', 'sx_h') AS the_twin_agrees_after_the_rollback, sift._test_tbl_tagged('sx_t') = 0 AS groups_gone;

DROP TABLE sx_t, sx_h;

-- ---------- SELECT FOR UPDATE of rows that are promoted: by the writer, and after it committed ----------
CREATE TABLE fu_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE fu_h (id int, v text) WITH (autovacuum_enabled = false);
CREATE INDEX fu_t_id ON fu_t (id);
CREATE INDEX fu_h_id ON fu_h (id);
BEGIN;
SELECT ins1('fu_t', 1, 40000);
SELECT ins1('fu_h', 1, 40000);
SELECT count(*) AS locked FROM (SELECT id FROM fu_t WHERE id % 1000 = 0 FOR UPDATE) x;
SELECT count(*) AS locked_share FROM (SELECT id FROM fu_t WHERE id % 1000 = 500 FOR SHARE) x;
SELECT count(*) AS locked_no_key FROM (SELECT id FROM fu_t WHERE id <= 100 FOR NO KEY UPDATE) x;
UPDATE fu_t SET v = v || 'l' WHERE id % 1000 = 0;
UPDATE fu_h SET v = v || 'l' WHERE id % 1000 = 0;
SELECT same('fu_t', 'fu_h') AS the_twin_agrees_while_running;
COMMIT;
BEGIN;
SELECT count(*) AS locked FROM (SELECT id FROM fu_t WHERE id % 500 = 0 FOR UPDATE) x;
SET enable_seqscan = off;
SELECT count(*) AS locked_by_index FROM (SELECT id FROM fu_t WHERE id > 39000 FOR UPDATE) x;
RESET enable_seqscan;
UPDATE fu_t SET v = v || 'm' WHERE id % 500 = 0;
UPDATE fu_h SET v = v || 'm' WHERE id % 500 = 0;
DELETE FROM fu_t WHERE id > 39000;
DELETE FROM fu_h WHERE id > 39000;
COMMIT;
SELECT same('fu_t', 'fu_h') AS the_twin_agrees;
VACUUM fu_t;
SELECT same('fu_t', 'fu_h') AS the_twin_agrees_after_vacuum;

DROP TABLE fu_t, fu_h;

-- ---------- the isolation levels ----------
CREATE TABLE il_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE il_h (id int, v text) WITH (autovacuum_enabled = false);
CREATE INDEX il_t_id ON il_t (id);
CREATE INDEX il_h_id ON il_h (id);
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT ins1('il_t', 1, 25000);
SELECT ins1('il_h', 1, 25000);
SELECT count(*), sum(id) FROM il_t;
SELECT agree('il_t', 'il_h') AS plans_agree_repeatable_read;
COMMIT;
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT ins1('il_t', 25001, 50000);
SELECT ins1('il_h', 25001, 50000);
SELECT count(*), sum(id) FROM il_t;
SELECT agree('il_t', 'il_h') AS plans_agree_serializable;
COMMIT;
BEGIN ISOLATION LEVEL READ COMMITTED;
SELECT ins1('il_t', 50001, 75000);
SELECT ins1('il_h', 50001, 75000);
SELECT count(*), sum(id) FROM il_t;
SELECT agree('il_t', 'il_h') AS plans_agree_read_committed;
COMMIT;
SELECT same('il_t', 'il_h') AS the_twin_agrees;
VACUUM il_t;
SELECT same('il_t', 'il_h') AS the_twin_agrees_after_vacuum;
SELECT total <= 14 AS within_the_limit, sift._test_tbl_tagged('il_t') = 0 AS frozen FROM nsegs('il_t');

DROP TABLE il_t, il_h;

-- ---------- ON CONFLICT of a row per command, against rows that are promoted ----------
CREATE TABLE oc_t (id int PRIMARY KEY, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE oc_h (id int PRIMARY KEY, v text) WITH (autovacuum_enabled = false);
BEGIN;
SELECT upsert('oc_t', 1, 30000, false);
SELECT upsert('oc_h', 1, 30000, false);
-- the first half conflicts, the second does not
SELECT upsert('oc_t', 15001, 45000, false);
SELECT upsert('oc_h', 15001, 45000, false);
SELECT count(*), sum(id) FROM oc_t;
-- DO UPDATE of rows written earlier by the transaction
SELECT upsert('oc_t', 1, 20000, true);
SELECT upsert('oc_h', 1, 20000, true);
SELECT count(*), count(*) FILTER (WHERE v LIKE '%c') AS updated FROM oc_t;
SELECT total <= 14 AS within_the_limit, sift._test_tbl_tagged('oc_t') > 0 AS carries_its_writer FROM nsegs('oc_t');
SELECT same('oc_t', 'oc_h') AS the_twin_agrees_while_running;
COMMIT;
SELECT same('oc_t', 'oc_h') AS the_twin_agrees;
-- DO UPDATE of rows of a committed transaction that are not frozen yet, and of frozen ones
BEGIN;
SELECT upsert('oc_t', 19001, 26000, true);
SELECT upsert('oc_h', 19001, 26000, true);
COMMIT;
SELECT same('oc_t', 'oc_h') AS the_twin_agrees_after_the_second_transaction;
VACUUM oc_t;
BEGIN;
SELECT upsert('oc_t', 1, 3000, true);
SELECT upsert('oc_h', 1, 3000, true);
COMMIT;
SELECT same('oc_t', 'oc_h') AS the_twin_agrees_after_vacuum;
SELECT agree('oc_t', 'oc_h') AS plans_agree;
SELECT count(*) = count(DISTINCT id) AS keys_are_unique FROM oc_t;

DROP TABLE oc_t, oc_h;

-- ---------- a duplicate key is found among the rows of the transaction itself, and among the ones that are committed or frozen ----------
CREATE TABLE uq_t (id int PRIMARY KEY, v text) USING sift_table WITH (autovacuum_enabled = false);
BEGIN;
SELECT ins1('uq_t', 1, 40000);
INSERT INTO uq_t VALUES (5, 'again');
ROLLBACK;
SELECT count(*) FROM uq_t;
BEGIN;
SELECT ins1('uq_t', 1, 40000);
COMMIT;
INSERT INTO uq_t VALUES (39999, 'again');
VACUUM uq_t;
INSERT INTO uq_t VALUES (39999, 'again');
INSERT INTO uq_t VALUES (40001, 'new');
SELECT count(*) FROM uq_t;

DROP TABLE uq_t;

-- ---------- parallel plans and the aggregate pushdown over a table written by a loop, before and after the groups are frozen ----------
CREATE TABLE par_t (id int, v text) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE par_h (id int, v text) WITH (autovacuum_enabled = false);
CREATE INDEX par_t_id ON par_t (id);
CREATE INDEX par_h_id ON par_h (id);
BEGIN;
SELECT ins1('par_t', 1, 40000);
SELECT ins1('par_h', 1, 40000);
COMMIT;
ANALYZE par_t;
ANALYZE par_h;
SELECT plans('par_t') AS plans_used;
SELECT agree('par_t', 'par_h') AS plans_agree;
VACUUM par_t;
SELECT plans('par_t') AS plans_used_after_vacuum;
SELECT agree('par_t', 'par_h') AS plans_agree_after_vacuum;

RESET max_parallel_workers_per_gather;
DROP TABLE par_t, par_h;
DROP FUNCTION digest(regclass), same(regclass, regclass), differ(regclass, regclass), nsegs(regclass), ins1(regclass, int, int),
	ins1x(regclass, int, int, int), mixed(regclass, int, int), upsert(regclass, int, int, bool), drain(refcursor), take(refcursor, int), tr_fn(),
	plan_sql(text, regclass), plan_ref(text), plan_settings(text), plan_node(text), plan_run(regclass, text, bool), agree(regclass, regclass), plans(regclass);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
