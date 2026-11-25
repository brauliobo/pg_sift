-- Spec 27 T5: SiftHybridScan, the structured scan of a sift_col index and the ranked text scan of a sift index handing each other their TIDs as a bridge.
-- Direction A (a selective structured filter and a wide query), direction B (a selective query, or a wide one with a structured filter or an aggregate), lossy bridges under a small
-- work_mem, rows that were deleted and updated (HOT and not), a transaction's own changes, cursors, rescans and parameters: every result is compared with the same query that
-- the hybrid scan cannot serve (a qual of its own that no store decides is added), and the node and its direction are read from EXPLAIN.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
CREATE TABLE hy_docs (id int PRIMARY KEY, tenant int NOT NULL, region int NOT NULL, amount int NOT NULL, body text NOT NULL, note text);
INSERT INTO hy_docs (id, tenant, region, amount, body)
SELECT i, i % 10000, i % 7, i % 100,
	concat_ws(' ', CASE WHEN i % 2 = 0 THEN 'common' END, CASE WHEN i % 20 = 0 THEN 'mid' END, CASE WHEN i % 20000 = 0 THEN 'rareterm' END, 'tok' || i % 13, 'base')
FROM generate_series(1, 200000) i;
CREATE INDEX hy_text ON hy_docs USING sift (body);
CREATE INDEX hy_col ON hy_docs USING sift_col (tenant, region, amount);
ANALYZE hy_docs;
RESET client_min_messages;

