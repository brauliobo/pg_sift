-- A SERIALIZABLE transaction that reads a table through a node of SiftAggV6 holds the read lock of the whole table (a SIReadLock on the relation), as the scan of any table method does: the
-- writer of another transaction that adds a row the read would have counted is told about it, and one of the two fails with 40001 (two transactions that each sum a group and add a
-- withdrawal, whose total goes negative, are the write skew). Each statement runs in a transaction of its own and lists the relations it holds the lock of: the table (and the index the
-- scan read) of a sift_col index and of a sift_table, for the scan of the aggregate, the Top-N, the window and the join of a small side.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
DO $$ BEGIN
	IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN
		PERFORM set_config('sift.test_create_format1', 'off', false);
	END IF;
END $$;

CREATE TABLE acct (grp int NOT NULL, bal int NOT NULL, id int NOT NULL) USING sift_table;
INSERT INTO acct SELECT g % 4, g, g FROM generate_series(1, 4000) g;
CREATE TABLE hacct (grp int NOT NULL, bal int NOT NULL, id int NOT NULL);
INSERT INTO hacct SELECT * FROM acct;
CREATE INDEX hacct_col ON hacct USING sift_col (grp, bal, id);
CREATE TABLE adim (grp int PRIMARY KEY, tier int NOT NULL);
INSERT INTO adim SELECT g, g % 2 FROM generate_series(0, 3) g;
VACUUM (ANALYZE) acct, hacct;
ANALYZE adim;

CREATE FUNCTION engine(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text[] := '{}';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan' THEN r := r || substring(l from 'Custom Scan \(([A-Za-z0-9]+)\)'); END IF;
	END LOOP;
	RETURN coalesce(nullif(array_to_string(r, ','), ''), 'normal');
END $$;
CREATE VIEW read_locks AS
	SELECT c.relname::text AS relation FROM pg_locks l JOIN pg_class c ON c.oid = l.relation
	WHERE l.mode = 'SIReadLock' AND l.locktype = 'relation' AND l.pid = pg_backend_pid() AND c.relnamespace = 'public'::regnamespace ORDER BY 1;

SELECT engine($$SELECT sum(bal) FROM acct WHERE grp = 1$$) AS table_aggregate,
	engine($$SELECT id FROM acct WHERE grp = 1 ORDER BY bal DESC LIMIT 3$$) AS table_top_n,
	engine($$SELECT count(*) FROM (SELECT row_number() OVER (PARTITION BY grp ORDER BY bal) rn FROM acct) s WHERE rn <= 2$$) AS table_window,
	engine($$SELECT sum(bal) FROM hacct WHERE grp = 1$$) AS col_aggregate,
	engine($$SELECT count(*) FROM hacct WHERE grp IN (SELECT grp FROM adim WHERE tier = 1)$$) AS col_semi_join;

BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT sum(bal) FROM acct WHERE grp = 1;
SELECT 'table aggregate' AS statement, relation FROM read_locks;
ROLLBACK;
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT id FROM acct WHERE grp = 1 ORDER BY bal DESC LIMIT 3;
SELECT 'table top-n' AS statement, relation FROM read_locks;
ROLLBACK;
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT count(*) FROM (SELECT row_number() OVER (PARTITION BY grp ORDER BY bal) rn FROM acct) s WHERE rn <= 2;
SELECT 'table window' AS statement, relation FROM read_locks;
ROLLBACK;
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT sum(bal) FROM hacct WHERE grp = 1;
SELECT 'col aggregate' AS statement, relation FROM read_locks;
ROLLBACK;
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT count(*) FROM hacct WHERE grp IN (SELECT grp FROM adim WHERE tier = 1);
SELECT 'col semi join' AS statement, relation FROM read_locks;
ROLLBACK;

DROP VIEW read_locks;
DROP TABLE acct, hacct, adim;
DROP FUNCTION engine(text);
DROP EXTENSION pg_sift;
