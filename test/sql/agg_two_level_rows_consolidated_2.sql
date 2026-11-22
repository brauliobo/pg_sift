-- The two-level aggregate of src/agg/two and the node that groups rows (SiftAggRows), one segment each, in a schema of its own: arms per row and category, outer FILTERs, NULL arm columns,
-- the spine, GROUPING SETS, a hashed subplan as key, parallel arms and the memory bound of the grouping. Every query runs as the scan and as the normal plan. Replaces fourteen tests.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;

-- >>> agg_two_level_null_filter
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
SET sift.test_create_format1 = on;
-- The arms of a two-level aggregate carry a column that is NULL in every arm (a column of a rollup whose arms have none), and the WHERE of the query that
-- joins them to a spine asks it IS NULL: true for each arm row and for the row of the spine no arm matched, so it filters nothing. PostgreSQL does not see it
-- through the UNION ALL, and the scan took the WHERE for a condition on the arms' rows that the spine join cannot carry. Every query runs as the two-level
-- scan, as the arms alone (sift.agg_two_level off) and as the normal plan; the answers must be the same.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET work_mem = '64MB';

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, ts date, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || i % 5 END, date '2025-01-01' + i % 6,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'score', (i * j) % 10))
                                              FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 200) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, score float8}');
CREATE TABLE buckets (kind text, ts date, label text) WITH (autovacuum_enabled = false);
INSERT INTO buckets VALUES ('k0', '2025-01-01', 'a'), ('k0', '2025-01-02', 'b'), ('k1', '2025-01-01', 'c'), ('k1', '2025-01-01', 'd'),
                           ('k2', '2025-01-04', 'e'), (NULL, '2025-01-03', 'f'), (NULL, NULL, 'g'), ('k9', '2025-01-01', 'h');
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) buckets;

CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

-- the rows of the two-level scan, of the arms' scans and of the normal plan are the same; scan: the plan is the scan's
CREATE FUNCTION same(q text, scan bool DEFAULT true) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	EXECUTE format('CREATE TEMP TABLE r_arms AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM ((TABLE r_arms EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_arms)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_arms, r_off;
	RETURN a = 0 AND b = 0 AND n > 0 AND (NOT scan OR pushed(q));
END $$;

-- the arms: per row and category, and per row, both with a subscore column of NULLs (sub: another constant, or a column of the table)
CREATE FUNCTION arms(sub text DEFAULT 'NULL::text') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$(SELECT e.id, e.parent_id, e.kind, e.ts, o.cat AS category, %1$s AS value_num, avg(o.score) AS score, count(*) AS n
	           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.kind, e.ts, o.cat
	           UNION ALL
	           SELECT e.id, e.parent_id, e.kind, e.ts, NULL::text, %1$s, avg(o.score), count(*)
	           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.kind, e.ts) m$q$, sub)
$$;

CREATE FUNCTION report(jointype text, filter text, sub text DEFAULT 'NULL::text') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH sp(kind, ts, label) AS (SELECT kind, ts, label FROM buckets)
	  SELECT category, sp.label, GROUPING(category, sp.label) AS g, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n,
	         count(*) AS cells, count(DISTINCT id) AS ids, count(*) FILTER (WHERE score >= 5.0::float8) AS high
	  FROM sp %s %s ON m.kind = sp.kind AND m.ts = sp.ts %s
	  GROUP BY GROUPING SETS ((category, sp.label), (category), (sp.label), ()) ORDER BY 1, 2, 3$q$, jointype, arms(sub), filter)
$$;

-- the filter is true of every row: the scan takes the query, as an inner and as a left join of the spine
SELECT j AS jointype, same(report(j, 'WHERE m.value_num IS NULL')) AS scan_and_same FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
-- with a condition of the spine as well
SELECT j AS jointype, same(report(j, 'WHERE m.value_num IS NULL AND sp.label <> $$h$$')) AS scan_and_same FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
-- a subscore that is not NULL in the arms: the filter drops the rows, the scan does not take it, the answers are the same
SELECT j AS jointype, same(report(j, 'WHERE m.value_num IS NULL', $$'x'::text$$), false) AS same_rows_only FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j WHERE j = 'JOIN';
-- a subscore that is a column of the rows (NULL in some): the filter keeps part of the rows, the scan does not take it, the answers are the same
SELECT j AS jointype, same(report(j, 'WHERE m.value_num IS NULL', $$CASE WHEN e.id % 2 = 0 THEN NULL ELSE 'y' END$$), false) AS same_rows_only FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;

-- the same through a sub-select that PostgreSQL pulls up above the join: its column arrives wrapped (a placeholder), as in the reports that name the joined rows first;
-- the inner join pushes the test into the arms, which the scan does not take, and the answers are the same
CREATE FUNCTION wrapped(jointype text) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH sp(kind, ts, label) AS (SELECT kind, ts, label FROM buckets)
	  SELECT category, label, GROUPING(category, label) AS g, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n,
	         count(*) AS cells, count(DISTINCT id) AS ids, count(*) FILTER (WHERE score >= 5.0::float8) AS high
	  FROM (SELECT m.*, sp.label FROM sp %s %s ON m.kind = sp.kind AND m.ts = sp.ts) w WHERE value_num IS NULL
	  GROUP BY GROUPING SETS ((category, label), (category), (label), ()) ORDER BY 1, 2, 3$q$, jointype, arms())
$$;
SELECT j AS jointype, same(wrapped(j), j = 'LEFT JOIN') AS same_and_scan_for_left FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;

DROP FUNCTION wrapped(text), report(text, text, text), arms(text), same(text, bool), pushed(text);
DROP TABLE events, buckets;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< agg_two_level_null_filter

-- >>> agg_two_level_null_key
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
SET sift.test_create_format1 = on;
-- A column the per-row aggregates have as NULL in every arm (a column of a generated series, a bucket the arms do not break down) can be an outer
-- group key: it is NULL in every group. Every query runs as the two-level scan, as the arms alone (sift.agg_two_level off) and as the normal
-- plan; the answers must be the same.
-- the tuples through the arms are in the price of the scan: on these few rows the normal plan is the cheaper one, and the test asks about the scan
SET sift.agg_force = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || i % 5 END,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'score', (i * j) % 10))
                                              FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 12000) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, doc) WITH (paths = 'doc.parts[]{cat text, score float8}');
VACUUM (FREEZE, ANALYZE) events;

-- the arms per row and category, and per row, with the columns given as NULL constants; the outer query is the aggregates and the tail
CREATE FUNCTION report(cols text, tail text, keys text DEFAULT 'category, bucket_date') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$SELECT %s, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	  FROM (SELECT e.id, e.parent_id, %s, o.cat AS category, avg(o.score) AS score, count(*) AS n
	        FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, o.cat
	        UNION ALL
	        SELECT e.id, e.parent_id, %s, NULL::text, avg(o.score), count(*)
	        FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id) t
	  GROUP BY %s ORDER BY 1, 2$q$, keys, cols, cols, tail)
$$;

CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	EXECUTE format('CREATE TEMP TABLE r_arms AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM ((TABLE r_arms EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_arms)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_arms, r_off;
	RETURN a = 0 AND b = 0 AND n > 0 AND pushed(q);
END $$;

-- a date NULL in every arm next to a key the arms break down
SELECT same(report('NULL::date AS bucket_date', 'category, bucket_date')) AS date_and_category;
-- only the NULL column: one group
SELECT same(report('NULL::date AS bucket_date', 'bucket_date', 'bucket_date')) AS only_null;
-- two columns of two types, in the middle of the keys
SELECT same(report('NULL::date AS bucket_date, NULL::int AS bucket_no', 'bucket_date, category, bucket_no', 'bucket_date, category, bucket_no')) AS two_nulls;
-- grouping sets over a NULL column: its rows and the totals
SELECT same(report('NULL::date AS bucket_date', 'ROLLUP (category, bucket_date)')) AS rollup;
SELECT same(report('NULL::date AS bucket_date', 'GROUPING SETS ((category, bucket_date), (bucket_date), ())')) AS sets;
-- a HAVING and a test of the NULL column
SELECT same(report('NULL::date AS bucket_date', 'category, bucket_date HAVING bucket_date IS NULL AND count(DISTINCT id) > 100')) AS having_null;

CREATE FUNCTION key_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Group Keys' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;
SELECT * FROM key_lines(report('NULL::date AS bucket_date', 'category, bucket_date'));

-- rows that were updated and deleted since the index was built are read from the heap
UPDATE events SET doc = jsonb_set(doc, '{parts,0,score}', '9') WHERE id % 17 = 0 AND doc IS NOT NULL;
UPDATE events SET parent_id = (parent_id + 1) % 600 WHERE id % 19 = 0;
DELETE FROM events WHERE id % 23 = 0;
SELECT same(report('NULL::date AS bucket_date', 'category, bucket_date')) AS dirty_rows;
SELECT same(report('NULL::date AS bucket_date', 'ROLLUP (category, bucket_date)')) AS dirty_rollup;

-- a hash that does not fit the memory
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT same(report('NULL::date AS bucket_date', 'category, bucket_date')) AS small_memory;
RESET work_mem;
RESET hash_mem_multiplier;

DROP FUNCTION key_lines(text), same(text), pushed(text), report(text, text, text);
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< agg_two_level_null_key

-- >>> agg_two_level_spine
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
SET sift.test_create_format1 = on;
-- The arms of a two-level aggregate joined to a spine (a VALUES list, a series or a small table of buckets: `spine JOIN arms ON key equalities`, or
-- `spine LEFT JOIN arms`, which keeps the buckets no arm row has): each cell is added to the group of each spine row its keys equal, and the
-- columns of the spine are group keys. Every query runs as the two-level scan, as the arms alone (sift.agg_two_level off) and as the normal
-- plan; the answers must be the same.
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
-- the memory of the group tables is not under test here: the scan is priced without a spill
SET work_mem = '64MB';

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, ts date, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || i % 5 END, date '2025-01-01' + i % 6,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'score', (i * j) % 10))
                                              FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 1500) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, score float8}');
CREATE TABLE buckets (kind text, ts date, label text) WITH (autovacuum_enabled = false);
INSERT INTO buckets VALUES ('k0', '2025-01-01', 'a'), ('k0', '2025-01-02', 'b'), ('k1', '2025-01-01', 'c'), ('k1', '2025-01-01', 'd'),
                           ('k2', '2025-01-04', 'e'), (NULL, '2025-01-03', 'f'), (NULL, NULL, 'g'), ('k9', '2025-01-01', 'h');
CREATE TABLE ticks (a int, b int) WITH (autovacuum_enabled = false);
INSERT INTO ticks SELECT i, i + 1 FROM generate_series(0, 375) i;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) buckets;
VACUUM (FREEZE, ANALYZE) ticks;

-- the arms: per row and category, and per row (a column of the table of rows can be a key of the cells)
CREATE FUNCTION arms() RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.agg_events_parts_arms_query() $$;

-- the outer aggregates over the spine (columns kind, ts and label) joined to the arms; count(*) counts the joined rows, a row of a LEFT JOIN no cell matched included
CREATE FUNCTION report(spine text, cond text, keys text DEFAULT 'category, sp.label', jointype text DEFAULT 'JOIN', hint text DEFAULT '') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH sp(kind, ts, label) AS %s (%s)
	  SELECT %s, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, count(*) AS cells, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents,
	         count(*) FILTER (WHERE score >= 5.0::float8) AS high, sum(n) FILTER (WHERE n > 1) AS multi
	  FROM sp %s %s ON %s GROUP BY %s ORDER BY 1, 2$q$, hint, spine, keys, jointype, arms(), cond, keys)
$$;

-- the plan is the scan's: no grouping of PostgreSQL's above it and no node that groups rows
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

