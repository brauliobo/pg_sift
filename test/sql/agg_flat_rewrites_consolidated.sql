-- The flat rewrites of src/agg/flat that decorrelate, split, factor, defer and push down aggregates, one segment each, in a schema of its own. Every query runs as the rewrite and as the
-- normal plan (sift.enable_agg off): the plan label and the answers are asserted. Replaces twelve tests of one fixture each.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

-- >>> agg_decor_lookup
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
SET sift.test_create_format1 = on;
-- A scalar sub-select that looks one row up in a table by its whole unique key, and an EXISTS, in the select list, the GROUP BY or an aggregate (its argument or FILTER) of a grouped query
-- cost one probe for each row of the outer table. They become a left join of the table (of the keys of the table that EXISTS asks), which the scan reads in its pass: a row with no match
-- is NULL, as the sub-select is. The rows must be the same as with the scan off, for outer keys that are NULL, that repeat and that have no row in the table, with no worker and with three.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE tenants (id int PRIMARY KEY, region int, name text NOT NULL, UNIQUE (region, id));
INSERT INTO tenants SELECT g, CASE WHEN g % 7 = 0 THEN NULL ELSE 1 + g % 5 END, 'tenant_' || g FROM generate_series(1, 1500) g;
CREATE UNIQUE INDEX tenants_name ON tenants (name);
CREATE TABLE codes (a int, b int, label text, PRIMARY KEY (a, b));
INSERT INTO codes SELECT g % 20, g / 20, 'code_' || g FROM generate_series(0, 399) g;
CREATE TABLE marks (tenant_id int NOT NULL, weight int);
INSERT INTO marks SELECT 1 + (g * 17) % 2000, g % 5 FROM generate_series(1, 3000) g;
CREATE INDEX marks_tenant ON marks (tenant_id);
-- tenant_id: NULL for a tenth of the rows, and 3001 to 3100 are tenants that do not exist
CREATE TABLE events (id int PRIMARY KEY, tenant_id int, category_id int NOT NULL, a int NOT NULL, b int NOT NULL, amount numeric(10,2)) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT g, CASE WHEN g % 10 = 0 THEN NULL WHEN g % 13 = 0 THEN 3001 + g % 100 ELSE 1 + (g * 7919) % 3000 END, 1 + g % 6, g % 20, g % 19,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g % 500) / 5.0 END FROM generate_series(1, 20000) g;
CREATE INDEX events_col ON events USING sift_col (id, tenant_id, category_id, a, b, amount);
VACUUM (FREEZE, ANALYZE) events, tenants, codes, marks;

CREATE TABLE decor_q (n int, label text, q text);
INSERT INTO decor_q VALUES
	(1, 'sum of a looked up column',          'SELECT category_id, sum((SELECT t.region FROM tenants t WHERE t.id = e.tenant_id)) AS s FROM events e GROUP BY 1 ORDER BY 1'),
	(2, 'a looked up column as the group key', 'SELECT (SELECT t.region FROM tenants t WHERE t.id = e.tenant_id) AS r, count(*) AS n, sum(e.amount) AS s FROM events e GROUP BY 1 ORDER BY 1'),
	(3, 'count filtered by EXISTS',           'SELECT category_id, count(*) AS n, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM marks m WHERE m.tenant_id = e.tenant_id)) AS marked FROM events e GROUP BY 1 ORDER BY 1'),
	(4, 'a condition of its own on the table', 'SELECT category_id, count(*) AS n, count((SELECT t.name FROM tenants t WHERE t.id = e.tenant_id AND t.region > 2)) AS c FROM events e GROUP BY 1 ORDER BY 1'),
	(5, 'a key of two columns',                'SELECT category_id, count(*) AS n, max((SELECT c.label FROM codes c WHERE c.a = e.a AND c.b = e.b)) AS l FROM events e GROUP BY 1 ORDER BY 1'),
	(6, 'an expression of the looked up column', 'SELECT category_id, sum((SELECT coalesce(t.region, -1) FROM tenants t WHERE t.id = e.tenant_id)) AS s FROM events e GROUP BY 1 ORDER BY 1'),
	(7, 'first row of a column that is not unique', 'SELECT category_id, count((SELECT t.id FROM tenants t WHERE t.region = e.a + 1 ORDER BY t.id LIMIT 1)) AS c FROM events e GROUP BY 1 ORDER BY 1'),
	(8, 'in the select list, no grouping',    'SELECT e.id, (SELECT t.name FROM tenants t WHERE t.id = e.tenant_id) AS name FROM events e WHERE e.id <= 40 ORDER BY 1'),
	(9, 'compared in the WHERE',               'SELECT category_id, count(*) AS n FROM events e WHERE (SELECT t.region FROM tenants t WHERE t.id = e.tenant_id) = 2 GROUP BY 1 ORDER BY 1'),
	(10, 'a lateral left join',                'SELECT s.region, count(*) AS n FROM events e LEFT JOIN LATERAL (SELECT t.region FROM tenants t WHERE t.id = e.tenant_id) s ON true GROUP BY 1 ORDER BY 1'),
	(11, 'a lateral inner join',               'SELECT s.region, count(*) AS n FROM events e JOIN LATERAL (SELECT t.region FROM tenants t WHERE t.id = e.tenant_id) s ON true GROUP BY 1 ORDER BY 1'),
	(12, 'a filter and a lookup together',     'SELECT (SELECT t.region FROM tenants t WHERE t.id = e.tenant_id) AS r, count(*) FILTER (WHERE e.amount > 50) AS big, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM marks m WHERE m.tenant_id = e.tenant_id AND m.weight = 3)) AS marked FROM events e GROUP BY 1 ORDER BY 1'),
	(13, 'an expression of two looked up columns', 'SELECT category_id, count(*) AS n, max((SELECT coalesce(t.name, ''none'') || t.id::text FROM tenants t WHERE t.id = e.tenant_id)) AS m FROM events e GROUP BY 1 ORDER BY 1'),
	(14, 'an expression that is NULL for a NULL', 'SELECT category_id, sum((SELECT t.region * 2 + 1 FROM tenants t WHERE t.id = e.tenant_id)) AS s, count((SELECT t.region * 2 FROM tenants t WHERE t.id = e.tenant_id)) AS c FROM events e GROUP BY 1 ORDER BY 1');
CREATE FUNCTION decor_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scans int := 0; subs int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scans := scans + (l LIKE '%Custom Scan (SiftAgg%')::int;
		subs := subs + (l LIKE '%SubPlan%')::int;
	END LOOP;
	RETURN CASE WHEN scans > 0 THEN 'scan x' || scans WHEN subs > 0 THEN 'probes' ELSE 'normal' END;
