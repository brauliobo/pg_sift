-- A dimension of a star join whose one integer key is unique and spans few more values than it has rows is held as its payload in the slot of each key (agg/star/dense.h): it is joined
-- in one pass where its rows in the hash form would not fit. The answers are those of the normal plan: fact keys outside the keys of the table or NULL, NULL payload values, a left join, a text
-- payload, keys that arrive out of order, rows the statistics do not know of (a key twice, a key far out, a NULL key: the table leaves the form) and rows the transaction deleted or changed.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.backend_cache_kb = 0;
SET sift.shared_cache_kb = 0;
SET work_mem = '256kB';

CREATE TABLE fx (id int8, k int8, amount numeric(10,2)) WITH (autovacuum_enabled = false);
INSERT INTO fx SELECT i, CASE WHEN i % 997 = 0 THEN NULL ELSE i % 9000 - 500 END, (i % 2950) / 100.0 FROM generate_series(1, 30000) i;
CREATE INDEX fx_col ON fx USING sift_col (id, k, amount);
-- keys 0 to 7999 inserted out of order, NULL payloads; the fact keys -500 to -1 and 8000 to 8499 have no row
CREATE TABLE dd (id int8, cls int, s int2) WITH (autovacuum_enabled = false);
INSERT INTO dd SELECT p, CASE WHEN p % 13 = 0 THEN NULL ELSE p % 7 END, (p % 3)::int2 FROM (SELECT (i * 1237) % 8000 AS p FROM generate_series(0, 7999) i) x;
-- a text payload, keys 0 to 1999 in descending order
CREATE TABLE dt (id int, tag text) WITH (autovacuum_enabled = false);
INSERT INTO dt SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE 't' || i % 5 END FROM generate_series(1999, 0, -1) i;
-- unique keys that span ten million values: no dense table fits, the hash form does
CREATE TABLE ds (id int8, cls int) WITH (autovacuum_enabled = false);
INSERT INTO ds SELECT i * 25, i % 7 FROM generate_series(0, 199) i;
INSERT INTO ds VALUES (10000000, 1);
ANALYZE fx, dd, dt, ds;

-- whether the star join ran the statement, how many rows, how many differ from the normal plan
CREATE FUNCTION dn_check(q text, OUT star boolean, OUT n bigint, OUT diff bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	star := false;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		star := star OR l LIKE '%Lookup Joins%';
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE r_on AS ' || q;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'CREATE TEMP TABLE r_off AS ' || q;
	SET LOCAL sift.enable_agg = on;
	SELECT count(*) INTO n FROM r_off;
	SELECT count(*) INTO diff FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) x;
	DROP TABLE r_on, r_off;
END $$;

-- the passes of the fact table the star join took: one while the table of dd is dense, more in the hash form, which its rows do not fit
CREATE FUNCTION dn_passes(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l LIKE '%Star Passes%' THEN
			RETURN btrim(l);
		END IF;
	END LOOP;
	RETURN 'one pass';
END $$;

CREATE VIEW dn_queries (q) AS VALUES
	('SELECT d.cls, count(*), sum(f.amount) FROM fx f JOIN dd d ON d.id = f.k GROUP BY d.cls'),
	('SELECT d.cls, d.s, count(*), sum(f.amount) FROM fx f LEFT JOIN dd d ON d.id = f.k GROUP BY d.cls, d.s'),
	('SELECT d.tag, count(*), min(f.k) FROM fx f JOIN dt d ON d.id = f.k GROUP BY d.tag'),
	('SELECT d.cls, count(*) FROM fx f JOIN ds d ON d.id = f.k GROUP BY d.cls');

SELECT q, c.* FROM dn_queries, dn_check(q) c;
SELECT dn_passes('SELECT d.cls, count(*) FROM fx f JOIN dd d ON d.id = f.k GROUP BY d.cls');

-- rows the statistics do not know of: the table leaves the dense form for the hash form
INSERT INTO dd VALUES (17, 3, 1), (1000000000, 1, 1), (NULL, 2, 2), (-3, 6, 0);
INSERT INTO dt VALUES (17, 'dup'), (NULL, 'nul'), (-3, 'neg');
SELECT q, c.* FROM dn_queries, dn_check(q) c;
SELECT dn_passes('SELECT d.cls, count(*) FROM fx f JOIN dd d ON d.id = f.k GROUP BY d.cls');

-- rows the transaction deleted or changed
BEGIN;
DELETE FROM dd WHERE id % 10 = 1;
UPDATE dd SET cls = 9 WHERE id % 10 = 2;
UPDATE dt SET tag = 'u' WHERE id % 10 = 3;
SELECT q, c.* FROM dn_queries, dn_check(q) c;
ROLLBACK;

DROP VIEW dn_queries;
DROP FUNCTION dn_check, dn_passes;
DROP TABLE fx, dd, dt, ds;
DROP EXTENSION pg_sift;