-- the hybrid scan's direction in the plan of q ('none': another plan), and a property of its EXPLAIN ANALYZE
CREATE FUNCTION hy_dir(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; d text := 'none';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Hybrid Direction: ' THEN d := substring(l from 'Hybrid Direction: (\S+)'); END IF;
	END LOOP;
	RETURN d;
END $$;

CREATE FUNCTION hy_prop(q text, label text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; v text := NULL;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ ('^\s*' || label || ': ') THEN v := substring(l from ': (\S+)$'); END IF;
	END LOOP;
	RETURN v;
END $$;

-- rows of q, and the rows that differ from q with a qual added that the store cannot decide (so that the standard plans serve it)
CREATE FUNCTION hy_check(q text, OUT rows bigint, OUT diff bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE 'CREATE TEMP TABLE hy_h AS ' || q;
	EXECUTE 'CREATE TEMP TABLE hy_b AS ' || regexp_replace(q, 'WHERE ', 'WHERE (tenant + 0) = tenant AND ');
	SELECT count(*) INTO rows FROM hy_h;
	SELECT count(*) INTO diff FROM ((TABLE hy_h EXCEPT ALL TABLE hy_b) UNION ALL (TABLE hy_b EXCEPT ALL TABLE hy_h)) d;
	DROP TABLE hy_h, hy_b;
END $$;

CREATE TABLE hy_q (name text PRIMARY KEY, q text);
INSERT INTO hy_q VALUES
	-- direction A: 20 rows of a tenant (a point bridge), a wide query
	('a_point_ids',	'SELECT id FROM hy_docs WHERE tenant = 8 AND body @@> ''common'''),
	('a_point_top',	'SELECT round(sift.score(ctid)::numeric, 5) AS s FROM hy_docs WHERE tenant = 8 AND body @@> ''common'' ORDER BY sift.score(ctid) DESC LIMIT 5'),
	-- A at 1%, 10% and 50% of the rows
	('a_1pct',		'SELECT id FROM hy_docs WHERE tenant < 100 AND body @@> ''mid'''),
	('a_10pct',		'SELECT id FROM hy_docs WHERE tenant < 1000 AND body @@> ''common'''),
	('a_10pct_mid',	'SELECT id FROM hy_docs WHERE tenant < 1000 AND body @@> ''mid'''),
	('a_50pct',		'SELECT id FROM hy_docs WHERE tenant < 5000 AND body @@> ''common'''),
	('a_range_top',	'SELECT round(sift.score(ctid)::numeric, 5) AS s FROM hy_docs WHERE amount < 10 AND region = 3 AND body @@> ''common tok5'' ORDER BY sift.score(ctid) DESC LIMIT 20'),
	-- direction B: a selective query with a filter, and a wide query aggregated
	('b_rare',		'SELECT id, amount FROM hy_docs WHERE body @@> ''rareterm'' AND amount >= 0'),
	('b_agg',		'SELECT region, count(*) AS n, sum(amount) AS total FROM hy_docs WHERE body @@> ''mid'' AND amount >= 0 GROUP BY region'),
	('b_agg_scored','SELECT region, round(avg(sift.score(ctid))::numeric, 4) AS avg_score FROM hy_docs WHERE body @@> ''mid'' AND amount >= 0 GROUP BY region'),
	-- no match anywhere: no rows from either direction
	('empty_col',	'SELECT id FROM hy_docs WHERE tenant = 8 AND amount > 100 AND body @@> ''common'''),
	('empty_text',	'SELECT id FROM hy_docs WHERE tenant < 1000 AND body @@> ''nosuchword''');

-- what the planner chooses
SELECT name, hy_dir(q) AS direction FROM hy_q WHERE name IN ('a_point_ids', 'a_point_top') ORDER BY name;

-- the same rows as the standard plans, whatever the direction
SELECT name, (hy_check(q)).* FROM hy_q ORDER BY name;

-- a hand-counted case: the 20 rows of tenant 8 are all even, so all of them match 'common'; the 10 multiples of 20000 hold 'rareterm'
SELECT count(*) AS tenant_8 FROM hy_docs WHERE tenant = 8 AND body @@> 'common';
SELECT count(*) AS rareterm FROM hy_docs WHERE body @@> 'rareterm' AND amount >= 0;

-- direction A on a point bridge: the ranked search read the 20 TIDs, and an empty bridge never ran it
SELECT hy_prop(q, 'Bridge Point') AS point, hy_prop(q, 'Bridge TIDs') AS tids FROM hy_q WHERE name = 'a_point_ids';
SELECT hy_prop(q, 'Bridge Empty') AS empty FROM hy_q WHERE name = 'empty_col';

-- a small work_mem (the bridge may hold half of it): the rows are the same, and the bridge is read from EXPLAIN (a lossy page is exercised by the unit test of the bridge)
SET work_mem = '256kB';
SELECT name, (hy_check(q)).* FROM hy_q WHERE name IN ('a_10pct', 'a_10pct_mid', 'a_50pct', 'a_range_top') ORDER BY name;
SELECT hy_prop(q, 'Bridge Exact Pages')::int > 0 AS exact_pages, hy_prop(q, 'Bridge Lossy Pages')::int >= 0 AS lossy_counted FROM hy_q WHERE name = 'a_10pct_mid';
RESET work_mem;

-- rows that were deleted, updated in a column of the store, updated in a column of no index (HOT) and updated in the text
DELETE FROM hy_docs WHERE tenant = 8 AND id % 20000 = 8;
UPDATE hy_docs SET amount = amount + 1000 WHERE tenant = 18;
UPDATE hy_docs SET note = 'x' WHERE tenant = 28;
UPDATE hy_docs SET body = body || ' common' WHERE tenant = 38 AND id % 2 = 1;
SELECT name, (hy_check(q)).* FROM hy_q ORDER BY name;
SELECT count(*) AS tenant_8_after FROM hy_docs WHERE tenant = 8 AND body @@> 'common';

-- the changes of the transaction itself, uncommitted
BEGIN;
DELETE FROM hy_docs WHERE tenant = 8;
UPDATE hy_docs SET amount = 5 WHERE tenant IN (48, 58);
INSERT INTO hy_docs VALUES (200001, 8, 1, 1, 'common fresh', NULL), (200002, 48, 2, 2, 'common mid fresh', NULL);
SELECT name, (hy_check(q)).* FROM hy_q WHERE name IN ('a_point_ids', 'a_point_top', 'a_1pct', 'b_rare', 'b_agg') ORDER BY name;
ROLLBACK;
SELECT name, (hy_check(q)).* FROM hy_q WHERE name IN ('a_point_ids', 'a_1pct') ORDER BY name;

-- a scroll cursor: the node does not run backwards, the plan holds a Materialize above it
BEGIN;
DECLARE hy_c SCROLL CURSOR FOR SELECT id FROM hy_docs WHERE tenant = 18 AND body @@> 'common' ORDER BY id;
FETCH 3 FROM hy_c;
FETCH BACKWARD 2 FROM hy_c;
FETCH ABSOLUTE 1 FROM hy_c;
COMMIT;

-- a plan that is run again with other constants, a generic plan whose query is a parameter, and a parameterized nested loop: the scan serves constants only
PREPARE hy_p (text) AS SELECT count(*) FROM hy_docs WHERE tenant = 28 AND body @@> $1;
SET plan_cache_mode = force_custom_plan;
EXECUTE hy_p ('common');
EXECUTE hy_p ('mid');
EXECUTE hy_p ('common');
SET plan_cache_mode = force_generic_plan;
EXECUTE hy_p ('common');
EXECUTE hy_p ('mid');
RESET plan_cache_mode;
DEALLOCATE hy_p;
SELECT g AS tenant, (SELECT count(*) FROM hy_docs WHERE tenant = g AND body @@> 'common') AS n FROM generate_series(28, 31) g ORDER BY g;

-- not served: a row-level policy, a volatile qual, a column the store does not hold in a direction B projection
CREATE ROLE hy_reader;
GRANT SELECT ON hy_docs TO hy_reader;
ALTER TABLE hy_docs ENABLE ROW LEVEL SECURITY;
CREATE POLICY hy_p ON hy_docs FOR SELECT TO hy_reader USING (tenant < 5000);
SET ROLE hy_reader;
SELECT hy_dir('SELECT id FROM hy_docs WHERE tenant = 8 AND body @@> ''common''') AS with_policy;
RESET ROLE;
ALTER TABLE hy_docs DISABLE ROW LEVEL SECURITY;
SELECT hy_dir('SELECT id FROM hy_docs WHERE tenant = 8 AND body @@> ''common'' AND random() < 2') AS volatile_qual;
SELECT hy_dir('SELECT note FROM hy_docs WHERE body @@> ''rareterm'' AND amount >= 0') AS uncovered_column;

DROP TABLE hy_docs, hy_q;
DROP FUNCTION hy_dir(text), hy_prop(text, text), hy_check(text);
DROP ROLE hy_reader;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
