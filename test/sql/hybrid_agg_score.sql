-- SiftHybridAgg with sift.score(ctid) as the argument of an aggregate: the score of each hit travels with its row through the bridge into the groups, with no heap fetch on an all-visible page.
-- Every result is compared with the same query that the hybrid node refuses (a volatile qual is added), and the node is read from EXPLAIN.
SET client_min_messages = warning;
CREATE EXTENSION pg_sift;
SET sift.test_create_format1 = off;
CREATE TABLE sc_docs (id int PRIMARY KEY, tenant int NOT NULL, category int NOT NULL, region text NOT NULL, amount numeric(10,2) NOT NULL, created date NOT NULL, body text NOT NULL);
INSERT INTO sc_docs
SELECT i, 1 + i % 400, 1 + i % 20, 'r' || (i % 6), ((i * 37) % 100000) / 100.0, DATE '2024-01-01' + (i % 500),
	concat_ws(' ', CASE WHEN i % 2 = 0 THEN 'common' END, CASE WHEN i % 7 = 0 THEN 'seven' END, CASE WHEN i % 5000 = 0 THEN 'rareterm' END,
		CASE WHEN i % 3 = 0 THEN 'three three' END, 'tok' || i % 11, 'w' || (i % 29) || ' w' || (i % 31))
FROM generate_series(1, 40000) i;
CREATE INDEX sc_text ON sc_docs USING sift (body);
CREATE INDEX sc_col ON sc_docs USING sift_col (tenant, id, category, region, amount, created);
ANALYZE sc_docs;
VACUUM (FREEZE) sc_docs;
RESET client_min_messages;
SET enable_nestloop = off;
SET effective_cache_size = '8kB';

