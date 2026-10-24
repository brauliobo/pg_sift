-- An aggregate of a table tied to the outer query by equalities (a LATERAL sub-select or a scalar sub-select) is the table grouped by those columns, joined to the
-- outer query: the scan runs the grouping once where the normal plan looks up the group of each outer row. The answers are the normal plan's: an outer row whose
-- key has no row gets NULL (0 for a count), a NULL key has none, an outer row that repeats gets its group each time. A few outer rows keep the lookups.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE key_sets (id int PRIMARY KEY, grp int, k int, k2 int, name text) WITH (autovacuum_enabled = false);
-- k repeats (duplicate outer rows), is NULL for some; ids above 1500 own nothing; k2 is NULL for some
INSERT INTO key_sets SELECT i, i % 7, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 1500 END, CASE WHEN i % 11 = 0 THEN NULL ELSE i % 5 END, 'o' || i FROM generate_series(1, 2000) i;
CREATE TABLE items (id int PRIMARY KEY, set_id int, owner_k2 int, cat int, amt int, txt text, d date) WITH (autovacuum_enabled = false);
INSERT INTO items SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 1200 END, CASE WHEN i % 19 = 0 THEN NULL ELSE i % 5 END, i % 9, CASE WHEN i % 23 = 0 THEN NULL ELSE (i * 7) % 100 END,
       CASE WHEN i % 29 = 0 THEN NULL ELSE 't' || i % 40 END, date '2024-01-01' + i % 300
FROM generate_series(1, 30000) i;
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
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN RETURN 'scan'; END IF;
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
DROP EXTENSION pg_sift;