-- the rows of the two-level scan, of the arms' scans and of the normal plan are the same; empty: the answer has no row; scan: the plan is the scan's
CREATE FUNCTION same(q text, empty bool DEFAULT false, scan bool DEFAULT true) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	EXECUTE format('CREATE TEMP TABLE r_arms AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM ((TABLE r_arms EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_arms)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_arms, r_off;
	RETURN a = 0 AND b = 0 AND (n > 0) <> empty AND (NOT scan OR pushed(q));
END $$;

-- each check as an inner and as a left join of the spine
CREATE FUNCTION each_join(spine text, cond text, keys text DEFAULT 'category, sp.label', hint text DEFAULT '') RETURNS TABLE (jointype text, ok bool) LANGUAGE sql AS $$
	SELECT j, same(report(spine, cond, keys, j, hint)) FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j
$$;

-- a VALUES list, joined by an equality: a NULL kind of the spine matches no cell, one kind that is twice in it repeats the cells, 'k7' has none
SELECT * FROM each_join($s$VALUES ('k0', NULL::date, 'a'), ('k1', NULL, 'b'), ('k1', NULL, 'c'), (NULL, NULL, 'd'), ('k7', NULL, 'e')$s$, 'm.kind = sp.kind');
-- null-safe: the NULL kind of the spine matches the cells without a kind
SELECT * FROM each_join($s$VALUES ('k0', NULL::date, 'a'), ('k1', NULL, 'b'), ('k1', NULL, 'c'), (NULL, NULL, 'd'), ('k7', NULL, 'e')$s$, 'm.kind IS NOT DISTINCT FROM sp.kind');
-- two columns as the elements of jsonb arrays (null-safe), a table of buckets with duplicates, NULLs and a row nothing matches
SELECT * FROM each_join('SELECT kind, ts, label FROM buckets', 'jsonb_build_array(m.kind, m.ts) = jsonb_build_array(sp.kind, sp.ts)');
SELECT * FROM each_join('TABLE buckets', 'm.kind = sp.kind AND m.ts = sp.ts');
-- the spine column is the only key (a label that two rows have, one that no cell matches), and the key of the arms is not an outer key
SELECT * FROM each_join($s$VALUES ('k0', NULL::date, 'x'), ('k7', NULL, 'x'), ('k1', NULL, 'y'), ('k8', NULL, 'z'), ('k8', NULL, 'z')$s$, 'm.kind = sp.kind', 'sp.label');
-- the arms' column is a key too, next to the spine's
SELECT * FROM each_join('TABLE buckets', 'm.kind IS NOT DISTINCT FROM sp.kind', 'category, m.kind, sp.label');
-- a constant of the spine: only the cells with a NULL timestamp, which there are none of (no group, or the spine's rows alone); or those of one day
SELECT j AS jointype, same(report($s$VALUES ('k0', NULL::date, 'a'), ('k1', NULL, 'b')$s$, 'jsonb_build_array(m.kind, m.ts) = jsonb_build_array(sp.kind, NULL::date)',
                                  'category, sp.label', j), j = 'JOIN') AS ok
FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
SELECT * FROM each_join($s$VALUES ('k0', NULL::date, 'a'), ('k1', NULL, 'b')$s$, 'jsonb_build_array(m.kind, m.ts) = jsonb_build_array(sp.kind, DATE ''2025-01-03'')');
-- a series (the integer parent of the rows, which is NULL for some); 600 and above have no row
SELECT j AS jointype, same(format($q$SELECT s.i, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, count(*) AS cells, count(DISTINCT id) AS ids
                                      FROM generate_series(595, 603) s(i) %s %s ON m.parent_id = s.i GROUP BY s.i ORDER BY 1$q$, j, arms())) AS ok
FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
-- two spine columns of 3,000 values each are more groups than the direct table holds: the groups are hashed
SELECT j AS jointype, same(format($q$SELECT t.a, t.b, round(avg(score)::numeric, 6) AS value, count(*) AS cells, count(DISTINCT id) AS ids
                                      FROM ticks t %s %s ON m.parent_id = t.a GROUP BY t.a, t.b ORDER BY 1, 2$q$, j, arms())) AS ok
FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
-- a spine of 12,001 rows, above the 10,000 it was limited to when each key of the cells scanned it, whose join values repeat (b 600 to 699 match no cell)
CREATE TABLE ticks_big (a int, b int) WITH (autovacuum_enabled = false);
INSERT INTO ticks_big SELECT i, i % 700 FROM generate_series(0, 1500) i;
VACUUM (FREEZE, ANALYZE) ticks_big;
SELECT j AS jointype, same(format($q$SELECT t.a, round(avg(score)::numeric, 6) AS value, count(*) AS cells, count(DISTINCT id) AS ids
                                      FROM ticks_big t %s %s ON m.parent_id = t.b GROUP BY t.a ORDER BY 1$q$, j, arms())) AS ok
FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
-- a spine whose rows are above the memory the lookups take keeps the normal plan, with the same answer
SET sift.agg_lookup_max_kb = 512;
SELECT same(q, scan => false) AND NOT pushed(q) AS over_budget
FROM (SELECT format($q$SELECT t.a, round(avg(score)::numeric, 6) AS value FROM ticks_big t JOIN %s ON m.parent_id = t.b GROUP BY t.a ORDER BY 1$q$, arms()) AS q) f;
RESET sift.agg_lookup_max_kb;
-- a spine without rows: no group
SELECT same(report($s$SELECT * FROM buckets WHERE label > 'zzz'$s$, 'm.kind = sp.kind', 'category, sp.label', j), true) AS empty_spine FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
-- a spine that no cell matches: no group, or one per spine row, of NULL arm columns
SELECT j AS jointype, same(report($s$VALUES ('nothing', NULL::date, 'x'), ('none', NULL, 'y')$s$, 'm.kind = sp.kind', 'category, sp.label', j), j = 'JOIN') AS ok
FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
-- the spine is read twice by the query: a common table expression that PostgreSQL keeps
SELECT * FROM each_join('TABLE buckets', 'm.kind = sp.kind', hint => 'MATERIALIZED');

-- the plan shows the spine's rows as a child of the scan
CREATE FUNCTION plan_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '(Group Keys|Cell Keys|Seq Scan|Values Scan)' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;
SELECT * FROM plan_lines(report('TABLE buckets', 'm.kind IS NOT DISTINCT FROM sp.kind', 'category, sp.label', 'LEFT JOIN'));

-- shapes that keep the normal plan, with the same answer: a VALUES list of one row (constants in the query), a volatile condition of the spine,
-- the arms on the preserved side, a condition of the join that is no equality, one that is an OR, an expression, a column of another type
SELECT same(report($s$VALUES ('k2', NULL::date, 'only')$s$, 'm.kind = sp.kind'), scan => false) AS one_row;
SELECT same(report($s$SELECT * FROM buckets WHERE random() < 2$s$, 'm.kind = sp.kind'), scan => false) AS volatile_spine;
SELECT same(report('TABLE buckets', 'm.kind = sp.kind', jointype => 'RIGHT JOIN'), scan => false) AS arms_preserved;
SELECT same(report('TABLE buckets', 'm.kind = sp.kind AND m.n > 1', jointype => 'LEFT JOIN'), scan => false) AS other_condition;
SELECT same(report('TABLE buckets', 'm.kind = sp.kind OR m.ts = sp.ts'), scan => false) AS or_condition;
SELECT same(report('TABLE buckets', 'm.ts + 1 = sp.ts + 1'), scan => false) AS expression_key;
SELECT same(report('SELECT kind::varchar, ts, label FROM buckets', 'm.kind = sp.kind'), scan => false) AS other_type;

-- rows that were updated and deleted since the index was built are read from the heap
UPDATE events SET doc = jsonb_set(doc, '{parts,0,score}', '9') WHERE id % 17 = 0 AND doc IS NOT NULL;
UPDATE events SET kind = 'k' || (id + 1) % 5, ts = ts + 1, parent_id = (parent_id + 1) % 600 WHERE id % 19 = 0;
DELETE FROM events WHERE id % 23 = 0;
SELECT * FROM each_join('TABLE buckets', 'jsonb_build_array(m.kind, m.ts) = jsonb_build_array(sp.kind, sp.ts)');
SELECT * FROM each_join('TABLE buckets', 'm.kind IS NOT DISTINCT FROM sp.kind');
-- a bucket that had no cell has some now
UPDATE buckets SET kind = 'k3' WHERE label = 'h';
SELECT * FROM each_join('TABLE buckets', 'm.kind = sp.kind');

-- a hash that does not fit the memory
SET work_mem = '256kB';
SET hash_mem_multiplier = 1;
SELECT * FROM each_join('TABLE buckets', 'm.kind IS NOT DISTINCT FROM sp.kind');
RESET work_mem;
RESET hash_mem_multiplier;

-- parallel workers (the two-level scan has none: the arms' scans may)
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT j AS jointype, same(report('TABLE buckets', 'm.kind IS NOT DISTINCT FROM sp.kind', 'category, sp.label', j), scan => false) AS ok FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;

DROP FUNCTION plan_lines(text), each_join(text, text, text, text), same(text, bool, bool), pushed(text), report(text, text, text, text, text), arms();
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< agg_two_level_spine

-- >>> agg_two_level_spine_sets
CREATE SCHEMA s4;
GRANT USAGE ON SCHEMA s4 TO PUBLIC;
SET search_path = s4, public;
SET sift.test_create_format1 = on;
-- The arms of a two-level aggregate joined to a spine, grouped by grouping sets (GROUPING SETS, ROLLUP, CUBE) over the keys of the arms and the
-- spine's columns: each cell is added to the group of each spine row its keys equal, in every set, and a row of a LEFT JOIN spine no cell
-- matched is a group of NULL arm columns in every set. Every query runs as the two-level scan, as the arms alone (sift.agg_two_level off) and as
-- the normal plan; the answers must be the same.
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET work_mem = '64MB';

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, ts date, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || i % 5 END, date '2025-01-01' + i % 6,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'score', (i * j) % 10))
                                              FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 375) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, score float8}');
