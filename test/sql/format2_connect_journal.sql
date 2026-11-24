-- connect = 'fk -> parent(key)' on indexes of Format 2.0: the parent keeps a key journal (SIFT_PT_KEYJRNL) that the writes of the parent append to and the writes of the child do not, and the child
-- (also with UNIQUE and INCLUDE) is built in Format 2.0. Every answer is compared with the heap's (sift.enable_agg = off) after the writes of the parent and the child.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET enable_hashagg = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET client_min_messages = warning;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE TABLE dim (k int PRIMARY KEY, a int, b int, flag bool) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, i % 7, i % 3, i % 5 = 0 FROM generate_series(0, 699) i;
CREATE TABLE ev (id int PRIMARY KEY, k int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev SELECT i, CASE WHEN i % 211 = 0 THEN NULL ELSE i % 750 END, i % 11 FROM generate_series(1, 3000) i;

-- what is wrong with the declaration, and with the parent
CREATE INDEX bad ON ev USING sift_col (id, k) WITH (connect = 'k -> dim(k)');
CREATE INDEX dim_col ON dim USING sift_col (k, a, b, flag);
CREATE INDEX bad ON ev USING sift_col (id, k) WITH (connect = 'v -> dim(k)');
CREATE INDEX bad ON ev USING sift_col (id, k) WITH (connect = 'k -> dim(nokey)');
SELECT sift.col_connection_state('dim_col') AS no_journal_yet;

-- the connections: the parent and the children are all Format 2.0
CREATE INDEX ev_col ON ev USING sift_col (id, k, v) WITH (connect = 'k -> dim(k)');
CREATE UNIQUE INDEX ev_uniq ON ev USING sift_col (id, k) INCLUDE (v) WITH (connect = 'k -> dim(k)');
SELECT relation::text, format_major FROM sift.status() WHERE relation::text IN ('dim_col', 'ev_col', 'ev_uniq') ORDER BY 1;
SELECT child, ordinal, fk, parent, declaration, lineage IS NOT NULL AS journal, records, stamp, fill FROM sift.col_connections ORDER BY child::text;
SELECT o AS option FROM pg_class c, unnest(c.reloptions) o WHERE c.relname = 'dim_col' AND o LIKE 'track_changes=%';
SELECT count(*) AS edges FROM pg_depend WHERE classid = 'pg_class'::regclass AND refobjid = 'dim_col'::regclass AND deptype = 'n';
VACUUM (FREEZE, ANALYZE) ev, dim;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT dim.a, count(*), sum(ev.v) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$),
 (2, $q$SELECT dim.a, dim.b, count(*) FROM ev LEFT JOIN dim ON dim.k = ev.k GROUP BY 1, 2 ORDER BY 1, 2$q$),
 (3, $q$SELECT ev.v, sum(dim.a), count(dim.flag), max(dim.b) FROM ev JOIN dim ON dim.k = ev.k GROUP BY 1 ORDER BY 1$q$);