END $$;
CREATE FUNCTION decor_rows(q text, scan bool, workers int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg_with_workers_reset(q, scan, workers) $$;
SELECT n, label, decor_plan(q) AS plan, decor_rows(q, false, 0) <> '' AS has_rows, decor_rows(q, true, 0) = decor_rows(q, false, 0) AS same, decor_rows(q, true, 3) = decor_rows(q, false, 0) AS same_workers FROM decor_q ORDER BY n;

DROP FUNCTION decor_plan(text);
DROP FUNCTION decor_rows(text, bool, int);
DROP TABLE decor_q, events, marks, codes, tenants;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< agg_decor_lookup

-- >>> agg_decorrelate
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
SET sift.test_create_format1 = on;
-- An aggregate of a table tied to the outer query by equalities (a LATERAL sub-select or a scalar sub-select) is the table grouped by those columns, joined to the
-- outer query: the scan runs the grouping once where the normal plan looks up the group of each outer row. The answers are the normal plan's: an outer row whose
-- key has no row gets NULL (0 for a count), a NULL key has none, an outer row that repeats gets its group each time. A few outer rows keep the lookups.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE key_sets (id int PRIMARY KEY, grp int, k int, k2 int, name text) WITH (autovacuum_enabled = false);
-- k repeats (duplicate outer rows), is NULL for some; ids above 1500 own nothing; k2 is NULL for some
INSERT INTO key_sets SELECT i, i % 7, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 1500 END, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 5 END, 'o' || i FROM generate_series(1, 500) i;
CREATE TABLE items (id int PRIMARY KEY, set_id int, owner_k2 int, cat int, amt int, txt text, d date) WITH (autovacuum_enabled = false);
INSERT INTO items SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 1200 END, CASE WHEN i % 19 = 0 THEN NULL ELSE i % 5 END, i % 9, CASE WHEN i % 23 = 0 THEN NULL ELSE (i * 7) % 100 END,
       CASE WHEN i % 29 = 0 THEN NULL ELSE 't' || i % 40 END, date '2024-01-01' + i % 300
FROM generate_series(1, 7500) i;
CREATE INDEX items_col ON items USING sift_col (id, set_id, owner_k2, cat, amt, txt, d);
CREATE INDEX items_owner ON items (set_id);
VACUUM (FREEZE, ANALYZE) key_sets;
VACUUM (FREEZE, ANALYZE) items;

\ir test/agg_helpers.sql
-- in_scan: the plan has a scan (planned with the table scans of the normal plan off); same: the normal plan's rows
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, in_scan bool, same bool, rows bigint) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true), (SELECT count(*) FROM jsonb_array_elements(outcome(q, true)::jsonb));
END $$;
-- the plan the planner chooses, with its own costs: does it read items by the index (the lookups), does it run in the scan?
CREATE FUNCTION chosen(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'other';
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg%' THEN RETURN 'scan'; END IF;
		IF l LIKE '%on items_owner%' OR l LIKE '%using items_owner%' THEN r := 'lookups'; END IF;
	END LOOP;
	RETURN r;
END $$;
CREATE FUNCTION why(q text) RETURNS text LANGUAGE sql AS $$ SELECT string_agg(DISTINCT slug, ', ') FROM sift.explain_why(q) $$;

-- LATERAL aggregates: one row for every outer row, whatever the group
SELECT * FROM check_query('left lateral, sum', $q$ SELECT o.id, x.s FROM key_sets o LEFT JOIN LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = o.k) x ON true $q$);
SELECT * FROM check_query('inner lateral, all aggregates', $q$
 SELECT o.id, x.* FROM key_sets o JOIN LATERAL (SELECT count(*) c, count(i.amt) ca, sum(i.amt) s, min(i.amt) lo, max(i.txt) hi, avg(i.amt) a, array_agg(i.cat ORDER BY i.id) cats
   FROM items i WHERE i.set_id = o.k) x ON true $q$);
SELECT * FROM check_query('count of no rows is 0, an expression of aggregates', $q$
 SELECT o.id, x.c, x.s0, x.r FROM key_sets o LEFT JOIN LATERAL (SELECT count(*) c, coalesce(sum(i.amt), -1) s0, sum(i.amt) / nullif(count(i.amt), 0) r FROM items i WHERE i.set_id = o.k) x ON true $q$);
SELECT * FROM check_query('DISTINCT and FILTER inside the aggregates, a condition of the table', $q$
 SELECT o.id, x.* FROM key_sets o LEFT JOIN LATERAL (SELECT count(DISTINCT i.cat) dc, count(*) FILTER (WHERE i.amt > 50) big, sum(i.amt) FILTER (WHERE i.cat = 3) s3
   FROM items i WHERE i.set_id = o.k AND i.d >= date '2024-03-01' AND i.txt IS NOT NULL) x ON true WHERE o.grp < 4 $q$);
SELECT * FROM check_query('two columns tie the tables', $q$
 SELECT o.id, x.s FROM key_sets o LEFT JOIN LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = o.k AND i.owner_k2 = o.k2) x ON true $q$);
SELECT * FROM check_query('a join of the outer tables', $q$
 SELECT o.id, p.name, x.c FROM key_sets o JOIN key_sets p ON p.id = o.id + 1 LEFT JOIN LATERAL (SELECT count(*) c FROM items i WHERE i.set_id = p.k) x ON true $q$);
SELECT * FROM check_query('the rows of the lateral read in a condition', $q$
 SELECT o.id, x.s FROM key_sets o LEFT JOIN LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = o.k) x ON true WHERE x.s > 300 OR x.s IS NULL $q$);
SELECT * FROM check_query('read by an aggregate of the outer query', $q$
 SELECT o.grp, count(*) n, sum(x.c) c, avg(x.s) s FROM key_sets o LEFT JOIN LATERAL (SELECT count(*) c, sum(i.amt) s FROM items i WHERE i.set_id = o.k) x ON true GROUP BY o.grp $q$);
-- a condition on the aggregate above an INNER join (which the rewrite makes a left join): count 0 is a group that is not there, a group of rows is not 0
SELECT * FROM check_query('inner lateral, count = 0', $q$ SELECT o.id, x.c FROM key_sets o JOIN LATERAL (SELECT count(i.amt) c FROM items i WHERE i.set_id = o.k) x ON true WHERE x.c = 0 $q$);
SELECT * FROM check_query('inner lateral, sum IS NULL', $q$ SELECT o.id FROM key_sets o JOIN LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = o.k) x ON true WHERE x.s IS NULL $q$);
SELECT * FROM check_query('the whole row of the lateral', $q$ SELECT o.id, x FROM key_sets o LEFT JOIN LATERAL (SELECT count(*) c, max(i.amt) m FROM items i WHERE i.set_id = o.k) x ON true WHERE o.id < 400 $q$);
-- the comma form: an inner join of the sub-select to the item of the FROM list that holds the columns it reads
SELECT * FROM check_query('comma lateral', $q$ SELECT o.id, x.s, x.c FROM key_sets o, LATERAL (SELECT sum(i.amt) s, count(*) c FROM items i WHERE i.set_id = o.k) x $q$);
SELECT * FROM check_query('comma lateral under a WHERE on its count', $q$ SELECT o.id, x.c FROM key_sets o, LATERAL (SELECT count(i.amt) c FROM items i WHERE i.set_id = o.k) x WHERE x.c = 0 $q$);
SELECT * FROM check_query('comma lateral after other items', $q$
 SELECT o.id, p.name, x.s FROM key_sets o, key_sets p, LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = o.k AND i.cat < 5) x WHERE p.id = o.id + 1 $q$);
