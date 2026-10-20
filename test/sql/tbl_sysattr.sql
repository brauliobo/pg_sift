-- sift_table system columns xmin, xmax, cmin and cmax against a heap twin. The ids change from run to run, so the checks are relations: the
-- rows of one inserting transaction share an xmin, a row's xmin is its inserter (a subtransaction included), an updated row has a new xmin,
-- a locked or deleted row has the xmax of its locker or deleter, VACUUM FREEZE leaves FrozenTransactionId. The command ids are the same
-- for the same statements of one transaction. Every statement runs on both tables; the groups printed are the heap's and must be the twin's.
CREATE EXTENSION pg_sift;

CREATE TABLE h (id int PRIMARY KEY, v text) USING heap;
CREATE TABLE s (id int PRIMARY KEY, v text) USING sift_table;

-- the ids that share a value of the column of the table, groups separated by '|'; the twin must print the same
CREATE FUNCTION groups(t regclass, col text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT string_agg(ids, '' | '' ORDER BY lo) FROM (SELECT string_agg(id::text, '','' ORDER BY id) ids, min(id) lo FROM %s GROUP BY %s::text) x', t, col) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION same(col text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN groups('h', col) = groups('s', col) THEN col || ': ' || groups('h', col)
		ELSE format('%s: DIFFERENT heap %s, sift_table %s', col, groups('h', col), groups('s', col)) END
$$;

-- two inserting transactions: each row's xmin is the inserter's, the rows of one share it
BEGIN;
INSERT INTO h VALUES (1, 'a'), (2, 'a'), (3, 'a');
INSERT INTO s VALUES (1, 'a'), (2, 'a'), (3, 'a');
SELECT (SELECT bool_and(xmin = pg_current_xact_id()::xid) FROM h) AS heap_xmin_is_inserter,
	(SELECT bool_and(xmin = pg_current_xact_id()::xid) FROM s) AS sift_xmin_is_inserter;
COMMIT;
BEGIN;
INSERT INTO h VALUES (4, 'b'), (5, 'b'), (6, 'b');
INSERT INTO s VALUES (4, 'b'), (5, 'b'), (6, 'b');
COMMIT;
SELECT same('xmin');
SELECT same('xmax');
-- through a sequential scan, an index scan, a TID scan and a bitmap scan
SELECT count(DISTINCT xmin::text) AS xmins_seq FROM s;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(DISTINCT xmin::text) AS xmins_index FROM s WHERE id IN (1, 2, 4);
SET enable_bitmapscan = on;
SET enable_indexscan = off;
SELECT count(DISTINCT xmin::text) AS xmins_bitmap FROM s WHERE id IN (1, 2, 4);
RESET enable_seqscan;
RESET enable_indexscan;
SELECT xmin = (SELECT xmin FROM s WHERE id = 4) AS tid_scan_same FROM s WHERE ctid = (SELECT ctid FROM s WHERE id = 5);

-- an update: the new version has the updater's xmin
UPDATE h SET v = 'u' WHERE id = 2;
UPDATE s SET v = 'u' WHERE id = 2;
SELECT same('xmin');
SELECT same('xmax');

-- a row lock: xmax is the locker's, in its transaction and after it
BEGIN;
SELECT id FROM h WHERE id = 1 FOR UPDATE;
SELECT id FROM s WHERE id = 1 FOR UPDATE;
SELECT (SELECT xmax = pg_current_xact_id()::xid FROM h WHERE id = 1) AS heap_locker,
	(SELECT xmax = pg_current_xact_id()::xid FROM s WHERE id = 1) AS sift_locker;
SELECT same('xmax');
COMMIT;
SELECT same('xmax');
SELECT same('xmin');

-- RETURNING reads the columns of the row just written
BEGIN;
WITH r AS (INSERT INTO s VALUES (7, 'r') RETURNING xmin) SELECT bool_and(xmin = pg_current_xact_id()::xid) AS insert_returning FROM r;
UPDATE s SET v = 'r2' WHERE id = 7 RETURNING xmin = pg_current_xact_id()::xid AS update_returning, xmax;
ROLLBACK;

-- command ids: the same statements give the same cmin in a transaction of each table
BEGIN;
INSERT INTO h VALUES (10, 'c');
INSERT INTO h VALUES (11, 'c');
SELECT count(*) FROM h;
INSERT INTO h VALUES (12, 'c');
SELECT id, cmin::text AS cmin FROM h WHERE id >= 10 ORDER BY id;
ROLLBACK;
BEGIN;
INSERT INTO s VALUES (10, 'c');
INSERT INTO s VALUES (11, 'c');
SELECT count(*) FROM s;
INSERT INTO s VALUES (12, 'c');
SELECT id, cmin::text AS cmin FROM s WHERE id >= 10 ORDER BY id;
ROLLBACK;

-- a row a cursor still sees after a later command of its transaction deleted it: xmax and cmax are the deleter's
BEGIN;
INSERT INTO h VALUES (99, 'x');
DECLARE ch CURSOR FOR SELECT id, xmax = pg_current_xact_id()::xid AS deleted_here, cmax::text AS cmax FROM h WHERE id > 3;
FETCH 1 FROM ch;
DELETE FROM h WHERE id = 5;
FETCH ALL FROM ch;
ROLLBACK;
BEGIN;
INSERT INTO s VALUES (99, 'x');
DECLARE cs CURSOR FOR SELECT id, xmax = pg_current_xact_id()::xid AS deleted_here, cmax::text AS cmax FROM s WHERE id > 3;
FETCH 1 FROM cs;
DELETE FROM s WHERE id = 5;
FETCH ALL FROM cs;
ROLLBACK;

-- subtransactions: a row's xmin is the subtransaction that inserted it, after RELEASE too; a rolled back one leaves nothing
BEGIN;
INSERT INTO h VALUES (20, 'top');
SAVEPOINT a;
INSERT INTO h VALUES (21, 'sub');
SAVEPOINT b;
INSERT INTO h VALUES (22, 'sub2');
ROLLBACK TO b;
INSERT INTO h VALUES (23, 'sub3');
RELEASE a;
INSERT INTO h VALUES (24, 'top');
SELECT groups('h', 'xmin') AS heap_groups;
SELECT bool_and(CASE WHEN id IN (20, 24) THEN xmin = pg_current_xact_id()::xid ELSE xmin <> pg_current_xact_id()::xid END) AS subs_differ FROM h WHERE id >= 20;
COMMIT;
DELETE FROM h WHERE id >= 20;
BEGIN;
INSERT INTO s VALUES (20, 'top');
SAVEPOINT a;
INSERT INTO s VALUES (21, 'sub');
SAVEPOINT b;
INSERT INTO s VALUES (22, 'sub2');
ROLLBACK TO b;
INSERT INTO s VALUES (23, 'sub3');
RELEASE a;
INSERT INTO s VALUES (24, 'top');
SELECT groups('s', 'xmin') AS sift_groups;
SELECT bool_and(CASE WHEN id IN (20, 24) THEN xmin = pg_current_xact_id()::xid ELSE xmin <> pg_current_xact_id()::xid END) AS subs_differ FROM s WHERE id >= 20;
COMMIT;
DELETE FROM s WHERE id >= 20;

-- a read through the aggregate pushdown path and a sort
SELECT count(xmin::text) AS n, count(DISTINCT xmin::text) AS xmins FROM s;
SELECT (SELECT string_agg(id::text, ',' ORDER BY xmin::text::bigint, id) FROM h) = (SELECT string_agg(id::text, ',' ORDER BY xmin::text::bigint, id) FROM s) AS same_order;

-- the aggregate pushdown reads the columns of the table only: a query that reads a system column keeps the scan, and answers as without it
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
SET max_parallel_workers_per_gather = 0;
SET cpu_tuple_cost = 0.5;
SELECT pushed('SELECT v, count(*) FROM s GROUP BY v') AS control_pushed,
	pushed('SELECT v, count(xmin::text) FROM s GROUP BY v') AS xmin_aggregate_pushed,
	pushed('SELECT v, count(*) FROM s WHERE xmax::text = ''0'' GROUP BY v') AS xmax_filter_pushed;
SELECT v, count(*), count(DISTINCT xmin::text) AS xmins, count(*) FILTER (WHERE xmax::text <> '0') AS with_xmax FROM s GROUP BY v ORDER BY v;
SET sift.enable_agg = off;
SELECT v, count(*), count(DISTINCT xmin::text) AS xmins, count(*) FILTER (WHERE xmax::text <> '0') AS with_xmax FROM s GROUP BY v ORDER BY v;
RESET sift.enable_agg;
RESET cpu_tuple_cost;
RESET max_parallel_workers_per_gather;

-- VACUUM FREEZE: a heap keeps the inserter's xid in a frozen row (the frozen flag is a hint bit), a sift_table row reads FrozenTransactionId (2)
VACUUM FREEZE h;
VACUUM FREEZE s;
SELECT (SELECT count(*) FROM s WHERE xmin::text = '2') AS sift_frozen, (SELECT count(*) FROM s) AS rows;
SELECT same('xmax');
-- the rows promoted to a column group: an update and a lock after the freeze
UPDATE h SET v = 'after' WHERE id = 3;
UPDATE s SET v = 'after' WHERE id = 3;
BEGIN;
SELECT id FROM h WHERE id = 4 FOR UPDATE;
SELECT id FROM s WHERE id = 4 FOR UPDATE;
SELECT same('xmax');
ROLLBACK;
SELECT same('xmax');
SELECT groups('s', 'xmin') AS sift_xmin;

DROP FUNCTION pushed(text);
DROP FUNCTION same(text);
DROP FUNCTION groups(regclass, text);
DROP TABLE h, s;
DROP EXTENSION pg_sift;
