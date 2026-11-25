-- A partitioned fact table, whose partitions are sift_tables and heaps with a sift_col index, joined to a dimension (inner, left, right, full, semi and anti join, in either order, with a key held more than
-- once, a filter that prunes partitions, an empty partition) is one SiftAggV6 over its partitions, and so are SELECT DISTINCT and SELECT DISTINCT ON of the partitioned table and SELECT DISTINCT of a
-- dimension's column over the join. Every answer is the heap twin's, which has the same rows and no sift relation. A key or an argument computed from columns of a join is the computed value.
-- Every check prints t.
-- requires: build col
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE EXTENSION pg_sift_helpers;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_bitmapscan = off;

CREATE TABLE pf (id int, k int, v int, t text) PARTITION BY RANGE (id);
CREATE TABLE pf_0 PARTITION OF pf FOR VALUES FROM (0) TO (5000) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE pf_1 PARTITION OF pf FOR VALUES FROM (5000) TO (10000) WITH (autovacuum_enabled = false);
CREATE TABLE pf_2 PARTITION OF pf FOR VALUES FROM (10000) TO (15000) USING sift_table WITH (autovacuum_enabled = false);
CREATE TABLE pf_3 PARTITION OF pf FOR VALUES FROM (15000) TO (20000) WITH (autovacuum_enabled = false);
CREATE INDEX pf_1_col ON pf_1 USING sift_col (id, k, v, t);
CREATE INDEX pf_3_col ON pf_3 USING sift_col (id, k, v, t);
CREATE TABLE ph (LIKE pf) PARTITION BY RANGE (id);
CREATE TABLE ph_0 PARTITION OF ph FOR VALUES FROM (0) TO (5000) WITH (autovacuum_enabled = false);
CREATE TABLE ph_1 PARTITION OF ph FOR VALUES FROM (5000) TO (10000) WITH (autovacuum_enabled = false);
CREATE TABLE ph_2 PARTITION OF ph FOR VALUES FROM (10000) TO (15000) WITH (autovacuum_enabled = false);
CREATE TABLE ph_3 PARTITION OF ph FOR VALUES FROM (15000) TO (20000) WITH (autovacuum_enabled = false);
INSERT INTO pf SELECT g, CASE WHEN g % 211 = 0 THEN NULL ELSE 5 + g % 55 END, g % 97, CASE WHEN g % 31 = 0 THEN NULL ELSE 't' || g % 13 END FROM generate_series(0, 14999) g;
INSERT INTO ph SELECT * FROM pf;

CREATE TABLE pc (id int, k int, v int, t text) WITH (autovacuum_enabled = false);
CREATE INDEX pc_col ON pc USING sift_col (id, k, v, t);
CREATE TABLE pch (LIKE pc) WITH (autovacuum_enabled = false);
INSERT INTO pc SELECT * FROM pf WHERE id < 5000;
INSERT INTO pch SELECT * FROM pc;

CREATE TABLE pd (id int PRIMARY KEY, kind text);
INSERT INTO pd SELECT g, 'kind' || g % 4 FROM generate_series(0, 49) g;
CREATE TABLE pdup (id int, kind text);
INSERT INTO pdup SELECT g % 25, 'dup' || g % 3 FROM generate_series(0, 49) g;
CREATE TABLE pd2 (id int PRIMARY KEY, grp text);
INSERT INTO pd2 SELECT g, 'grp' || g % 3 FROM generate_series(0, 99) g;
ANALYZE pf; ANALYZE ph; ANALYZE pc; ANALYZE pch; ANALYZE pd; ANALYZE pdup; ANALYZE pd2;

CREATE FUNCTION on_table(q text, twin boolean) RETURNS text LANGUAGE sql AS
$$ SELECT replace(replace(q, '@P', CASE WHEN twin THEN 'ph' ELSE 'pf' END), '@C', CASE WHEN twin THEN 'pch' ELSE 'pc' END) $$;

CREATE FUNCTION parity(q text) RETURNS boolean LANGUAGE sql AS
$$ SELECT sift_helpers.same_rows(on_table(q, false), on_table(q, true)) $$;