SELECT * FROM check_query('comma lateral of the second item, read by a later lateral', $q$
 SELECT o.id, p.id pid, x.s, y.m FROM key_sets o, key_sets p, LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = p.k) x, LATERAL (SELECT max(i.amt) m FROM items i WHERE i.set_id = x.s) y
 WHERE p.id = o.id + 1 $q$);
SELECT * FROM check_query('comma lateral beside a join lateral: the first is in a join, not in the list', $q$
 SELECT o.id, x.s, y.c FROM key_sets o, LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = o.k) x LEFT JOIN LATERAL (SELECT count(*) c FROM items i WHERE i.set_id = o.k2) y ON true $q$);

SELECT * FROM check_query('a lateral that reads the columns of another lateral', $q$
 SELECT o.id, x.s, y.m FROM key_sets o LEFT JOIN LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = o.k) x ON true LEFT JOIN LATERAL (SELECT max(i.amt) m FROM items i WHERE i.set_id = x.s) y ON true $q$);
-- LATERAL with GROUP BY: a row for each group of an outer row (an inner join gives none for a key without rows, a left join a row of NULLs, a count included)
SELECT * FROM check_query('inner lateral, group by', $q$ SELECT o.id, x.cat, x.s, x.c FROM key_sets o JOIN LATERAL (SELECT i.cat, sum(i.amt) s, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat) x ON true $q$);
SELECT * FROM check_query('left lateral, group by: a count of no group is NULL', $q$ SELECT o.id, x.cat, x.c FROM key_sets o LEFT JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat) x ON true $q$);
SELECT * FROM check_query('left lateral, group by, the rows without a group', $q$ SELECT o.id FROM key_sets o LEFT JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat) x ON true WHERE x.c IS NULL $q$);
SELECT * FROM check_query('comma lateral, group by', $q$ SELECT o.id, x.cat, x.m FROM key_sets o, LATERAL (SELECT i.cat, max(i.amt) m FROM items i WHERE i.set_id = o.k GROUP BY i.cat) x $q$);
SELECT * FROM check_query('group by a column that is NULL for some rows, two columns', $q$
 SELECT o.id, x.txt, x.cat, x.n FROM key_sets o JOIN LATERAL (SELECT i.txt, i.cat, count(i.amt) n FROM items i WHERE i.set_id = o.k GROUP BY i.txt, i.cat) x ON true WHERE o.grp < 3 $q$);
SELECT * FROM check_query('HAVING', $q$ SELECT o.id, x.cat, x.c FROM key_sets o JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat HAVING count(*) > 1 AND min(i.amt) < 50) x ON true $q$);
SELECT * FROM check_query('HAVING, left join: a key whose groups all fail has one row of NULLs', $q$
 SELECT o.id, x.cat FROM key_sets o LEFT JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat HAVING count(*) > 2) x ON true $q$);
SELECT * FROM check_query('expressions of the group and the aggregates, the group column alone', $q$
 SELECT o.id, x.k2, x.r FROM key_sets o JOIN LATERAL (SELECT i.cat + 1 AS k2, sum(i.amt) / nullif(count(i.amt), 0) r FROM items i WHERE i.set_id = o.k GROUP BY i.cat) x ON true $q$);
SELECT * FROM check_query('left lateral, group by: an expression of the aggregates is NULL for a key without a group', $q$
 SELECT o.id, x.cat, x.s0, x.r FROM key_sets o LEFT JOIN LATERAL (SELECT i.cat, coalesce(sum(i.amt), -1) s0, sum(i.amt) / nullif(count(i.amt), 0) r FROM items i WHERE i.set_id = o.k GROUP BY i.cat) x ON true $q$);
SELECT * FROM check_query('groups without an aggregate', $q$ SELECT o.id, x.cat FROM key_sets o JOIN LATERAL (SELECT i.cat FROM items i WHERE i.set_id = o.k AND i.amt > 20 GROUP BY i.cat) x ON true $q$);
SELECT * FROM check_query('the tied column is a group too', $q$ SELECT o.id, x.set_id, x.c FROM key_sets o JOIN LATERAL (SELECT i.set_id, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.set_id) x ON true $q$);
SELECT * FROM check_query('read by an aggregate of the outer query', $q$
 SELECT o.grp, count(*) n, sum(x.c) c FROM key_sets o JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat) x ON true GROUP BY o.grp $q$);
SELECT * FROM check_query('a condition on a group column above a left join', $q$
 SELECT o.id, x.cat FROM key_sets o LEFT JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat) x ON true WHERE x.cat IS NULL OR x.cat = 3 $q$);
-- the rewrite shows in explain_why as the arm that is used
SELECT bool_or(used) AS rewritten FROM sift.explain_why($q$ SELECT o.id, x.c FROM key_sets o JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat) x ON true $q$);
SELECT bool_or(used) AS limited FROM sift.explain_why($q$ SELECT o.id, x.c FROM key_sets o JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat ORDER BY 1 LIMIT 1) x ON true $q$);
-- as written: an ORDER BY or a LIMIT, a HAVING without groups (it can drop the one row), grouping sets, an outer column in a group or the HAVING, a scalar sub-select with groups
SELECT * FROM check_query('group by with a limit', $q$ SELECT o.id, x.cat FROM key_sets o JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat ORDER BY 2 DESC, 1 LIMIT 1) x ON true WHERE o.id < 500 $q$);
SELECT * FROM check_query('HAVING without groups', $q$ SELECT o.id, x.c FROM key_sets o JOIN LATERAL (SELECT count(*) c FROM items i WHERE i.set_id = o.k HAVING count(*) > 5) x ON true $q$);
SELECT * FROM check_query('grouping sets', $q$ SELECT o.id, x.cat, x.c FROM key_sets o JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY ROLLUP (i.cat)) x ON true WHERE o.id < 500 $q$);
SELECT * FROM check_query('an outer column in the HAVING', $q$ SELECT o.id, x.cat FROM key_sets o JOIN LATERAL (SELECT i.cat, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat HAVING count(*) > o.grp) x ON true $q$);
SELECT * FROM check_query('an outer column in the group', $q$ SELECT o.id, x.g FROM key_sets o JOIN LATERAL (SELECT i.cat + o.grp g, count(*) c FROM items i WHERE i.set_id = o.k GROUP BY i.cat + o.grp) x ON true WHERE o.id < 500 $q$);
SELECT * FROM check_query('a scalar sub-select with groups', $q$ SELECT o.id, (SELECT count(*) FROM items i WHERE i.set_id = o.k GROUP BY i.cat HAVING i.cat = 3) c FROM key_sets o $q$);