CREATE TABLE buckets (kind text, ts date, label text) WITH (autovacuum_enabled = false);
INSERT INTO buckets VALUES ('k0', '2025-01-01', 'a'), ('k0', '2025-01-02', 'b'), ('k1', '2025-01-01', 'c'), ('k1', '2025-01-01', 'd'),
                           ('k2', '2025-01-04', 'e'), (NULL, '2025-01-03', 'f'), (NULL, NULL, 'g'), ('k9', '2025-01-01', 'h');
CREATE TABLE ticks (a int, b int) WITH (autovacuum_enabled = false);
INSERT INTO ticks SELECT i, i + 1 FROM generate_series(0, 200) i;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) buckets;
VACUUM (FREEZE, ANALYZE) ticks;

-- the plan is the scan's: no grouping of PostgreSQL's above it and no node that groups rows
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

-- the rows of the two-level scan, of the arms' scans and of the normal plan are the same; empty: the answer has no row; scan: the plan is the scan's
CREATE FUNCTION same(q text, empty bool DEFAULT false, scan bool DEFAULT true) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	EXECUTE format('CREATE TEMP TABLE r_arms AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM ((TABLE r_arms EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_arms)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_arms, r_off;
	RETURN a = 0 AND b = 0 AND (n > 0) <> empty AND (NOT scan OR pushed(q));
END $$;

-- the arms: per row and category, and per row
CREATE FUNCTION arms() RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.agg_events_parts_arms_query() $$;

-- the outer aggregates by grouping sets of the columns cols (category, the kind of the arms, the label of the spine); g tells the sets apart
CREATE FUNCTION report(spine text, cond text, sets text, jointype text DEFAULT 'JOIN', cols text DEFAULT 'category, sp.label') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH sp(kind, ts, label) AS (%s)
	  SELECT %s, GROUPING(%s) AS g, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n,
	         count(*) AS cells, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents, count(*) FILTER (WHERE score >= 5.0::float8) AS high
	  FROM sp %s %s ON %s GROUP BY %s ORDER BY 1, 2$q$, spine, cols, cols, jointype, arms(), cond, sets)
$$;

-- each check as an inner and as a left join of the spine
CREATE FUNCTION each_join(spine text, cond text, sets text, cols text DEFAULT 'category, sp.label') RETURNS TABLE (jointype text, ok bool) LANGUAGE sql AS $$
	SELECT j, same(report(spine, cond, sets, j, cols)) FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j
$$;

-- a VALUES list with a NULL kind (null-safe: it matches the cells without a kind), a kind twice, a kind no cell has
SELECT * FROM each_join($s$VALUES ('k0', NULL::date, 'a'), ('k1', NULL, 'b'), ('k1', NULL, 'c'), (NULL, NULL, 'd'), ('k7', NULL, 'e')$s$,
                        'm.kind IS NOT DISTINCT FROM sp.kind', 'GROUPING SETS ((category, sp.label), (sp.label), (category), ())');
SELECT * FROM each_join($s$VALUES ('k0', NULL::date, 'a'), ('k1', NULL, 'b'), ('k1', NULL, 'c'), (NULL, NULL, 'd'), ('k7', NULL, 'e')$s$,
                        'm.kind = sp.kind', 'ROLLUP (category, sp.label)');
SELECT * FROM each_join($s$VALUES ('k0', NULL::date, 'a'), ('k1', NULL, 'b'), ('k7', NULL, 'e')$s$,
                        'm.kind = sp.kind', 'CUBE (category, sp.label)');
-- the keys of the arms next to the spine's, and a set that leaves one of them out (the shape of a report with and without a drill-down)
SELECT * FROM each_join('TABLE buckets', 'm.kind IS NOT DISTINCT FROM sp.kind',
                        'GROUPING SETS ((category, m.kind, sp.label), (category, sp.label), (m.kind, sp.label))', 'category, m.kind, sp.label');
SELECT * FROM each_join('SELECT kind, ts, label FROM buckets', 'jsonb_build_array(m.kind, m.ts) = jsonb_build_array(sp.kind, sp.ts)',
                        'GROUPING SETS ((category, m.kind, sp.label), (category, sp.label), (m.kind, sp.label))', 'category, m.kind, sp.label');
-- no cell matches: the groups of a LEFT JOIN are the spine rows in each set, and the empty set is a row of its own either way
SELECT j AS jointype, same(report($s$VALUES ('nothing', NULL::date, 'x'), ('none', NULL, 'y')$s$, 'm.kind = sp.kind',
                                  'GROUPING SETS ((category, sp.label), (sp.label), ())', j)) AS ok
FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
-- a spine of 3,000 rows with two columns: the groups of each set are hashed
SELECT j AS jointype, same(format($q$SELECT t.a, t.b, category, GROUPING(t.a, t.b, category) AS g, round(avg(score)::numeric, 6) AS value, count(*) AS cells, count(DISTINCT id) AS ids
                                      FROM ticks t %s %s ON m.parent_id = t.a GROUP BY GROUPING SETS ((t.a, t.b), (t.a, category), (category)) ORDER BY 1, 2, 3, 4$q$, j, arms())) AS ok
FROM unnest(ARRAY['JOIN', 'LEFT JOIN']) j;
-- the plan is one scan with the sets
CREATE FUNCTION plan_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '(Group Keys|Cell Keys|Seq Scan|Values Scan|Grouping)' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;
SELECT * FROM plan_lines(report('TABLE buckets', 'm.kind IS NOT DISTINCT FROM sp.kind', 'GROUPING SETS ((category, sp.label), (sp.label))', 'LEFT JOIN'));

-- rows that were updated and deleted since the index was built are read from the heap
UPDATE events SET doc = jsonb_set(doc, '{parts,0,score}', '9') WHERE id % 17 = 0 AND doc IS NOT NULL;
UPDATE events SET kind = 'k' || (id + 1) % 5, ts = ts + 1, parent_id = (parent_id + 1) % 600 WHERE id % 19 = 0;
DELETE FROM events WHERE id % 23 = 0;
SELECT * FROM each_join('TABLE buckets', 'm.kind IS NOT DISTINCT FROM sp.kind', 'GROUPING SETS ((category, m.kind, sp.label), (category, sp.label), (m.kind, sp.label))', 'category, m.kind, sp.label');
DROP TABLE buckets, events, ticks;
DROP FUNCTION arms(), each_join(text,text,text,text), plan_lines(text), pushed(text), report(text,text,text,text,text), same(text,boolean,boolean);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s4 CASCADE;
RESET client_min_messages;
-- <<< agg_two_level_spine_sets