CREATE FUNCTION plan_has(q text, node text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE 'EXPLAIN (COSTS off) ' || on_table(q, false) LOOP
		IF line LIKE '%' || node || '%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;

CREATE TABLE cases (name text, q text);
INSERT INTO cases VALUES
	('inner', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f JOIN pd d ON d.id = f.k GROUP BY 1'),
	('inner dimension first', 'SELECT d.kind, count(f.id) AS n, sum(f.v) AS s FROM pd d JOIN @P f ON d.id = f.k GROUP BY 1'),
	('left', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f LEFT JOIN pd d ON d.id = f.k GROUP BY 1'),
	('dimension left', 'SELECT d.kind, count(f.id) AS n, sum(f.v) AS s FROM pd d LEFT JOIN @P f ON d.id = f.k GROUP BY 1'),
	('right', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f RIGHT JOIN pd d ON d.id = f.k GROUP BY 1'),
	('dimension right', 'SELECT d.kind, count(f.id) AS n, sum(f.v) AS s FROM pd d RIGHT JOIN @P f ON d.id = f.k GROUP BY 1'),
	('full', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f FULL JOIN pd d ON d.id = f.k GROUP BY 1'),
	('dimension full', 'SELECT d.kind, count(f.id) AS n, sum(f.v) AS s FROM pd d FULL JOIN @P f ON d.id = f.k GROUP BY 1'),
	('semi', 'SELECT f.k % 5 AS g, count(*) AS n FROM @P f WHERE f.k IN (SELECT id FROM pd WHERE kind <> ''kind1'') GROUP BY 1'),
	('anti', 'SELECT f.k % 5 AS g, count(*) AS n FROM @P f WHERE NOT EXISTS (SELECT 1 FROM pd d WHERE d.id = f.k AND d.kind <> ''kind1'') GROUP BY 1'),
	('inner on a key held twice', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f JOIN pdup d ON d.id = f.k GROUP BY 1'),
	('left on a key held twice', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f LEFT JOIN pdup d ON d.id = f.k GROUP BY 1'),
	('right on a key held twice', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f RIGHT JOIN pdup d ON d.id = f.k GROUP BY 1'),
	('full on a key held twice', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f FULL JOIN pdup d ON d.id = f.k GROUP BY 1'),
	('two dimensions', 'SELECT d.kind, e.grp, count(*) AS n FROM @P f JOIN pd d ON d.id = f.k JOIN pd2 e ON e.id = f.v GROUP BY 1, 2'),
	('filter on the fact', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f JOIN pd d ON d.id = f.k WHERE f.v < 50 AND f.k > 10 GROUP BY 1'),
	('filter on the dimension', 'SELECT d.kind, count(*) AS n FROM @P f LEFT JOIN pd d ON d.id = f.k WHERE d.kind = ''kind2'' GROUP BY 1'),
	('pruned partitions', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f RIGHT JOIN pd d ON d.id = f.k WHERE f.id < 7000 GROUP BY 1'),
	('one partition left', 'SELECT d.kind, count(*) AS n, sum(f.v) AS s FROM @P f FULL JOIN pd d ON d.id = f.k WHERE f.id >= 15000 GROUP BY 1'),
	('no aggregate over the dimension', 'SELECT f.t, count(*) AS n FROM @P f JOIN pd d ON d.id = f.k GROUP BY 1'),
	('computed key', 'SELECT d.kind, f.k % 3 AS m, count(*) AS n FROM @P f RIGHT JOIN pd d ON d.id = f.k GROUP BY 1, 2'),
	('computed key of a full join', 'SELECT d.kind, f.k * 2 + 1 AS m, sum(f.v % 5) AS s FROM @P f FULL JOIN pd d ON d.id = f.k GROUP BY 1, 2'),
	('computed key that is not NULL on NULL', 'SELECT d.kind, coalesce(f.k, -1) % 3 AS m, count(*) AS n FROM @P f FULL JOIN pd d ON d.id = f.k GROUP BY 1, 2'),
	('computed key of an inner join', 'SELECT d.kind, length(f.t) AS m, count(*) AS n FROM @P f JOIN pd d ON d.id = f.k GROUP BY 1, 2'),
	('computed argument of an inner join', 'SELECT d.kind, sum(length(f.t)) AS s FROM @P f JOIN pd d ON d.id = f.k GROUP BY 1'),
	('distinct', 'SELECT DISTINCT f.k, f.t FROM @P f WHERE f.k < 30'),
	('distinct pruned', 'SELECT DISTINCT f.k, f.t FROM @P f WHERE f.id < 7000 AND f.k < 30'),
	('distinct on', 'SELECT DISTINCT ON (f.k) f.k, f.id, f.v FROM @P f ORDER BY f.k, f.v DESC, f.id'),
	('distinct over a join', 'SELECT DISTINCT d.kind FROM @C f JOIN pd d ON d.id = f.k'),
	('distinct over a right join', 'SELECT DISTINCT d.kind, f.k % 3 FROM @C f RIGHT JOIN pd d ON d.id = f.k'),
	('distinct over a full join', 'SELECT DISTINCT d.kind FROM @C f FULL JOIN pd d ON d.id = f.k'),
	('distinct over a left join', 'SELECT DISTINCT d.kind, f.t FROM @C f LEFT JOIN pd d ON d.id = f.k'),
	('distinct of the partitions over a join', 'SELECT DISTINCT d.kind FROM @P f JOIN pd d ON d.id = f.k'),
	('distinct of the partitions over a full join', 'SELECT DISTINCT d.kind, f.k FROM @P f FULL JOIN pd d ON d.id = f.k');

SELECT name, parity(q) AS same_as_heap FROM cases ORDER BY name;
SELECT name, plan_has(q, 'Custom Scan (SiftAggV6') AS served_by_agg_v6 FROM cases ORDER BY name;
SELECT name, plan_has(q, 'Partitions: ') AS reads_the_partitions FROM cases WHERE q LIKE '%@P%' AND name <> 'distinct on' ORDER BY name;

DROP TABLE cases, pf, ph, pc, pch, pd, pdup, pd2;
DROP FUNCTION on_table(text, boolean), parity(text), plan_has(text, text);
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