-- scalar sub-selects, in the select list and the WHERE
SELECT * FROM check_query('scalar in the select list', $q$ SELECT o.id, (SELECT sum(i.amt) FROM items i WHERE i.set_id = o.k) s, (SELECT count(*) FROM items i WHERE i.set_id = o.k) c FROM key_sets o $q$);
SELECT * FROM check_query('scalar in the WHERE', $q$ SELECT o.id FROM key_sets o WHERE (SELECT count(*) FROM items i WHERE i.set_id = o.k) = 0 $q$);
SELECT * FROM check_query('scalar in the WHERE, NULL when none', $q$ SELECT o.id FROM key_sets o WHERE (SELECT max(i.amt) FROM items i WHERE i.set_id = o.k) > 90 $q$);
SELECT * FROM check_query('scalar under a join and an ORDER BY', $q$
 SELECT o.id, p.name, (SELECT min(i.d) FROM items i WHERE i.set_id = p.k) first_d FROM key_sets o JOIN key_sets p ON p.id = o.id + 1 ORDER BY 3 NULLS FIRST, 1 $q$);
-- a few outer rows: the lookups; many: the scan
SELECT chosen($q$ SELECT o.id, (SELECT sum(i.amt) FROM items i WHERE i.set_id = o.k) s FROM key_sets o WHERE o.id < 3 $q$) AS few, chosen($q$ SELECT o.id, (SELECT sum(i.amt) FROM items i WHERE i.set_id = o.k) s FROM key_sets o $q$) AS many;
SELECT chosen($q$ SELECT o.id, x.s FROM key_sets o LEFT JOIN LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = o.k) x ON true WHERE o.id < 3 $q$) AS few,
       chosen($q$ SELECT o.id, x.s FROM key_sets o LEFT JOIN LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = o.k) x ON true $q$) AS many;

-- as written: the table's column is read outside the equalities, the tie is not an equality of the same type or is an expression, the sub-select groups, limits or joins,
-- the outer query groups, the join has a condition of its own
SELECT * FROM check_query('outer column in an aggregate (the correlated child of before)', $q$ SELECT o.id, x.s FROM key_sets o LEFT JOIN LATERAL (SELECT sum(i.amt * o.grp) s FROM items i WHERE i.set_id = o.k) x ON true $q$);
SELECT * FROM check_query('an inequality', $q$ SELECT o.id, x.c FROM key_sets o LEFT JOIN LATERAL (SELECT count(*) c FROM items i WHERE i.set_id < o.k AND i.set_id > o.k - 3) x ON true WHERE o.id < 500 $q$);
SELECT * FROM check_query('an expression of the outer column', $q$ SELECT o.id, x.c FROM key_sets o LEFT JOIN LATERAL (SELECT count(*) c FROM items i WHERE i.set_id = o.k + 1) x ON true $q$);
SELECT * FROM check_query('a limit', $q$ SELECT o.id, x.s FROM key_sets o LEFT JOIN LATERAL (SELECT sum(i.amt) s FROM (SELECT * FROM items j WHERE j.set_id = o.k LIMIT 3) i) x ON true $q$);
SELECT * FROM check_query('a join condition of its own', $q$ SELECT o.id, x.s FROM key_sets o LEFT JOIN LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id = o.k) x ON x.s > 100 $q$);
SELECT * FROM check_query('the outer query groups', $q$ SELECT o.grp, sum((SELECT sum(i.amt) FROM items i WHERE i.set_id = o.k)) s FROM key_sets o GROUP BY o.grp $q$);
SELECT * FROM check_query('an aggregate with a value over no rows', $q$ SELECT o.id, (SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY i.amt) FROM items i WHERE i.set_id = o.k) m FROM key_sets o $q$);
SELECT * FROM check_query('comma lateral with a condition the lateral cannot move',$q$ SELECT o.id, x.s FROM key_sets o, LATERAL (SELECT sum(i.amt) s FROM items i WHERE i.set_id < o.k AND i.set_id > o.k - 3) x WHERE o.id < 500 $q$);
SELECT * FROM check_query('EXISTS', $q$ SELECT o.id FROM key_sets o WHERE EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt > 90) $q$);

DROP TABLE key_sets, items;
DROP FUNCTION check_query(text,text), chosen(text), why(text), outcome(text,boolean), runs_in_scan(text), runq(text,boolean), pushed(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_decorrelate

-- >>> agg_exists_decorrelate
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
SET sift.test_create_format1 = on;
-- EXISTS and NOT EXISTS of a table tied to the outer query by equalities, where PostgreSQL leaves a sub-plan for each outer row (the select list, an OR, a CASE), are
-- the table grouped by those columns, left joined to the outer query: EXISTS is "a group is there". The answers are the normal plan's, bit for bit: a NULL key has no
-- group (EXISTS is false, NOT EXISTS true), nor has a key of an empty table, a key that repeats in the outer query gets its answer each time. An EXISTS that the WHERE ANDs
-- stays a semi join (a NOT EXISTS an anti join), which the scan has arms for. A few outer rows keep the lookups.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE key_sets (id int PRIMARY KEY, grp int, k int, k2 int, tx text, dd date, k8 bigint) WITH (autovacuum_enabled = false);
-- k repeats (duplicate outer rows), is NULL for some; ids above 1500 own nothing; k2, tx, dd and k8 are NULL for some
INSERT INTO key_sets SELECT i, i % 7, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 1500 END, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 5 END,
       CASE WHEN i % 17 = 0 THEN NULL ELSE 't' || i % 60 END, CASE WHEN i % 19 = 0 THEN NULL ELSE date '2024-01-01' + i % 400 END, CASE WHEN i % 23 = 0 THEN NULL ELSE i % 900 END
FROM generate_series(1, 2000) i;
CREATE TABLE items (id int PRIMARY KEY, set_id int, owner_k2 int, cat int, amt int, txt text, d date, k8 bigint) WITH (autovacuum_enabled = false);
INSERT INTO items SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 1200 END, CASE WHEN i % 19 = 0 THEN NULL ELSE i % 5 END, i % 9, CASE WHEN i % 23 = 0 THEN NULL ELSE (i * 7) % 100 END,
       CASE WHEN i % 29 = 0 THEN NULL ELSE 't' || i % 40 END, date '2024-01-01' + i % 300, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 700 END
FROM generate_series(1, 30000) i;
CREATE INDEX items_col ON items USING sift_col (id, set_id, owner_k2, cat, amt, txt, d, k8);
CREATE INDEX items_owner ON items (set_id);
CREATE TABLE nothing (id int, set_id int, amt int);
CREATE INDEX nothing_col ON nothing USING sift_col (id, set_id, amt);
VACUUM (FREEZE, ANALYZE) key_sets;
VACUUM (FREEZE, ANALYZE) items;

\ir test/agg_helpers.sql
-- in_scan: the plan has a scan (planned with the table scans of the normal plan off); same: the normal plan's rows
CREATE FUNCTION check_query(name text, q text) RETURNS TABLE (label text, in_scan bool, same bool, rows bigint) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT name, runs_in_scan(q), outcome(q, false) = outcome(q, true), (SELECT count(*) FROM jsonb_array_elements(outcome(q, true)::jsonb));
END $$;
-- the plan the planner chooses, with its own costs: does it read items by the index (the lookups), does it run in the scan, does it keep a semi or anti join?
CREATE FUNCTION chosen(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'other';
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg%' THEN RETURN 'scan'; END IF;
		IF l LIKE '%Semi Join%' OR l LIKE '%Anti Join%' THEN r := 'semi/anti'; END IF;
		IF r = 'other' AND (l LIKE '%on items_owner%' OR l LIKE '%using items_owner%') THEN r := 'lookups'; END IF;
	END LOOP;
	RETURN r;
END $$;

-- the select list
SELECT * FROM check_query('EXISTS in the select list', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k) AS has FROM key_sets o $q$);
SELECT * FROM check_query('NOT EXISTS in the select list', $q$ SELECT o.id, NOT EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k) AS none FROM key_sets o $q$);
SELECT * FROM check_query('both, a condition of the table', $q$
 SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt > 90) AS big, NOT EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.d >= date '2024-09-01' AND i.txt IS NOT NULL) AS none
 FROM key_sets o $q$);