-- >>> agg_two_level_subplan_key
CREATE SCHEMA s5;
GRANT USAGE ON SCHEMA s5 TO PUBLIC;
SET search_path = s5, public;
SET sift.test_create_format1 = on;
-- A group key that holds a hashed sub-select (CASE WHEN kind IN (SELECT id FROM kinds) THEN .. END) is one every arm of the two-level scan groups by: each arm plans
-- a sub-plan of its own for it, the same test of the same column, so the scan compiles the key once and asks the sub-select once. Every query runs as the two-level
-- scan, as the arms alone (sift.agg_two_level off) and as the normal plan; the answers must be the same: NULL kinds, kinds the lookup lacks, ids the lookup holds
-- twice, a table whose visibility map is empty. Arms whose sub-selects differ, or are correlated, keep the plain refusal.
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE kinds (id text, label text);
INSERT INTO kinds SELECT 'k' || i, 'label ' || i FROM generate_series(0, 3) i;
INSERT INTO kinds VALUES ('k1', 'again'), ('k2', 'again');		-- an id the lookup holds twice; k4 is not in it
ANALYZE kinds;

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'k' || i % 5 END,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'score', (i * j) % 10))
                                              FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 12000) i;
-- the same rows in a table that is never vacuumed: its visibility map is empty
CREATE TABLE events_nv (LIKE events INCLUDING ALL) WITH (autovacuum_enabled = false);
INSERT INTO events_nv SELECT * FROM events;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, doc) WITH (paths = 'doc.parts[]{cat text, score float8}');
CREATE INDEX events_nv_col ON events_nv USING sift_col (id, parent_id, kind, doc) WITH (paths = 'doc.parts[]{cat text, score float8}');
VACUUM (FREEZE, ANALYZE) events;
ANALYZE events_nv;

-- the arms per row and category, and per row; dim1 and dim2 are the key of each arm, tail is the GROUP BY and what follows
CREATE FUNCTION report(tbl text, dim1 text, dim2 text, tail text DEFAULT 'category, dimension', cols text DEFAULT 'category, dimension') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$SELECT %s, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	  FROM (SELECT e.id, e.parent_id, %s AS dimension, o.cat AS category, avg(o.score) AS score, count(*) AS n
	        FROM %I e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, 3, o.cat
	        UNION ALL
	        SELECT e.id, e.parent_id, %s, NULL::text, avg(o.score), count(*)
	        FROM %I e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, 3) t
	  GROUP BY %s ORDER BY 1, 2$q$, cols, dim1, tbl, dim2, tbl, tail)
$$;

CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

-- whether the scan answered and the rows it differs by from the normal plan and from the arms alone
CREATE FUNCTION check_same(q text) RETURNS TABLE (pushed bool, rows bigint, differences bigint) LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	EXECUTE format('CREATE TEMP TABLE r_arms AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_on)) d;
	SELECT count(*) INTO b FROM ((TABLE r_arms EXCEPT ALL TABLE r_off) UNION ALL (TABLE r_off EXCEPT ALL TABLE r_arms)) d;
	SELECT count(*) INTO rows FROM r_off;
	DROP TABLE r_on, r_arms, r_off;
	pushed := pushed(q);
	differences := a + b;
	RETURN NEXT;
END $$;

CREATE FUNCTION why(q text) RETURNS TABLE (arm text, used bool, slug text) LANGUAGE sql AS $$
	SELECT arm, used, slug FROM sift.explain_why(q) ORDER BY arm
$$;

\set dim 'CASE WHEN e.kind IN (SELECT id FROM kinds) THEN jsonb_build_object(''id'', e.kind, ''type'', ''kind'') ELSE NULL END'

-- the dimension and the category; NULL kinds, k4 (not in the lookup) and the ids it holds twice are groups like the others
SELECT * FROM check_same(report('events', :'dim', :'dim'));
-- only the dimension, grouping sets over it, and HAVING on it
SELECT * FROM check_same(report('events', :'dim', :'dim', 'dimension', 'dimension'));
SELECT * FROM check_same(report('events', :'dim', :'dim', 'ROLLUP (category, dimension)'));
SELECT * FROM check_same(report('events', :'dim', :'dim', 'category, dimension HAVING count(DISTINCT id) > 1500'));
-- NOT IN: a NULL kind passes neither test
SELECT * FROM check_same(report('events', 'CASE WHEN e.kind NOT IN (SELECT id FROM kinds) THEN e.kind ELSE ''known'' END', 'CASE WHEN e.kind NOT IN (SELECT id FROM kinds) THEN e.kind ELSE ''known'' END'));
-- the empty map
SELECT * FROM check_same(report('events_nv', :'dim', :'dim'));
SELECT * FROM check_same(report('events_nv', :'dim', :'dim', 'ROLLUP (category, dimension)'));

-- a lookup that changes between the statements is read when the scan starts: the answer follows the table
DELETE FROM kinds WHERE id = 'k3';
INSERT INTO kinds VALUES ('k4', 'new');
SELECT * FROM check_same(report('events', :'dim', :'dim'));

-- keep the plain refusal: the arms test different sub-selects, a sub-select that reads a column of the row
SELECT * FROM check_same(report('events', :'dim', 'CASE WHEN e.kind IN (SELECT id FROM kinds WHERE label <> ''again'') THEN jsonb_build_object(''id'', e.kind, ''type'', ''kind'') ELSE NULL END'));
SELECT * FROM check_same(report('events', 'CASE WHEN EXISTS (SELECT 1 FROM kinds k WHERE k.id = e.kind AND k.label <> e.kind) THEN e.kind END',
                                          'CASE WHEN EXISTS (SELECT 1 FROM kinds k WHERE k.id = e.kind AND k.label <> e.kind) THEN e.kind END'));
SELECT * FROM why(report('events', :'dim', 'CASE WHEN e.kind IN (SELECT id FROM kinds WHERE label <> ''again'') THEN jsonb_build_object(''id'', e.kind, ''type'', ''kind'') ELSE NULL END'));
SELECT * FROM why(report('events', 'CASE WHEN EXISTS (SELECT 1 FROM kinds k WHERE k.id = e.kind AND k.label <> e.kind) THEN e.kind END',
                                   'CASE WHEN EXISTS (SELECT 1 FROM kinds k WHERE k.id = e.kind AND k.label <> e.kind) THEN e.kind END'));

DROP TABLE events, events_nv, kinds;
DROP FUNCTION report(text, text, text, text, text), pushed(text), check_same(text), why(text);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s5 CASCADE;
RESET client_min_messages;
-- <<< agg_two_level_subplan_key

-- >>> agg_par_arms
CREATE SCHEMA s6;
GRANT USAGE ON SCHEMA s6 TO PUBLIC;
SET search_path = s6, public;
SET sift.test_create_format1 = on;
-- The outer aggregate over per-row aggregates (the arms of a UNION ALL grouped by a row id) runs with workers: a row is in one item of work, so the
-- cells it makes are flushed into the item's table, and the tables of the items are merged (counts, sums, the distinct rows and the distinct ids of a
-- key the arms group by). Each query is planned with workers and gives the rows of the scan without workers and of the normal plan; a float sum is
-- the same with 0, 2 and 4 workers to the last digit. A child plan that runs per combination of leaves keeps one process, a unique-key LIMIT 1 child
-- (read as a table) does not.
SET client_min_messages = warning;

SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET seq_page_cost = 10000;
SET sift.agg_preagg = off;

CREATE TABLE items (id int PRIMARY KEY, parent_id int, region text, doc jsonb) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO items
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, CASE WHEN i % 17 = 0 THEN NULL ELSE 'r' || i % 9 END,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('k', (array['a', 'b', 'c'])[1 + (i + j) % 3], 'tag', CASE WHEN (i + j) % 7 = 0 THEN NULL ELSE 't' || (i * j) % 5 END,
                                                                                  'n', (i * j) % 10, 'x', ((i * j) % 10) / 4.0 + 0.1))
                                              FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 7000) i;
-- a row with forty cells in a group, and one that spans several chunks of tuples
INSERT INTO items
SELECT 9000 + i, i, 'wide', jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('k', (array['a', 'b'])[1 + j % 2], 'tag', 'w' || j, 'n', j % 10, 'x', (j % 10) / 4.0 + 0.1))
                                                         FROM generate_series(1, CASE WHEN i = 1 THEN 5000 ELSE 40 END) j))
FROM generate_series(1, 4) i;
CREATE INDEX items_col ON items USING sift_col (id, parent_id, region, doc) WITH (paths = 'doc.parts[]{k text, tag text, n int4, x float8}');
CREATE TABLE kinds (k text, mk text);
INSERT INTO kinds VALUES ('a', 'm0'), ('b', 'm0'), ('c', 'm1');
CREATE TABLE kinds_u (k text PRIMARY KEY, mk text);
INSERT INTO kinds_u SELECT * FROM kinds;
CREATE TABLE parents (id int PRIMARY KEY, live bool);
INSERT INTO parents SELECT i, i % 5 <> 0 FROM generate_series(0, 599) i;
VACUUM (FREEZE, ANALYZE) items;
ANALYZE kinds;
ANALYZE kinds_u;
ANALYZE parents;

-- the outer aggregates, the arms' extra group column and its HAVING, the WHERE of the scan, a join of the arms, and the outer tail
CREATE FUNCTION report(aggs text, hid text DEFAULT 'parent_id', arm_having text DEFAULT '', tail text DEFAULT '', filt text DEFAULT 'true', j text DEFAULT '') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH base AS (SELECT t.id, t.parent_id, t.region, p.k, p.tag, p.n, p.x
	                                FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, tag text, n int, x float8) %6$s WHERE %4$s)
	                 SELECT k, %1$s FROM (SELECT id, %2$s, k, sum(n) AS s, count(*) AS c, sum(x) AS fs, avg(x) AS fa FROM base GROUP BY id, %2$s, k %3$s
	                                      UNION ALL
	                                      SELECT id, %2$s, NULL::text, sum(n), count(*), sum(x), avg(x) FROM base GROUP BY id, %2$s) q
	                 GROUP BY k %5$s$q$, aggs, hid, arm_having, filt, tail, j)
$$;

CREATE FUNCTION p_rows(q text, push bool, workers int) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg_with_workers(q, push, workers) $$;

CREATE FUNCTION p_mode(q text) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.plan_mode_two_workers(q) $$;

-- did workers scan? (a launch can fail when the other sessions hold the workers: tried again)
CREATE FUNCTION p_launched(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; launched bool;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config('max_parallel_workers_per_gather', '2', true);
	FOR i IN 1..20 LOOP
		launched := false;
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
			launched := launched OR l ~ 'Workers Launched: [1-9]';
		END LOOP;
		IF launched THEN RETURN true; END IF;
		PERFORM pg_sleep(0.2);
	END LOOP;
	RETURN false;
END $$;

-- q is compared with the plain scan and the normal plan after rounding its floats; raw (floats as they are) with the scan without workers and with four
CREATE TABLE p_q (n serial, q text, par bool, raw bool DEFAULT false);
INSERT INTO p_q (q, par) VALUES
	-- a hidden key that is a column of the row: one cell per row and outer group
	(report('sum(s) AS total, sum(c) AS cells, count(*) AS n, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents'), true),
	(report('sum(s) AS total, count(DISTINCT region) AS regions', 'region'), true),
	-- a hidden key that is an element's tag: several cells in a row, NULL tags among them
	(report('sum(s) AS total, sum(c) AS cells, count(*) AS n, count(DISTINCT id) AS ids, count(DISTINCT tag) AS tags', 'tag'), true),
	(report('sum(s) AS total, count(*) AS n, count(DISTINCT parent_id) AS parents, count(DISTINCT tag) AS tags', 'parent_id, tag'), true),
	-- the arms' HAVING and WHERE, the outer HAVING, a filter on the cells
	(report('sum(s) AS total, count(*) AS n, count(DISTINCT tag) AS tags', 'tag', 'HAVING sum(n) > 2'), true),
	(report('sum(s) AS total, count(*) AS n, count(DISTINCT parent_id) AS parents', 'parent_id', '', '', 'n > 3 AND t.parent_id < 400'), true),
	(report('count(DISTINCT parent_id) AS parents, count(*) AS n', 'parent_id', '', 'HAVING count(DISTINCT parent_id) > 100'), true),
	(report('count(DISTINCT parent_id) AS parents, count(*) FILTER (WHERE s > 9) AS big', 'parent_id'), true),
	-- grouping sets over the outer key
	(replace(report('sum(s) AS total, count(*) AS n, count(DISTINCT parent_id) AS parents, count(DISTINCT tag) AS tags', 'parent_id, tag'), 'GROUP BY k', 'GROUP BY ROLLUP (k)'), true),
	-- an arm that has no hidden key next to one that has: its cells are the outer groups
	($q$WITH base AS (SELECT t.id, t.parent_id, p.k, p.n FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int))
	    SELECT k, sum(s) AS total, count(*) AS n, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	    FROM (SELECT id, parent_id, k, sum(n) AS s FROM base GROUP BY id, parent_id, k
	          UNION ALL
	          SELECT id, NULL::int, k, sum(n) FROM base GROUP BY id, k) q GROUP BY k$q$, true),
	-- float sums and averages of the cells
	(report('round(sum(fs)::numeric, 3) AS fsum, round(avg(fa)::numeric, 4) AS favg, count(*) AS n'), true),
	(report('round(avg(fa)::numeric, 4) AS favg, count(DISTINCT parent_id) AS parents', 'parent_id, tag'), true),
	-- a joined table, a lookup of a unique key and one of a key a table holds twice, a sub-select
	(report('sum(s) AS total, count(DISTINCT parent_id) AS parents', 'parent_id', '', '', 'true', 'JOIN parents pa ON pa.id = t.parent_id AND pa.live'), true),
	($q$WITH base AS (SELECT t.id, t.parent_id, kk.mk AS mk, p.k, p.n FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int)
	                  LEFT JOIN LATERAL (SELECT mk FROM kinds_u m WHERE m.k = p.k LIMIT 1) kk ON true)
	    SELECT mk, sum(s) AS total, count(*) AS n, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	    FROM (SELECT id, parent_id, mk, sum(n) AS s FROM base GROUP BY id, parent_id, mk
	          UNION ALL
	          SELECT id, parent_id, NULL::text, sum(n) FROM base GROUP BY id, parent_id) q GROUP BY mk$q$, true),
	($q$WITH base AS (SELECT t.id, t.parent_id, kk.mk AS mk, p.k, p.n FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int)
	                  LEFT JOIN LATERAL (SELECT mk FROM kinds m WHERE m.k = p.k LIMIT 1) kk ON true)
	    SELECT mk, sum(s) AS total, count(*) AS n, count(DISTINCT id) AS ids
	    FROM (SELECT id, parent_id, mk, sum(n) AS s FROM base GROUP BY id, parent_id, mk
	          UNION ALL
	          SELECT id, parent_id, NULL::text, sum(n) FROM base GROUP BY id, parent_id) q GROUP BY mk$q$, true),
	-- the arms joined to a spine of buckets: the leader runs its plan once and the workers read the rows from the shared memory; its unmatched rows are the end of the scan
	($q$WITH base AS (SELECT t.id, t.region, p.k, p.n FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int)),
	         sp(region, label) AS (VALUES ('r1', 'one'), ('r2', 'two'), ('r2', 'deux'), ('r9', 'none'))
	    SELECT sp.label, m.k, sum(m.s) AS total, count(*) AS cells, count(DISTINCT m.id) AS ids
	    FROM sp LEFT JOIN (SELECT id, region, k, sum(n) AS s FROM base GROUP BY id, region, k
	                       UNION ALL
	                       SELECT id, region, NULL::text, sum(n) FROM base GROUP BY id, region) m ON m.region = sp.region GROUP BY sp.label, m.k$q$, true),
	-- floats as they are
	(report('sum(fs) AS fsum, avg(fa) AS favg, count(*) AS n'), true),
	(report('sum(fs) AS fsum, avg(fa) AS favg, count(DISTINCT parent_id) AS parents', 'parent_id, tag', '', 'HAVING count(*) > 1'), true),
	-- a spine and float sums: a worker scans several lanes, and the spine's values get ids in the tables of each
	($q$WITH base AS (SELECT t.id, t.region, p.k, p.x FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, x float8)),
	         sp(region, label) AS (VALUES ('r1', 'one'), ('r2', 'two'), ('r2', 'deux'), ('r9', 'none'))
	    SELECT sp.label, m.k, round(sum(m.s)::numeric, 3) AS total, round(avg(m.s)::numeric, 4) AS mean, count(*) AS cells
	    FROM sp LEFT JOIN (SELECT id, region, k, sum(x) AS s FROM base GROUP BY id, region, k
	                       UNION ALL
	                       SELECT id, region, NULL::text, sum(x) FROM base GROUP BY id, region) m ON m.region = sp.region GROUP BY sp.label, m.k$q$, true);