CREATE FUNCTION answer(sql text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text; BEGIN
	PERFORM set_config('sift.enable_agg', scan::text, true);
	EXECUTE 'SELECT string_agg(t::text, '';'' ORDER BY t::text) FROM (' || sql || ') t' INTO r;
	RETURN r;
END $$;
CREATE FUNCTION engaged(sql text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	PERFORM set_config('sift.enable_agg', 'true', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		IF l LIKE '%SiftAggV6%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION report(stage text) RETURNS TABLE (stage text, shape int, same_answers bool, engine bool) LANGUAGE sql AS $$
	SELECT stage, n, answer(sql, true) = answer(sql, false), engaged(sql) FROM shapes ORDER BY n
$$;
CREATE TABLE seen (records bigint);
INSERT INTO seen VALUES (0);
CREATE FUNCTION journaled(label text) RETURNS TABLE (stage text, records_moved bool) LANGUAGE plpgsql AS $$
DECLARE r bigint; o bigint; BEGIN
	SELECT s.records INTO r FROM sift.col_connection_state('dim_col') s;
	SELECT records INTO o FROM seen;
	UPDATE seen SET records = r;
	RETURN QUERY SELECT label, r IS DISTINCT FROM o;
END $$;
CREATE FUNCTION problems() RETURNS bigint LANGUAGE sql AS
$$ SELECT count(*) FROM sift.check('dim_col', heapallindexed => true) c1, sift.check('ev_col', heapallindexed => true) c2, sift.check('ev_uniq', heapallindexed => true) c3 $$;
CREATE FUNCTION state() RETURNS text LANGUAGE sql AS $$ SELECT stamp FROM sift.col_connection_state('dim_col') $$;
SELECT * FROM journaled('built');


-- a committed update of the payload, then of the key; a delete; an insert
UPDATE dim SET a = 6, flag = NULL WHERE k % 31 = 0;
SELECT * FROM report('payload update');
SELECT * FROM journaled('payload update');
UPDATE dim SET k = k + 100000 WHERE k % 97 = 0;
SELECT * FROM report('key update');
SELECT * FROM journaled('key update');
DELETE FROM dim WHERE k % 53 = 0;
SELECT * FROM report('delete');
SELECT * FROM journaled('delete');
INSERT INTO dim SELECT i, 1, 1, true FROM generate_series(700, 749) i;
SELECT * FROM report('insert');
SELECT * FROM journaled('insert');
SELECT problems() AS problems;

-- the writer's own transaction, an abort and a savepoint that rolls back: the aborted writes are no change of the parent (the aggregate inside the transaction asks the journal, which writes the
-- records the transaction holds: they count as moved, no reader counts them)
BEGIN;
UPDATE dim SET b = 2 WHERE k % 17 = 0;
SELECT * FROM report('in the transaction');
ROLLBACK;
SELECT * FROM report('after the abort');
SELECT * FROM journaled('abort');
BEGIN;
SAVEPOINT s;
UPDATE dim SET b = 2 WHERE k % 19 = 0;
INSERT INTO dim VALUES (5000, 0, 0, false);
ROLLBACK TO s;
COMMIT;
SELECT * FROM report('after the savepoint');

-- a statement that ends more versions than a page of the journal holds: its trim says the records went
UPDATE dim SET a = a + 1;
SELECT * FROM report('journal wrapped');
SELECT * FROM journaled('wrap');
SELECT problems() AS problems;

-- the child's own writes change nothing of the parent
INSERT INTO ev SELECT 10000 + i, i % 750, i % 11 FROM generate_series(1, 200) i;
UPDATE ev SET k = k + 1 WHERE id % 13 = 0;
DELETE FROM ev WHERE id % 29 = 0;
SELECT * FROM report('child writes');
SELECT * FROM journaled('child writes');

-- VACUUM and sift.maintain() trim the journal of what no reader counts, and keep what a stamp may need
UPDATE dim SET b = 1 WHERE k = 11;
VACUUM (INDEX_CLEANUP ON) dim;
SELECT * FROM report('vacuumed');
SELECT busy FROM sift.maintain('dim_col');
SELECT * FROM report('maintained');
SELECT problems() AS problems;

-- ALTER INDEX changes the connection of an index that holds data; the parent keeps its journal
SELECT lineage IS NOT NULL AS journal FROM sift.col_connection_state('dim_col');
ALTER INDEX ev_col SET (connect = 'off');
SELECT count(*) AS edges FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'ev_col'::regclass AND refobjid = 'dim_col'::regclass;
SELECT format_major FROM sift.status('ev_col');
UPDATE dim SET a = 5 WHERE k = 11;
SELECT * FROM report('connection off');
ALTER INDEX ev_col SET (connect = 'k -> dim(k)');
SELECT count(*) AS edges FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'ev_col'::regclass AND refobjid = 'dim_col'::regclass;
ALTER INDEX ev_col RESET (connect);
SELECT count(*) AS edges FROM pg_depend WHERE classid = 'pg_class'::regclass AND objid = 'ev_col'::regclass AND refobjid = 'dim_col'::regclass;
ALTER INDEX ev_col SET (connect = 'k -> dim(k)');
ALTER INDEX ev_col SET (connect = 'k -> dim(nokey)');
SELECT * FROM report('connection back');

-- a rebuilt parent is another journal: the answers are the heap's
SELECT lineage AS before FROM sift.col_connection_state('dim_col') \gset
REINDEX INDEX dim_col;
SELECT lineage <> :before AS new_lineage, records, stamp FROM sift.col_connection_state('dim_col');
SELECT format_major FROM sift.status('dim_col');
SELECT * FROM report('reindexed parent');
SELECT * FROM journaled('reindex');
REINDEX INDEX ev_col;
REINDEX INDEX ev_uniq;
SELECT relation::text, format_major FROM sift.status() WHERE relation::text IN ('dim_col', 'ev_col', 'ev_uniq') ORDER BY 1;
SELECT * FROM report('reindexed children');
UPDATE dim SET a = 4 WHERE k % 41 = 0;
SELECT * FROM report('after the reindex, an update');
SELECT problems() AS problems;

-- the journal and the connections go with the indexes, and the parent index cannot go under a child
DROP INDEX dim_col;
DROP TABLE dim CASCADE;
SELECT count(*) AS child_indexes FROM pg_class WHERE relname IN ('ev_col', 'ev_uniq');

DROP FUNCTION report(text), journaled(text), engaged(text), answer(text, bool), problems(), state();
DROP TABLE shapes, seen, ev;
DROP EXTENSION pg_sift;