-- where the WHERE cannot make a join of it
SELECT * FROM check_query('under an OR', $q$ SELECT o.id FROM key_sets o WHERE o.grp = 1 OR EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt > 95) $q$);
SELECT * FROM check_query('NOT EXISTS under an OR', $q$ SELECT o.id FROM key_sets o WHERE o.grp = 1 OR NOT EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt > 50) $q$);
SELECT * FROM check_query('in a CASE', $q$ SELECT o.id, CASE WHEN EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k) THEN 'a' WHEN o.grp > 3 THEN 'b' ELSE 'c' END AS c FROM key_sets o $q$);
SELECT * FROM check_query('NOT (a AND EXISTS)', $q$ SELECT o.id FROM key_sets o WHERE NOT (o.grp < 5 AND EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt < 5)) $q$);
SELECT * FROM check_query('with the other conjuncts of a WHERE that has an OR', $q$
 SELECT o.id FROM key_sets o WHERE o.id > 100 AND (EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.cat = 3) OR o.k2 = 2) AND NOT EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt = 7) $q$);
-- keys: NULL, repeated, of several types, two columns, an empty table, a table whose rows the condition drops
SELECT * FROM check_query('a NULL key has no group', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k) AS has FROM key_sets o WHERE o.k IS NULL $q$);
SELECT * FROM check_query('text key', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.txt = o.tx) AS has FROM key_sets o $q$);
SELECT * FROM check_query('date key', $q$ SELECT o.id, NOT EXISTS (SELECT 1 FROM items i WHERE i.d = o.dd AND i.cat = 4) AS none FROM key_sets o $q$);
SELECT * FROM check_query('bigint key', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.k8 = o.k8) AS has FROM key_sets o $q$);
SELECT * FROM check_query('two columns', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.owner_k2 = o.k2) AS has FROM key_sets o $q$);
SELECT * FROM check_query('the same sub-select twice, a different condition', $q$
 SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt < 3) AS lo, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt > 97) AS hi FROM key_sets o $q$);
SELECT * FROM check_query('an empty table', $q$ SELECT o.id, EXISTS (SELECT 1 FROM nothing n WHERE n.set_id = o.k) AS has, NOT EXISTS (SELECT 1 FROM nothing n WHERE n.set_id = o.k) AS none FROM key_sets o $q$);
SELECT * FROM check_query('a condition that no row meets', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt > 1000) AS has FROM key_sets o $q$);
SELECT * FROM check_query('an outer join in the FROM list', $q$
 SELECT o.id, p.id AS pid, EXISTS (SELECT 1 FROM items i WHERE i.set_id = p.k) AS has FROM key_sets o LEFT JOIN key_sets p ON p.id = o.id + 1500 $q$);
SELECT * FROM check_query('a join of the outer tables', $q$
 SELECT o.id, p.tx, EXISTS (SELECT 1 FROM items i WHERE i.set_id = p.k AND i.cat = 2) AS has FROM key_sets o JOIN key_sets p ON p.id = o.id + 1 $q$);
-- a few outer rows: the lookups; many: the scan; a WHERE that makes a join of it stays one
SELECT chosen($q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k) AS has FROM key_sets o WHERE o.id < 3 $q$) AS few, chosen($q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k) AS has FROM key_sets o $q$) AS many;
SELECT chosen($q$ SELECT o.id FROM key_sets o WHERE EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt > 90) $q$) AS exists_in_where,
       chosen($q$ SELECT o.id FROM key_sets o WHERE NOT EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt > 90) $q$) AS not_exists_in_where;

-- as written: the outer query groups, no equality, an equality with an expression, an outer column beside the equality, a LIMIT, a table that groups or joins, a sub-select inside it, IN and NOT IN (their NULLs differ)
SELECT * FROM check_query('the outer query groups', $q$
 SELECT o.grp, count(*) AS n, count(*) FILTER (WHERE EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt > 80)) AS big FROM key_sets o GROUP BY o.grp $q$);
SELECT * FROM check_query('an inequality', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id < o.k AND i.set_id > o.k - 3) AS has FROM key_sets o WHERE o.id < 500 $q$);
SELECT * FROM check_query('an expression of the outer column', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k + 1) AS has FROM key_sets o $q$);
SELECT * FROM check_query('a second equality, of two columns of the same types', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.cat = o.grp) AS has FROM key_sets o $q$);
SELECT * FROM check_query('a LIMIT 0', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k LIMIT 0) AS has FROM key_sets o $q$);
SELECT * FROM check_query('a join inside', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i JOIN items j ON j.id = i.id + 1 WHERE i.set_id = o.k) AS has FROM key_sets o $q$);
SELECT * FROM check_query('a sub-select inside (uncorrelated: the scan runs it on its own)', $q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k AND i.amt > (SELECT avg(amt) FROM items)) AS has FROM key_sets o $q$);
SELECT * FROM check_query('IN', $q$ SELECT o.id, o.k IN (SELECT i.set_id FROM items i WHERE i.amt > 90) AS has FROM key_sets o $q$);
SELECT * FROM check_query('NOT IN (a NULL among the keys)', $q$ SELECT o.id, o.k NOT IN (SELECT i.set_id FROM items i WHERE i.amt > 90) AS none FROM key_sets o $q$);
SELECT * FROM check_query('a table without a column index', $q$ SELECT o.id, EXISTS (SELECT 1 FROM key_sets p WHERE p.k = o.k AND p.id <> o.id) AS has FROM key_sets o $q$);
-- the rewrite shows in explain_why as the arm that is used, and not where it was refused
SELECT bool_or(used) AS rewritten FROM sift.explain_why($q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id = o.k) AS has FROM key_sets o $q$);
SELECT bool_or(used) AS inequality FROM sift.explain_why($q$ SELECT o.id, EXISTS (SELECT 1 FROM items i WHERE i.set_id < o.k AND i.set_id > o.k - 3) AS has FROM key_sets o $q$);