UPDATE p_q SET raw = true WHERE n IN (SELECT n FROM p_q WHERE q LIKE '%sum(fs) AS fsum, avg(fa)%');

CREATE FUNCTION p_compare(pick int[] DEFAULT NULL, OUT n int, OUT mode text, OUT par_same bool, OUT off_same text) RETURNS SETOF record LANGUAGE plpgsql AS $$
DECLARE q text; x bool; r bool; want text; r0 text; r2 text;
BEGIN
	FOR n, q, x, r IN SELECT p_q.n, p_q.q, p_q.par, p_q.raw FROM p_q WHERE pick IS NULL OR p_q.n = ANY (pick) ORDER BY p_q.n LOOP
		mode := p_mode(q);
		want := CASE WHEN x THEN 'parallel' ELSE 'serial' END;
		mode := CASE WHEN mode = want THEN 'as expected' ELSE mode || ' (expected ' || want || ')' END;
		r0 := p_rows(q, true, 0);
		r2 := p_rows(q, true, 2);
		par_same := r2 IS NOT DISTINCT FROM r0 AND p_rows(q, true, 4) IS NOT DISTINCT FROM r0;
		off_same := CASE WHEN r THEN 'not compared' ELSE (r2 IS NOT DISTINCT FROM p_rows(q, false, 0))::text END;
		RETURN NEXT;
	END LOOP;
END $$;

SELECT n, mode, par_same, off_same FROM p_compare();

-- the leader does not scan: the tables of the workers are the whole answer
SET parallel_leader_participation = off;
SELECT n, mode, par_same, off_same FROM p_compare(ARRAY[1, 4, 11, 14, 16, 18, 19]);
RESET parallel_leader_participation;

-- the workers scanned
SELECT p_launched(q) AS launched FROM p_q WHERE n IN (1, 3, 9, 14) ORDER BY n;

-- more distinct ids than hash_mem holds
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT n, mode, par_same, off_same FROM p_compare(ARRAY[1, 4]);
RESET work_mem;
RESET hash_mem_multiplier;

-- rows in the write buffer, rows updated and deleted since the index was built (the units that are dirty are read again from the heap)
INSERT INTO items SELECT 20000 + i, i % 600, 'r' || i % 9, jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('k', 'a', 'tag', 't' || j, 'n', (i * j) % 10, 'x', j / 4.0))
                                                                                       FROM generate_series(1, 1 + i % 3) j)) FROM generate_series(1, 500) i;
UPDATE items SET parent_id = parent_id + 1, doc = jsonb_set(doc, '{parts,0,n}', '7') WHERE id % 97 = 0 AND doc IS NOT NULL;
DELETE FROM items WHERE id % 89 = 0;
SELECT n, mode, par_same, off_same FROM p_compare();

-- a prepared statement with a generic plan, run again with other values
PREPARE p_gen (int) AS
	SELECT k, sum(s) AS total, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	FROM (SELECT t.id, t.parent_id, p.k, sum(p.n) AS s FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int)
	      WHERE t.parent_id >= $1 GROUP BY t.id, t.parent_id, p.k
	      UNION ALL
	      SELECT t.id, t.parent_id, NULL::text, sum(p.n) FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int)
	      WHERE t.parent_id >= $1 GROUP BY t.id, t.parent_id) q GROUP BY k ORDER BY 1;
SET plan_cache_mode = force_generic_plan;
SET sift.enable_agg = on;
EXECUTE p_gen (100);
EXECUTE p_gen (400);
EXECUTE p_gen (100);
SET sift.enable_agg = off;
EXECUTE p_gen (100);
EXECUTE p_gen (400);
RESET plan_cache_mode;
SET sift.enable_agg = on;

-- a cursor, and a query that runs again for every row of the outer query
BEGIN;
DECLARE c NO SCROLL CURSOR FOR
	SELECT k, sum(s) AS total, count(DISTINCT parent_id) AS parents
	FROM (SELECT t.id, t.parent_id, p.k, sum(p.n) AS s FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int) GROUP BY t.id, t.parent_id, p.k
	      UNION ALL
	      SELECT t.id, t.parent_id, NULL::text, sum(p.n) FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int) GROUP BY t.id, t.parent_id) q GROUP BY k ORDER BY 1;
FETCH 2 FROM c;
FETCH ALL FROM c;
COMMIT;