CREATE FUNCTION sc_prop(q text, label text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; v text := NULL;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ ('^\s*' || label || ': ') THEN v := substring(l from ': (\S+)$'); END IF;
	END LOOP;
	RETURN v;
END $$;

CREATE FUNCTION sc_node(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; n text := 'none';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftHybrid' THEN n := substring(l from 'Custom Scan \((SiftHybrid[A-Za-z]+)\)'); END IF;
		IF n <> 'none' AND l ~ 'Hybrid Direction: ' THEN n := n || ' ' || substring(l from 'Hybrid Direction: (.+)$'); END IF;
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION sc_cmp(a text, b text, OUT rows bigint, OUT diff bigint) LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE 'CREATE TEMP TABLE sc_a AS ' || a;
	EXECUTE 'CREATE TEMP TABLE sc_b AS ' || b;
	SELECT count(*) INTO rows FROM sc_a;
	SELECT count(*) INTO diff FROM ((TABLE sc_a EXCEPT ALL TABLE sc_b) UNION ALL (TABLE sc_b EXCEPT ALL TABLE sc_a)) d;
	DROP TABLE sc_a, sc_b;
END $$;

CREATE FUNCTION sc_same(q text) RETURNS TABLE (rows bigint, diff bigint) LANGUAGE sql AS
$$ SELECT * FROM sc_cmp(q, regexp_replace(q, 'WHERE ', 'WHERE random() < 2 AND ')) $$;

CREATE TABLE sc_q (name text PRIMARY KEY, q text);
INSERT INTO sc_q VALUES
('avg',       'SELECT category, region, count(*), sum(amount), round(avg(sift.score(ctid))::numeric, 4) FROM sc_docs WHERE body @@> ''common'' GROUP BY category, region'),
('minmax',    'SELECT region, min(sift.score(ctid)), max(sift.score(ctid)), count(sift.score(ctid)) FROM sc_docs WHERE body @@> ''seven'' GROUP BY region'),
('ungrouped', 'SELECT count(*), round(avg(sift.score(ctid))::numeric, 4), min(sift.score(ctid)), max(sift.score(ctid)) FROM sc_docs WHERE body @@> ''three'''),
('or_terms',  'SELECT category, round(avg(sift.score(ctid))::numeric, 4), count(*) FROM sc_docs WHERE body @@> ''seven OR three OR w5'' GROUP BY category'),
('structured','SELECT region, round(avg(sift.score(ctid))::numeric, 4), sum(amount), count(*) FROM sc_docs WHERE category = 7 AND created >= DATE ''2024-06-01'' AND body @@> ''common'' GROUP BY region'),
('range',     'SELECT category, round(avg(sift.score(ctid))::numeric, 4), max(amount) FROM sc_docs WHERE amount > 500 AND category < 6 AND body @@> ''w3 w4'' GROUP BY category'),
('no_hits',   'SELECT category, round(avg(sift.score(ctid))::numeric, 4) FROM sc_docs WHERE body @@> ''nosuchword'' GROUP BY category');

SELECT name, sc_node(q) AS node, (sc_same(q)).* FROM sc_q ORDER BY name;
SELECT name, sc_prop(q, 'Heap Vet Rows') AS heap_vet_rows FROM sc_q WHERE name IN ('avg', 'minmax', 'ungrouped', 'or_terms') ORDER BY name;

-- sum() of a real accumulates as a real in PostgreSQL: the groups' sum is of the same scores to within the rounding of the real
SELECT sc_node('SELECT category, sum(sift.score(ctid)) FROM sc_docs WHERE body @@> ''common'' GROUP BY category') AS sum_node;
WITH h AS (SELECT category, sum(sift.score(ctid)) v FROM sc_docs WHERE body @@> 'common' GROUP BY category),
	 s AS (SELECT category, sum(sift.score(ctid)) v FROM sc_docs WHERE random() < 2 AND body @@> 'common' GROUP BY category)
SELECT count(*) AS groups, max(abs(h.v - s.v) / s.v) < 1e-4 AS within_real_rounding FROM h JOIN s USING (category);

-- the score outside an aggregate belongs to a row the folded scan does not return: the standard plan
SELECT sc_node('SELECT sift.score(ctid) AS sc, count(*) FROM sc_docs WHERE body @@> ''rareterm'' GROUP BY sift.score(ctid)') AS score_key;
SELECT sc_node('SELECT category, count(*) FROM sc_docs WHERE sift.score(ctid) > 0.1 AND body @@> ''common'' GROUP BY category') AS score_qual;
SELECT (sc_same('SELECT category, count(*) FROM sc_docs WHERE sift.score(ctid) > 0.1 AND body @@> ''common'' GROUP BY category')).*;
SELECT sc_node('SELECT category, avg(sift.score(ctid)) FILTER (WHERE amount > 10) FROM sc_docs WHERE body @@> ''common'' GROUP BY category') AS filtered;
SELECT sc_node('SELECT category, max(sift.score(ctid)), count(DISTINCT sift.score(ctid)) FROM sc_docs WHERE body @@> ''common'' GROUP BY category') AS count_distinct;

-- DML of the transaction, and a prepared statement with a Param
BEGIN;
DELETE FROM sc_docs WHERE id % 10 = 0;
UPDATE sc_docs SET amount = amount + 1, region = 'moved' WHERE id % 7 = 0 AND id < 2000;
SELECT (sc_same('SELECT region, count(*), sum(amount), round(avg(sift.score(ctid))::numeric, 4) FROM sc_docs WHERE body @@> ''seven'' GROUP BY region')).*;
ROLLBACK;
-- generic plans of a tenant: a term with few hits reads the rows by key and folds the numeric aggregates at their scale
PREPARE sc_p(int, text) AS SELECT region, count(*), sum(amount), avg(amount), min(amount), max(amount) AS mx, round(avg(sift.score(ctid))::numeric, 4) AS sa, max(sift.score(ctid)) AS ms FROM sc_docs WHERE tenant = $1 AND body @@> $2 GROUP BY region;
PREPARE sc_p_std(int, text) AS SELECT region, count(*), sum(amount), avg(amount), min(amount), max(amount) AS mx, round(avg(sift.score(ctid))::numeric, 4) AS sa, max(sift.score(ctid)) AS ms FROM sc_docs WHERE random() < 2 AND tenant = $1 AND body @@> $2 GROUP BY region;
SET plan_cache_mode = force_generic_plan;
SELECT sc_node('EXECUTE sc_p(9, ''common'')') AS generic_node, sc_node('EXECUTE sc_p(201, ''rareterm'')') AS generic_few_hits,
	sc_prop('EXECUTE sc_p(201, ''rareterm'')', 'Heap Vet Rows') AS few_hits_vet_rows;
SELECT a.args, (sc_cmp(format('EXECUTE sc_p%s', a.args), format('EXECUTE sc_p_std%s', a.args))).*
FROM (VALUES ('(9, ''common'')'), ('(9, ''w3'')'), ('(201, ''common'')'), ('(205, ''w5 OR w6'')'), ('(10, ''nosuchword'')'), ('(401, ''common'')'), ('(1, ''rareterm'')'), ('(201, ''rareterm'')'), ('(201, ''rareterm OR w3'')')) a(args);
RESET plan_cache_mode;

-- sift.enable_agg = off keeps the standard plan, and the same answer
SET sift.enable_agg = off;
SELECT sc_node('SELECT category, round(avg(sift.score(ctid))::numeric, 4) FROM sc_docs WHERE body @@> ''common'' GROUP BY category') AS agg_off;
RESET sift.enable_agg;
DROP FUNCTION sc_cmp(text, text), sc_node(text), sc_prop(text, text), sc_same(text);
DROP TABLE sc_docs, sc_q;
DROP EXTENSION pg_sift;
