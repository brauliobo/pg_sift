-- The VACUUM that a transaction asks for when it commits (sift.col_auto_merge, col/vacreq.c): a table that was just loaded has no statistics and no scan yet, so the
-- transaction that builds a sift_col index over pages the visibility map does not hold, or writes to one a thousandth of the table (1,000 rows at least), starts the worker that vacuums it.
-- The test clusters run with the worker off; the commit reads the session's setting.
CREATE EXTENSION pg_sift;
SET sift.col_auto_merge = on;
SET sift.async_maintenance = on;
SET max_parallel_workers_per_gather = 0;

-- true when the worker's VACUUM marked every page of the table all-visible within 30 s
CREATE FUNCTION vr_vacuumed(tbl regclass) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..300 LOOP
		IF (SELECT relallvisible > 0 AND relallvisible = relpages FROM pg_class WHERE oid = tbl) THEN RETURN true; END IF;
		PERFORM pg_sleep(0.1);
	END LOOP;
	RETURN false;
END $$;

-- no maintenance worker is at work in the database
CREATE FUNCTION vr_idle() RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..300 LOOP
		IF NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE backend_type = 'sift maintenance' AND datname = current_database()) THEN RETURN true; END IF;
		PERFORM pg_sleep(0.1);
	END LOOP;
	RETURN false;
END $$;

-- true when the table was vacuumed more than n times (vacuum_count) within 30 s
CREATE FUNCTION vr_vacuumed_over(tbl regclass, n bigint) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..300 LOOP
		PERFORM pg_stat_clear_snapshot();
		IF (SELECT vacuum_count > n FROM pg_stat_user_tables WHERE relid = tbl) THEN RETURN true; END IF;
		PERFORM pg_sleep(0.1);
	END LOOP;
	RETURN false;
END $$;

-- the grouped sum of the table through the index and through the heap
CREATE FUNCTION vr_same(tbl regclass) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (SELECT s, count(*), sum(amt) FROM %s GROUP BY s) x', tbl) INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (SELECT s, count(*), sum(amt) FROM %s GROUP BY s) x', tbl) INTO b;
	RETURN a = b;
END $$;

-- a bulk load, then the index: the build asks
CREATE TABLE vr_built (id int PRIMARY KEY, s text NOT NULL, amt numeric NOT NULL);
INSERT INTO vr_built SELECT g, 'st' || g % 7, g % 1000 FROM generate_series(1, 30000) g;
SELECT relallvisible FROM pg_class WHERE relname = 'vr_built';
CREATE INDEX vr_built_col ON vr_built USING sift_col (s, amt);
SELECT vr_vacuumed('vr_built') AS built_vacuumed;
SELECT vr_same('vr_built') AS built_same;

-- the index on an empty table, then the bulk load: the rows the transaction writes ask
CREATE TABLE vr_loaded (id int PRIMARY KEY, s text NOT NULL, amt numeric NOT NULL);
CREATE INDEX vr_loaded_col ON vr_loaded USING sift_col (s, amt);
INSERT INTO vr_loaded SELECT g, 'st' || g % 7, g % 1000 FROM generate_series(1, 30000) g;
SELECT vr_vacuumed('vr_loaded') AS loaded_vacuumed;
SELECT vr_same('vr_loaded') AS loaded_same;

-- an UPDATE of a tenth of the table is churn the same way, once the worker of the load has left (it looks again for a few naptimes, and a request while one is at the table is not needed)
SELECT vr_idle() AS idle;
SELECT vacuum_count AS before FROM pg_stat_user_tables WHERE relname = 'vr_loaded' \gset
UPDATE vr_loaded SET amt = amt + 1 WHERE id % 10 = 0;
SELECT vr_vacuumed_over('vr_loaded', :before) AS updated_vacuumed;
SELECT vr_same('vr_loaded') AS updated_same;

-- a table of a few pages is not worth the worker
CREATE TABLE vr_small AS SELECT g AS id, 'st' || g % 7 AS s, (g % 1000)::numeric AS amt FROM generate_series(1, 300) g;
CREATE INDEX vr_small_col ON vr_small USING sift_col (s, amt);
SELECT pg_sleep(1);
SELECT relallvisible FROM pg_class WHERE relname = 'vr_small';

-- the setting off: the build asks for nothing
SET sift.col_auto_merge = off;
CREATE TABLE vr_off AS SELECT g AS id, 'st' || g % 7 AS s, (g % 1000)::numeric AS amt FROM generate_series(1, 30000) g;
CREATE INDEX vr_off_col ON vr_off USING sift_col (s, amt);
SELECT pg_sleep(1);
SELECT relallvisible FROM pg_class WHERE relname = 'vr_off';

-- a table that opted out of autovacuum is left to the user (the notice of the build says so)
SET sift.col_auto_merge = on;
CREATE TABLE vr_optout WITH (autovacuum_enabled = false) AS SELECT g AS id, 'st' || g % 7 AS s, (g % 1000)::numeric AS amt FROM generate_series(1, 30000) g;
CREATE INDEX vr_optout_col ON vr_optout USING sift_col (s, amt);
SELECT pg_sleep(1);
SELECT relallvisible FROM pg_class WHERE relname = 'vr_optout';

-- a build that rolls back asks for nothing, and the request of the next one that commits is not lost
SET sift.col_auto_merge = on;
CREATE TABLE vr_rolled AS SELECT g AS id, 'st' || g % 7 AS s, (g % 1000)::numeric AS amt FROM generate_series(1, 30000) g;
BEGIN;
CREATE INDEX vr_rolled_col ON vr_rolled USING sift_col (s, amt);
ROLLBACK;
SELECT pg_sleep(1);
SELECT relallvisible FROM pg_class WHERE relname = 'vr_rolled';
CREATE INDEX vr_rolled_col ON vr_rolled USING sift_col (s, amt);
SELECT vr_vacuumed('vr_rolled') AS rolled_then_built_vacuumed;

DROP TABLE vr_built, vr_loaded, vr_small, vr_off, vr_optout, vr_rolled;
DROP FUNCTION vr_vacuumed, vr_idle, vr_vacuumed_over, vr_same;
DROP EXTENSION pg_sift;