CREATE FUNCTION p_each(off bool) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE r record;
BEGIN
	PERFORM set_config('sift.enable_agg', (NOT off)::text, true);
	FOR r IN SELECT g.v,
	           (SELECT sum(s) FROM (SELECT t.id, sum(p.n) AS s FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int)
	                                WHERE t.parent_id = g.v GROUP BY t.id
	                                UNION ALL
	                                SELECT t.id, sum(p.n) FROM items t CROSS JOIN LATERAL jsonb_to_recordset(t.doc -> 'parts') AS p(k text, n int)
	                                WHERE t.parent_id = g.v GROUP BY t.id) q) AS total
	    FROM generate_series(1, 4) g(v) ORDER BY 1 LOOP
		RETURN NEXT r.v || ' ' || coalesce(r.total::text, 'null');
	END LOOP;
END $$;
SELECT * FROM p_each(false);
SELECT * FROM p_each(true);

DROP TABLE items, kinds, kinds_u, parents, p_q;
DROP FUNCTION report(text, text, text, text, text, text), p_rows(text, bool, int), p_mode(text), p_launched(text), p_compare(int[]), p_each(bool);
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s6 CASCADE;
RESET client_min_messages;
-- <<< agg_par_arms

-- >>> agg_rows_group_memory
CREATE SCHEMA s7;
GRANT USAGE ON SCHEMA s7 TO PUBLIC;
SET search_path = s7, public;
SET sift.test_create_format1 = on;
-- The node that groups the rows of a plan (Custom Scan (SiftAggRows)) holds its groups to the end. When the groups it holds pass group_nospill_factor times hash memory, the bytes a scan that cannot spill may take (its estimate
-- of the groups of a join is a guess: 845 for 47,000 here), the statement is run again as PostgreSQL plans it, and a cursor that fetches rows ends with an error that
-- says what to raise. The factor is lowered so that a few thousand groups reach it (group_nospill_base: the 8 MB floor of the hash memory).
SET max_parallel_workers_per_gather = 0;
-- the node under test: a LEFT JOIN of the arms to the spine is a two-level aggregate by default (agg_two_level_spine), which has no rows to group
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, bucket int, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO facts
SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE i % 50 END, i % 2,
       CASE WHEN i % 11 = 0 THEN NULL
            ELSE jsonb_build_object('items', coalesce((SELECT jsonb_agg(jsonb_build_object('k', (array['a', 'b', 'c'])[1 + (i + j) % 3],
                                                                                   'n', CASE WHEN (i + j) % 13 = 0 THEN NULL ELSE (i * j) % 10 END))
                                              FROM generate_series(1, i % 5) j), '[]')) END
FROM generate_series(1, 2500) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, bucket, doc) WITH (paths = 'doc.items[]{k text, n float8}');
VACUUM (FREEZE, ANALYZE) facts;

-- the spine has a range no row belongs to ('c'), and a row without items or without an entity is in no or a NULL group
CREATE FUNCTION report(aggs text, filt text DEFAULT 'true', tail text DEFAULT '') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH periods AS (SELECT * FROM (VALUES ('a', 0), ('b', 1), ('c', 2)) p(label, bucket)),
	                  cells AS (SELECT f.id, f.dim_a_id, f.bucket, i.k, avg(i.n) AS val, count(*) FILTER (WHERE i.n > 5) AS big
	                            FROM facts f CROSS JOIN LATERAL jsonb_to_recordset(f.doc -> 'items') AS i(k text, n float8)
	                            WHERE %s GROUP BY f.id, f.dim_a_id, f.bucket, i.k
	                            UNION ALL
	                            SELECT f.id, f.dim_a_id, f.bucket, NULL::text, avg(i.n), count(*) FILTER (WHERE i.n > 5)
	                            FROM facts f CROSS JOIN LATERAL jsonb_to_recordset(f.doc -> 'items') AS i(k text, n float8)
	                            WHERE %s GROUP BY f.id, f.dim_a_id, f.bucket)
	               SELECT c.k, p.label, %s
	               FROM periods p LEFT JOIN cells c ON c.bucket = p.bucket GROUP BY c.k, p.label %s$q$, filt, filt, aggs, tail)
$$;

CREATE FUNCTION plan_has(q text, node text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%' || node || '%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;


-- a group for each id and key: tens of thousands of groups
CREATE FUNCTION many_groups() RETURNS text LANGUAGE sql AS $$
	SELECT report('count(DISTINCT c.dim_a_id) AS dim_a, count(*) AS n, count(c.id) AS ids', 'true', ', c.id')
$$;

SET sift.agg_rows = off;
DO $$ BEGIN EXECUTE format('CREATE TEMP TABLE want AS %s', many_groups()); END $$;
RESET sift.agg_rows;

-- the plan is made at the default factor and has the node; it is run under 1.6 MB (the tables of the arms, 20,000 groups, take 1.2 MB; the 47,000 groups of the node 2 MB)
SET plan_cache_mode = force_generic_plan;
SELECT format('PREPARE r AS %s', many_groups()) AS prepare_r \gexec
SELECT plan_has('EXECUTE r', 'SiftAggRows') AS node_at_default_factor;
CREATE TEMP TABLE got AS EXECUTE r;
SELECT count(*) AS groups, (SELECT count(*) FROM (TABLE want EXCEPT TABLE got) d) AS differ FROM got;
DROP TABLE got;

SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET sift.test_limits = 'group_nospill_factor=1,group_nospill_base=1638400';
-- the statement is run again as PostgreSQL plans it
CREATE TEMP TABLE got AS EXECUTE r;
SELECT count(*) AS groups, (SELECT count(*) FROM (TABLE want EXCEPT TABLE got) d) AS differ FROM got;

-- the node spills its groups: a cursor is no different
DO $$
DECLARE n bigint;
BEGIN
	EXECUTE 'DECLARE c CURSOR FOR ' || many_groups();
	EXECUTE 'FETCH 10 FROM c';
	GET DIAGNOSTICS n = ROW_COUNT;
	RAISE WARNING 'fetched % rows', n;
END $$;

-- the groups are counted in 32 bits and the arrays double in 32 bits (docs/LIMITS.md, rows_groups): a node at the bound ends as one past hash memory does
DROP TABLE got;
RESET work_mem;
RESET hash_mem_multiplier;
SET client_min_messages = notice;
SET sift.test_limits = 'rows_groups=5000';
CREATE TEMP TABLE got AS EXECUTE r;
SELECT count(*) AS groups, (SELECT count(*) FROM (TABLE want EXCEPT TABLE got) d) AS differ FROM got;
DROP TABLE got;
SET sift.test_limits = 'rows_groups=100000';
SELECT plan_has('EXECUTE r', 'SiftAggRows') AS node_below_the_bound;
CREATE TEMP TABLE got AS EXECUTE r;
SELECT count(*) AS groups, (SELECT count(*) FROM (TABLE want EXCEPT TABLE got) d) AS differ FROM got;
DROP TABLE got;
RESET client_min_messages;

DEALLOCATE r;
RESET plan_cache_mode;
RESET sift.test_limits;
RESET work_mem;
RESET hash_mem_multiplier;

DROP FUNCTION many_groups();
DROP FUNCTION plan_has(text, text);
DROP FUNCTION report(text, text, text);
DROP TABLE facts;
RESET ALL;
SET client_min_messages = warning;
DROP SCHEMA s7 CASCADE;
RESET client_min_messages;
-- <<< agg_rows_group_memory

DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