DROP TABLE key_sets, items, nothing;
DROP FUNCTION check_query(text,text), chosen(text), outcome(text,boolean), runs_in_scan(text), runq(text,boolean), pushed(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_exists_decorrelate

-- >>> agg_exists_or_split
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
SET sift.test_create_format1 = on;
-- A correlated EXISTS whose WHERE is an OR of correlated tests (`link.key = d.id OR link.group_id = ANY(ARRAY(SELECT .. FROM group_links WHERE key = d.id))`) cannot be pulled up
-- into a join by PostgreSQL: the plan is a semi join with a filter that runs the sub-select for every pair of rows (121,975 runs of a sequential scan for 500 x 425 rows), a cliff on
-- small link tables. Each arm is a test of one key, so the EXISTS is the key in the union of the keys the arms give, computed once (an IN sub-select, hashed), with the NULL
-- keys left out so that the answer is true or false as EXISTS is. Each statement gives the rows of the plan without the extension.
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
CREATE TABLE dim_a (id int PRIMARY KEY, label text NOT NULL);
INSERT INTO dim_a SELECT g, 'a' || (g * 7919) % 1000 FROM generate_series(1, 500) g;
CREATE TABLE links_a (id int PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, set_id int, deleted_at timestamp);
INSERT INTO links_a SELECT g, CASE WHEN g % 6 = 0 THEN 1 + g % 3 ELSE 4 END, CASE WHEN g % 3 <> 0 THEN 1 + (g * 13) % 500 END, CASE WHEN g % 3 = 0 THEN g % 9 END,
       CASE WHEN g % 7 = 0 THEN 1 + (g * 31) % 500 END, CASE WHEN g % 11 = 0 THEN timestamp '2025-05-01' END FROM generate_series(1, 497) g;
CREATE TABLE group_links (id int PRIMARY KEY, group_id int NOT NULL, dim_a_id int, deleted_at timestamp);
INSERT INTO group_links SELECT g, g % 9, CASE WHEN g % 17 <> 0 THEN 1 + (g * 29) % 500 END, CASE WHEN g % 13 = 0 THEN timestamp '2025-05-01' END FROM generate_series(1, 150) g;
VACUUM (FREEZE, ANALYZE) dim_a, links_a, group_links;

CREATE TABLE forms (name text PRIMARY KEY, pred text);
INSERT INTO forms VALUES
	('key or group of the key', $p$EXISTS (SELECT 1 FROM links_a l WHERE l.deleted_at IS NULL AND l.tenant_id = 4 AND (l.dim_a_id = d.id OR l.group_id = ANY(ARRAY(SELECT g.group_id FROM group_links g WHERE g.deleted_at IS NULL AND g.dim_a_id = d.id))))$p$),
	('not exists', $p$NOT EXISTS (SELECT 1 FROM links_a l WHERE l.deleted_at IS NULL AND l.tenant_id = 4 AND (l.dim_a_id = d.id OR l.group_id = ANY(ARRAY(SELECT g.group_id FROM group_links g WHERE g.deleted_at IS NULL AND g.dim_a_id = d.id))))$p$),
	('two keys', $p$EXISTS (SELECT 1 FROM links_a l WHERE l.tenant_id = 4 AND (l.dim_a_id = d.id OR l.set_id = d.id))$p$),
	('key or group, in an or', $p$(d.label < 'a3' OR EXISTS (SELECT 1 FROM links_a l WHERE l.deleted_at IS NULL AND l.tenant_id = 4 AND (l.dim_a_id = d.id OR l.group_id = ANY(ARRAY(SELECT g.group_id FROM group_links g WHERE g.deleted_at IS NULL AND g.dim_a_id = d.id)))))$p$),
	('an arm that is no key test', $p$EXISTS (SELECT 1 FROM links_a l WHERE l.tenant_id = 4 AND (l.dim_a_id = d.id OR l.id < 3))$p$),
	('a key from an expression', $p$EXISTS (SELECT 1 FROM links_a l WHERE l.tenant_id = 4 AND (l.dim_a_id = d.id + 1 OR l.set_id = d.id * 2))$p$),
	('a limit inside', $p$EXISTS (SELECT 1 FROM links_a l WHERE l.tenant_id = 4 AND (l.dim_a_id = d.id OR l.set_id = d.id) LIMIT 1)$p$);

-- how many times the plan scans a link table (the runs of a sub-select for each pair of rows), and whether the rows are the plan without the extension's
CREATE FUNCTION link_scans(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE p jsonb;
BEGIN
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) ' || q INTO p;
	RETURN coalesce((SELECT max((n ->> 'Actual Loops')::bigint) FROM jsonb_path_query(p, 'strict $.**."Plans"[*] ? (@."Relation Name" == "group_links" || @."Relation Name" == "links_a")') n), 0);
END $$;
CREATE FUNCTION rows_equal(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	EXECUTE format('SELECT string_agg(r::text, ''|'' ORDER BY r::text) FROM (%s) r', q) INTO a;
	PERFORM set_config('sift.enable_agg', 'on', true);
	EXECUTE format('SELECT string_agg(r::text, ''|'' ORDER BY r::text) FROM (%s) r', q) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;
SELECT name, link_scans(q) <= 2 AS keys_once, rows_equal(q) AS rows_equal
FROM (SELECT name, 'SELECT d.id, d.label FROM dim_a d WHERE ' || pred AS q FROM forms) t ORDER BY name;

DROP FUNCTION link_scans(text), rows_equal(text);
DROP TABLE forms, dim_a, links_a, group_links;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< agg_exists_or_split

-- >>> agg_distinct_split
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
SET sift.test_create_format1 = on;
-- Distinct counts of one parent key under filters of the parent's own columns share one set: the scan groups by the filters' outcomes and counts each id once, and the
-- statement adds up the counts of the groups each filter keeps. The plans are the planner's own (nothing is forced): the split is the scan's where it costs less, and the answers are
-- those of the normal plan. The tables are 500,000 parents and 1,000,000 rows (under 10 s): the scan saves the sets of the ids only where they are large, and the planner keeps the
-- plan of a set for each count on smaller tables.
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET client_min_messages = warning;

CREATE TABLE par (id int PRIMARY KEY, a text, b text, c text, lat float8, lon float8) WITH (autovacuum_enabled = false);
INSERT INTO par SELECT i, CASE WHEN i % 3 = 0 THEN NULL ELSE 'a' || i END, CASE WHEN i % 5 = 0 THEN '  ' ELSE 'b' END,
	CASE WHEN i % 7 = 0 THEN '' ELSE 'c' END, CASE WHEN i % 11 = 0 THEN NULL ELSE i END, CASE WHEN i % 13 = 0 THEN NULL ELSE i END
FROM generate_series(1, 15625) i;
CREATE INDEX par_col ON par USING sift_col (id, (nullif(btrim(a), '') IS NOT NULL), (nullif(btrim(b), '') IS NOT NULL), (nullif(btrim(c), '') IS NOT NULL),
	(lat IS NOT NULL AND lon IS NOT NULL));
CREATE TABLE grp (id int PRIMARY KEY, name text) WITH (autovacuum_enabled = false);
INSERT INTO grp SELECT i, 'grp' || i FROM generate_series(1, 20) i;
CREATE TABLE child (id int, foreign_id int, par_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO child SELECT i, 1 + i % 20, 1 + (i * 7) % 500000, i % 11 FROM generate_series(1, 31250) i;
CREATE INDEX child_col ON child USING sift_col (id, foreign_id, par_id, v);
VACUUM (FREEZE, ANALYZE) par, grp, child;

CREATE TABLE shapes (n int, sql text);
INSERT INTO shapes VALUES
 (1, $q$SELECT grp.id, grp.name, count(DISTINCT par.id), count(DISTINCT par.id) FILTER (WHERE nullif(btrim(par.a), '') IS NOT NULL),
		count(DISTINCT par.id) FILTER (WHERE nullif(btrim(par.b), '') IS NOT NULL), count(DISTINCT par.id) FILTER (WHERE nullif(btrim(par.c), '') IS NOT NULL),
		count(DISTINCT par.id) FILTER (WHERE par.lat IS NOT NULL AND par.lon IS NOT NULL)
		FROM child JOIN par ON par.id = child.par_id JOIN grp ON grp.id = child.foreign_id GROUP BY grp.id, grp.name ORDER BY grp.name$q$),
 (2, $q$SELECT grp.id, count(DISTINCT par.id), round(100.0 * count(DISTINCT par.id) FILTER (WHERE nullif(btrim(par.a), '') IS NOT NULL) / nullif(count(DISTINCT par.id), 0), 2),
		count(DISTINCT par.id) FILTER (WHERE nullif(btrim(par.b), '') IS NOT NULL), count(DISTINCT par.id) FILTER (WHERE nullif(btrim(par.c), '') IS NOT NULL),
		count(DISTINCT par.id) FILTER (WHERE par.lat IS NOT NULL AND par.lon IS NOT NULL)
		FROM child JOIN par ON par.id = child.par_id JOIN grp ON grp.id = child.foreign_id GROUP BY grp.id ORDER BY grp.id$q$),
 (3, $q$SELECT grp.id, count(DISTINCT par.id), count(DISTINCT par.id) FILTER (WHERE nullif(btrim(par.a), '') IS NOT NULL), count(*)
		FROM child JOIN par ON par.id = child.par_id JOIN grp ON grp.id = child.foreign_id WHERE child.v < 5 GROUP BY grp.id ORDER BY grp.id$q$),
 (4, $q$SELECT grp.id, count(DISTINCT child.id), count(DISTINCT par.id) FILTER (WHERE nullif(btrim(par.a), '') IS NOT NULL)
		FROM child JOIN par ON par.id = child.par_id JOIN grp ON grp.id = child.foreign_id GROUP BY grp.id ORDER BY grp.id$q$);

-- the groups the scan makes: one set of ids when the split is the plan, one for each filter otherwise
CREATE FUNCTION chosen(sql text, OUT scan bool, OUT sets int) LANGUAGE plpgsql AS $$
DECLARE l text; BEGIN
	scan := false; sets := 0;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || sql LOOP
		scan := scan OR l ~ 'Custom Scan \(SiftAgg';
		IF l ~ 'Pushed Aggregates' THEN sets := (SELECT count(*) FROM regexp_matches(l, 'count\(DISTINCT', 'g')); END IF;
	END LOOP;
END $$;

SELECT n, (c).scan AS scan, (c).sets AS distinct_sets, sift_helpers.answer(sql, true) = sift_helpers.answer(sql, false) AS same_answers
FROM (SELECT n, sql, chosen(sql) AS c FROM shapes) s ORDER BY n;

DROP FUNCTION chosen(text);
DROP TABLE shapes, child, grp, par;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< agg_distinct_split

-- >>> agg_eager_lookup
CREATE SCHEMA s6;
GRANT USAGE ON SCHEMA s6 TO PUBLIC;
SET search_path = s6, public;
SET sift.test_create_format1 = on;
-- A sum of a numeric column times a value of a table joined by a unique key (`sum(e.amount * k.rate) .. JOIN kinds k ON k.id = e.kind_id`) adds up the amounts for each key first: a
-- sub-select groups the rows by the keys of the query and the key of the lookup, and the query above joins it to the lookup and adds up the products of the groups. The rows must be those of
-- the scan off, with no worker and with three, for keys that are NULL or that the lookup has no row for, columns and rates that are NULL, numerics of different scales and NaN. A sum that is not of a
-- product of numerics, a lookup whose key repeats, a column of the lookup read in a group or a condition, a column compared with a constant (the normal plan reads those rows through an index) and a join that is not an inner one stay as they are.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE tenants (id int PRIMARY KEY, region int NOT NULL, name text NOT NULL);
INSERT INTO tenants SELECT g, 1 + g % 6, 'tenant_' || g FROM generate_series(1, 300) g;
-- rate: NULL for a kind, nrate: numerics of different scales
CREATE TABLE kinds (id int PRIMARY KEY, rate numeric(8,3), nrate numeric, name text NOT NULL);
INSERT INTO kinds SELECT g, CASE WHEN g % 9 = 0 THEN NULL ELSE g / 8.0 END, CASE WHEN g % 7 = 0 THEN 'NaN'::numeric WHEN g % 5 = 0 THEN NULL ELSE g / (CASE WHEN g % 2 = 0 THEN 4.0 ELSE 400.0 END) END, 'kind_' || g FROM generate_series(1, 40) g;
CREATE TABLE kinds_dup (id int, rate numeric(8,3));
INSERT INTO kinds_dup SELECT g % 30, g / 8.0 FROM generate_series(1, 60) g;
CREATE TABLE events (id int PRIMARY KEY, tenant_id int NOT NULL, kind_id int, status int NOT NULL, amount numeric(10,2), loose numeric, f float8) WITH (autovacuum_enabled = false);
-- kinds 41 to 45 are not in the lookup, a tenth of the rows have no kind
INSERT INTO events SELECT g, 1 + (g * 7919) % 300, CASE WHEN g % 10 = 0 THEN NULL ELSE 1 + (g * 31) % 45 END, 1 + g % 4, CASE WHEN g % 13 = 0 THEN NULL ELSE (g % 500) / 5.0 END,
       CASE WHEN g % 997 = 0 THEN 'NaN'::numeric WHEN g % 7 = 0 THEN NULL ELSE (g % 300) / (CASE WHEN g % 3 = 0 THEN 7.0 ELSE 70.0 END) END, g / 3.0 FROM generate_series(1, 60000) g;
-- a tenant with rows of kinds 41 to 45 only
INSERT INTO events SELECT 100000 + g, 301, 41 + g % 5, 1, g, g, g FROM generate_series(1, 50) g;
INSERT INTO tenants VALUES (301, 7, 'orphans');
CREATE INDEX events_col ON events USING sift_col (id, tenant_id, kind_id, status, amount, loose, f) WITH (key_cut = tenant_id);
VACUUM (FREEZE, ANALYZE) events, tenants, kinds, kinds_dup;

CREATE TABLE eager_q (n int, label text, q text);
INSERT INTO eager_q VALUES
	(1, 'a sum by region',                 'SELECT t.region, sum(e.amount * k.rate) AS s FROM events e JOIN tenants t ON t.id = e.tenant_id JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(2, 'the lookup on the left',          'SELECT t.region, sum(k.rate * e.amount) AS s FROM events e JOIN tenants t ON t.id = e.tenant_id JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(3, 'a group of the fact table',       'SELECT e.status, sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(4, 'no group',                        'SELECT sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id'),
	(5, 'scales that differ, and NaN',     'SELECT e.status, sum(e.loose * k.nrate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(6, 'two sums of two columns',         'SELECT t.region, sum(e.amount * k.rate) AS s, sum(e.loose * k.rate) AS m FROM events e JOIN tenants t ON t.id = e.tenant_id JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(7, 'the same sum twice',              'SELECT e.status, sum(e.amount * k.rate) AS s, sum(e.amount * k.rate) + 1 AS s1 FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(8, 'a having and an order',           'SELECT t.region, sum(e.amount * k.rate) AS s FROM events e JOIN tenants t ON t.id = e.tenant_id JOIN kinds k ON k.id = e.kind_id GROUP BY 1 HAVING sum(e.amount * k.rate) > 1000 ORDER BY 2 DESC'),
	(9, 'a column compared with a constant',   'SELECT t.region, sum(e.amount * k.rate) AS s FROM events e JOIN tenants t ON t.id = e.tenant_id JOIN kinds k ON k.id = e.kind_id WHERE e.status < 3 AND t.region <> 2 GROUP BY 1'),
	(10, 'the tables listed with a comma',  'SELECT t.region, sum(e.amount * k.rate) AS s FROM events e, tenants t, kinds k WHERE k.id = e.kind_id AND t.id = e.tenant_id GROUP BY 1'),
	(11, 'an expression of the factor',     'SELECT e.status, sum((e.amount + 1) * (k.rate * 2)) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(12, 'an expression of the group',      'SELECT e.status % 2 AS parity, sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(13, 'tenants with kinds the lookup lacks', 'SELECT t.name, sum(e.amount * k.rate) AS s FROM events e JOIN tenants t ON t.id = e.tenant_id JOIN kinds k ON k.id = e.kind_id WHERE t.region = 7 GROUP BY 1'),
	(14, 'an average',                      'SELECT e.status, avg(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(15, 'a count next to the sum',        'SELECT e.status, count(*) AS n, sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(16, 'a lookup whose key repeats',     'SELECT e.status, sum(e.amount * k.rate) AS s FROM events e JOIN kinds_dup k ON k.id = e.kind_id GROUP BY 1'),
	(17, 'a column of the lookup in a group', 'SELECT k.name, sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(18, 'a condition on the lookup',      'SELECT e.status, sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id AND k.name <> ''kind_3'' GROUP BY 1'),
	(19, 'a left join',                    'SELECT e.status, sum(e.amount * k.rate) AS s FROM events e LEFT JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(20, 'a sum of the factor beside',      'SELECT e.status, sum(e.amount * k.rate) AS s, sum(e.amount) AS t FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(21, 'a distinct sum',                 'SELECT e.status, sum(DISTINCT e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(22, 'a count of a column with NULLs',  'SELECT e.status, count(e.amount) AS n, sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(23, 'a count and no group',            'SELECT count(*) AS n, sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id'),
	(24, 'a having on the count',           'SELECT e.status, count(*) AS n, sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1 HAVING count(*) > 100 ORDER BY count(*) DESC, 1'),
	(25, 'a count of distinct values',      'SELECT e.status, count(DISTINCT e.tenant_id) AS n, sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(26, 'a count with a filter',           'SELECT e.status, count(*) FILTER (WHERE e.amount > 10) AS n, sum(e.amount * k.rate) AS s FROM events e JOIN kinds k ON k.id = e.kind_id GROUP BY 1'),
	(27, 'a sum of a column and a count',   'SELECT t.region, count(*) AS n, sum(e.amount) AS a, sum(e.loose) AS l, sum(e.amount * k.rate) AS s FROM events e JOIN tenants t ON t.id = e.tenant_id JOIN kinds k ON k.id = e.kind_id GROUP BY 1');
-- the plan of each statement: the sums of the factors in the scan (the lookup joined above it), the products in the scan, or the normal plan
CREATE FUNCTION eager_plan(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; scans int := 0; product int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		scans := scans + (l LIKE '%Custom Scan (SiftAgg%')::int;
		product := product + (l LIKE '%Pushed Aggregates:%' AND l LIKE '% * %')::int;
	END LOOP;
	RETURN CASE WHEN scans = 0 THEN 'normal' WHEN product > 0 THEN 'scan, product in it' ELSE 'scan, sums only' END;
END $$;
CREATE FUNCTION eager_rows(q text, scan bool, workers int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg_with_workers_reset(q, scan, workers) $$;
SELECT n, label, eager_plan(q) AS plan, eager_rows(q, false, 0) <> '' AS has_rows, eager_rows(q, true, 0) = eager_rows(q, false, 0) AS same, eager_rows(q, true, 3) = eager_rows(q, false, 0) AS same_workers FROM eager_q ORDER BY n;
-- the order of the rows of a statement with an ORDER BY, with the scan on and off
SELECT t.region, sum(e.amount * k.rate) AS s FROM events e JOIN tenants t ON t.id = e.tenant_id JOIN kinds k ON k.id = e.kind_id GROUP BY 1 HAVING sum(e.amount * k.rate) > 1000 ORDER BY 2 DESC LIMIT 3;
SET sift.enable_agg = off;
SELECT t.region, sum(e.amount * k.rate) AS s FROM events e JOIN tenants t ON t.id = e.tenant_id JOIN kinds k ON k.id = e.kind_id GROUP BY 1 HAVING sum(e.amount * k.rate) > 1000 ORDER BY 2 DESC LIMIT 3;
RESET sift.enable_agg;

DROP FUNCTION eager_plan(text);
DROP FUNCTION eager_rows(text, bool, int);
DROP TABLE eager_q, events, tenants, kinds, kinds_dup;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s6 CASCADE;
RESET client_min_messages;
-- <<< agg_eager_lookup

DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
