-- track_changes = 'ctid' (col_track_ctid): the entries of the index are at the address of the root of a HOT chain, and a version made by a HOT update is not at it. The row trigger writes the tombstone
-- of the version a statement ends: for a member of a chain, the address of its root (the roots of a page are read once for the versions of it a statement ends); a HOT update writes nothing,
-- because the entry stays right. Each case runs the same aggregate through the heap-free read, through the heap check (sift.test_no_heapfree) and with no sift plan (the oracle), and the
-- mutants that tombstone the member, or nothing, must give other rows.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE chains (id int NOT NULL, g int NOT NULL, w int NOT NULL, pad text NOT NULL) WITH (autovacuum_enabled = false, fillfactor = 60);
CREATE UNIQUE INDEX chains_id ON chains (id);
INSERT INTO chains SELECT i, i % 10, i, 'x' FROM generate_series(1, 30000) i;
CREATE INDEX chains_c ON chains USING sift_col (g, w) WITH (track_changes = 'ctid');
VACUUM chains;

CREATE FUNCTION answer(want_sift bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE q text := 'SELECT g, count(*) AS n, sum(w) AS s FROM chains GROUP BY g'; plan text := ''; l text; r text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || l || E'\n';
	END LOOP;
	IF (plan LIKE '%SiftAgg%') <> want_sift THEN
		RAISE EXCEPTION 'the plan is % a SiftAgg plan: %', CASE WHEN want_sift THEN 'not' ELSE 'still' END, plan;
	END IF;
	EXECUTE 'SELECT coalesce(string_agg(format(''%s:%s:%s'', g, n, s), '' '' ORDER BY g), ''none'') FROM (' || q || ') q' INTO r;
	RETURN r;
END $$;

CREATE FUNCTION pages() RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT g, count(*), sum(w) FROM chains GROUP BY g' INTO j;
	RETURN coalesce(jsonb_path_query_first(j, 'strict $.**."Heap Pages"')::int, -1);
END $$;

CREATE FUNCTION chk(what text, OUT test text, OUT same_as_heap_check bool, OUT same_as_no_sift bool, OUT free_reads_heap bool)
LANGUAGE plpgsql AS $$
DECLARE free_ans text; checked text; oracle text; pf int;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('sift.test_no_heapfree', 'off', true);
	free_ans := answer(true);
	pf := pages();
	PERFORM set_config('sift.test_no_heapfree', 'on', true);
	checked := answer(true);
	PERFORM set_config('sift.test_no_heapfree', 'off', true);
	PERFORM set_config('sift.enable_agg', 'off', true);
	oracle := answer(false);
	test := what;
	same_as_heap_check := free_ans = checked;
	same_as_no_sift := free_ans = oracle;
	free_reads_heap := pf > 0;
END $$;

SELECT * FROM chk('fresh');
SELECT heap_free FROM sift.col_tracking('chains_c');

-- three HOT updates of the same rows: chains of four versions, the entries still at the roots, no tombstone written
UPDATE chains SET pad = 'a' WHERE id % 3 = 0;
UPDATE chains SET pad = 'b' WHERE id % 3 = 0;
UPDATE chains SET pad = 'c' WHERE id % 3 = 0;
SELECT count(*) AS tombstones FROM sift._test_col_log_entries('chains_c') WHERE true;
SELECT * FROM chk('hot chains');

-- the end of a chain is updated in a covered column (a new entry, and the root's entry ends) and deleted
UPDATE chains SET w = w + 1000 WHERE id % 6 = 0;
SELECT * FROM chk('update of a chain member');
DELETE FROM chains WHERE id % 15 = 0;
SELECT * FROM chk('delete of a chain member');

-- the same with the mutant that tombstones the member, not the root: the entry at the root stays live
UPDATE chains SET pad = 'd' WHERE id % 4 = 1;
UPDATE chains SET pad = 'e' WHERE id % 4 = 1;
SET sift.test_no_chain_root = on;
UPDATE chains SET w = w + 5000 WHERE id % 8 = 1;
DELETE FROM chains WHERE id % 28 = 5;
RESET sift.test_no_chain_root;
SELECT same_as_no_sift AS mutant_member_address_agrees FROM chk('mutant: the address of the member');
-- one honest tombstone more does not repair what the mutant left: REINDEX does
REINDEX INDEX chains_c;
SELECT same_as_no_sift AS rebuilt_agrees FROM chk('rebuilt');

-- a HOT update and a covered update of one row in one transaction, and two covered updates
BEGIN;
UPDATE chains SET pad = 'f' WHERE id % 5 = 2;
UPDATE chains SET w = w + 1 WHERE id % 5 = 2;
UPDATE chains SET w = w + 1 WHERE id % 5 = 2;
COMMIT;
SELECT * FROM chk('one transaction');

-- a read prunes the page (the chains are cut to their live version and the roots become redirects), then more changes
SELECT count(*) FROM chains WHERE pad = 'zz';
UPDATE chains SET pad = 'g' WHERE id % 7 = 3;
UPDATE chains SET w = w + 3 WHERE id % 7 = 3;
SELECT * FROM chk('after pruning');
VACUUM chains;
UPDATE chains SET w = w + 2 WHERE id % 9 = 4;
DELETE FROM chains WHERE id % 31 = 7;
SELECT * FROM chk('after vacuum');

-- a BEFORE ROW trigger that rewrites the new version: the tombstone is for the version the statement ended, whatever the trigger made
CREATE FUNCTION bump() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.w := NEW.w + 1; RETURN NEW; END $$;
CREATE TRIGGER chains_bump BEFORE UPDATE ON chains FOR EACH ROW EXECUTE FUNCTION bump();
UPDATE chains SET pad = 'h' WHERE id % 11 = 2;
UPDATE chains SET w = w + 4 WHERE id % 13 = 3;
SELECT * FROM chk('before row trigger');
DROP TRIGGER chains_bump ON chains;

-- an upsert and a MERGE that update, delete and insert
INSERT INTO chains SELECT i, i % 10, i, 'u' FROM generate_series(100, 160) i ON CONFLICT (id) DO UPDATE SET w = chains.w + 7;
SELECT * FROM chk('upsert');
MERGE INTO chains c USING (SELECT i AS id, i % 10 AS g, i + 1 AS w FROM generate_series(150, 250) i) s ON c.id = s.id
	WHEN MATCHED AND s.id % 3 = 0 THEN DELETE
	WHEN MATCHED THEN UPDATE SET w = s.w
	WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.g, s.w, 'm');
SELECT * FROM chk('merge');

-- a savepoint that rolls back an update, a statement that fails in the middle
BEGIN;
SAVEPOINT s1;
UPDATE chains SET w = w + 9 WHERE id % 10 = 1;
ROLLBACK TO s1;
UPDATE chains SET w = w + 10 WHERE id % 10 = 2;
COMMIT;
SELECT * FROM chk('savepoint');
DO $$ BEGIN UPDATE chains SET w = w + 11 WHERE id % 10 = 3; PERFORM 1 / 0; EXCEPTION WHEN division_by_zero THEN NULL; END $$;
SELECT * FROM chk('failed statement');

-- the key path and the address path on one table: the index that holds the primary key's column changes from one to the other without a rebuild of the rows
ALTER TABLE chains ADD PRIMARY KEY USING INDEX chains_id;
CREATE INDEX chains_k ON chains USING sift_col (id, g, w) WITH (track_changes = 'id');
SELECT (SELECT track_changes AND heap_free FROM sift.col_tracking('chains_k')) AS key_index_heap_free;
ALTER INDEX chains_k SET (track_changes = 'ctid');
SELECT (SELECT track_changes AND heap_free FROM sift.col_tracking('chains_k')) AS key_index_by_address;
UPDATE chains SET w = w + 1 WHERE id % 17 = 0;
DELETE FROM chains WHERE id % 19 = 0;
SELECT * FROM chk('both indexes by address');
ALTER INDEX chains_k SET (track_changes = 'id');
UPDATE chains SET w = w + 1 WHERE id % 23 = 0;
DELETE FROM chains WHERE id % 29 = 0;
SELECT * FROM chk('the key index by key again');
DROP INDEX chains_k;

-- a partition: a statement on the parent fires no row trigger of the child that the log could take for complete
CREATE TABLE parted (id int NOT NULL, g int) PARTITION BY RANGE (id);
CREATE TABLE parted_1 PARTITION OF parted FOR VALUES FROM (0) TO (1000);
CREATE INDEX parted_1_c ON parted_1 USING sift_col (g) WITH (track_changes = 'ctid');
DROP TABLE parted;

-- the option off: the rows written while it was off are not in the log, and the reads check the heap
ALTER INDEX chains_c SET (track_changes = 'off');
SELECT track_changes, triggers FROM sift.col_tracking('chains_c');
DELETE FROM chains WHERE id % 37 = 0;
SELECT * FROM chk('option off');
-- the NOTICE that names the memory of the row queue and the switch
SET client_min_messages = notice;
ALTER INDEX chains_c SET (track_changes = 'ctid');
SET client_min_messages = warning;
SELECT track_changes, triggers, complete FROM sift.col_tracking('chains_c');
SELECT * FROM chk('on again');

-- the mutant that writes nothing
SET sift.test_no_track = on;
DELETE FROM chains WHERE id % 41 = 0;
RESET sift.test_no_track;
SELECT same_as_no_sift AS mutant_no_tombstones_agrees FROM chk('mutant: no tombstones');
REINDEX INDEX chains_c;
SELECT same_as_no_sift AS rebuilt_agrees FROM chk('rebuilt again');
DROP FUNCTION chk(text);
DROP FUNCTION pages();
DROP FUNCTION answer(bool);
DROP FUNCTION bump();
DROP TABLE chains;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
