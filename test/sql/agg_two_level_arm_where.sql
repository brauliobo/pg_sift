-- The arms of a two-level aggregate may differ in their WHERE: a condition that every arm has is the scan's, and one that only some arms have is a
-- filter of those arms for their tuples (a third arm without the NOT NULL test of a lookup counts the rows the others drop). A constant in the GROUP
-- BY of the arms splits no group. Every query runs as the two-level scan, as the arms alone (sift.agg_two_level off) and as the normal plan; the
-- answers must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, ts date, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, 'k' || i % 5, date '2025-01-01' + i % 400,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'dir_a', (array['p', 'n'])[1 + (i + j) % 2],
                                                                                 'score', (i * j) % 10)) FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, dir_a text, score float8}');
CREATE TABLE lookup (kind text, cat text, derived_cat text) WITH (autovacuum_enabled = false);
INSERT INTO lookup SELECT 'k' || k, 'cat_' || c, 'm' || (k + c) % 3 FROM generate_series(0, 4) k, generate_series(0, 5) c WHERE (k + c) % 5 <> 0;
CREATE UNIQUE INDEX lookup_key ON lookup (kind, cat);
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) lookup;

-- three arms over the rows with their spans and the lookup (LIMIT 1); w1, w2, w3 are the WHERE of each, bucket is a constant the arms group by
CREATE FUNCTION report(w1 text DEFAULT 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', w2 text DEFAULT 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', w3 text DEFAULT 'true') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH scoped AS NOT MATERIALIZED (
	    SELECT e.id, e.parent_id, e.doc, e.kind, p.span, NULL::date AS bucket
	    FROM events e JOIN (VALUES ('span_a', daterange('2025-07-01', '2026-01-31', '[]')), ('span_b', daterange('2025-01-01', '2025-06-30', '[]'))) p(span, range)
	         ON e.ts <@ p.range),
	  flag_a AS (
	    SELECT s.id, s.parent_id, s.span, s.bucket, CASE WHEN o.dir_a = 'p' THEN o.cat ELSE rm.derived_cat END AS category, avg(o.score) AS score, count(s.id) AS n
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE %s GROUP BY s.id, s.parent_id, s.span, s.bucket, CASE WHEN o.dir_a = 'p' THEN o.cat ELSE rm.derived_cat END
	    UNION ALL
	    SELECT s.id, s.parent_id, s.span, s.bucket, NULL::text, avg(o.score), count(s.id)
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE %s GROUP BY s.id, s.parent_id, s.span, s.bucket
	    UNION ALL
	    SELECT s.id, s.parent_id, s.span, s.bucket, NULL::text, avg(o.score), count(s.id)
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE %s GROUP BY s.id, s.parent_id, s.span, s.bucket)
	  SELECT coalesce(category, 'all_cats') AS category, span, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n,
	         count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	  FROM flag_a GROUP BY category, span ORDER BY 1, 2$q$, w1, w2, w3)
$$;

-- the same arms under a spine of buckets, one of which no row has: it is a row of NULL columns
CREATE FUNCTION spined(sets text DEFAULT NULL) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH scoped AS NOT MATERIALIZED (SELECT e.id, e.kind, e.doc, e.ts FROM events e),
	  flag_a AS (
	    SELECT s.id, CASE WHEN o.dir_a = 'p' THEN o.cat ELSE rm.derived_cat END AS category, avg(o.score) AS score, count(s.id) AS n
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE CASE WHEN o.dir_a = 'p' THEN o.cat ELSE rm.derived_cat END IS NOT NULL GROUP BY s.id, CASE WHEN o.dir_a = 'p' THEN o.cat ELSE rm.derived_cat END
	    UNION ALL
	    SELECT s.id, NULL::text, avg(o.score), count(s.id)
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	    LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    GROUP BY s.id)
	  SELECT b.label, m.category, count(*) AS cells, round(avg(m.score)::numeric, 6) AS value, sum(m.n)::int AS n, count(DISTINCT m.id) AS ids
	  FROM (VALUES ('m0'), ('m1'), ('cat_2'), ('none')) b(label) LEFT JOIN flag_a m ON m.category = b.label
	  GROUP BY %s ORDER BY 1, 2$q$, coalesce(sets, 'b.label, m.category'))
$$;

-- the plan is the two-level scan: the joins in the node, and neither the grouping of PostgreSQL nor the node that groups rows
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE sql AS $$ SELECT sift_helpers.plan_is_agg_scan(q) $$;

-- the rows of the two-level scan and of the normal plan are the same; empty: the answer has no row; scan: the plan is the scan's
CREATE FUNCTION same(q text, empty bool DEFAULT false, scan bool DEFAULT true) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND (n > 0) <> empty AND (NOT scan OR pushed(q));
END $$;

-- the third arm has no WHERE: its rows count where the lookup has no row
SELECT same(report()) AS third_arm_unfiltered;
-- the first arm is the one without WHERE
SELECT same(report('true', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL')) AS first_arm_unfiltered;
-- a NOT NULL test of the lookup itself makes its outer join an inner one in the arms that have it: the scan keeps the outer join (a row the lookup has no
-- row for is a tuple of NULLs, which the third arm counts) and the other arms test the NOT NULL
SELECT same(report('rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS join_types_differ;
SELECT same(report('true', 'rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS inner_in_the_last_two;
SELECT same(report('rm.derived_cat IS NOT NULL', 'true', 'rm.derived_cat IS NOT NULL')) AS inner_in_the_ends;
SELECT same(report('rm.derived_cat IS NOT NULL AND o.score > 2', 'true', 'true')) AS inner_in_one_with_a_condition;
SELECT same(report('rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS inner_in_all;
-- the number of tuples the lookup has no row for: the third arm counts them, the others do not
SELECT same($q$SELECT count(*) FILTER (WHERE rm.derived_cat IS NULL) AS unmatched, count(*) AS tuples
	FROM events s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, dir_a text, score float8)
	LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true$q$) AS unmatched_exist;
-- every arm has the same WHERE: it is the scan's
SELECT same(report('CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL')) AS all_alike;
SELECT same(report('true', 'true', 'true')) AS none;
-- each arm has a condition of its own besides the common one
SELECT same(report('CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL AND o.score > 2', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'o.dir_a = ''p'' AND CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL')) AS own_conditions;
SELECT same(report('o.score > 2', 'o.score > 2 AND o.dir_a = ''n''', 'o.score > 2')) AS common_and_own;
-- a condition no tuple passes in one arm: that arm has no group
SELECT same(report('o.score > 100', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'true')) AS arm_without_rows;
SELECT same(report('CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'o.score > 100', 'o.score > 100'), scan => false) AS arms_without_rows;

-- a spine of buckets over arms with different WHERE
SELECT same(spined()) AS spine;
-- and by grouping sets: the sets that lack a key null it, the spine rows no cell matched are a group in each
SELECT same(spined('GROUPING SETS ((b.label, m.category), (b.label), (m.category), ())')) AS spine_sets;

-- rows that were updated and deleted since the index was built are read from the heap
UPDATE events SET doc = jsonb_set(doc, '{parts,0,score}', '9') WHERE id % 17 = 0 AND doc IS NOT NULL;
UPDATE events SET ts = ts + 30, kind = 'k' || (id + 1) % 5 WHERE id % 19 = 0;
DELETE FROM events WHERE id % 23 = 0;
SELECT same(report()) AS dirty_rows;
SELECT same(report('CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL AND o.score > 2', 'CASE WHEN o.dir_a = ''p'' THEN o.cat ELSE rm.derived_cat END IS NOT NULL', 'o.dir_a = ''p''')) AS dirty_own_conditions;
UPDATE lookup SET derived_cat = 'm9' WHERE cat = 'cat_1';
SELECT same(report()) AS dirty_lookup;
SELECT same(report('rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS dirty_join_types_differ;
UPDATE lookup SET derived_cat = NULL WHERE cat = 'cat_3';
SELECT same(report('rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS null_lookup_values;
DELETE FROM lookup WHERE kind = 'k2';
SELECT same(report('rm.derived_cat IS NOT NULL', 'rm.derived_cat IS NOT NULL')) AS missing_lookup_rows;

-- a table joined by an outer join in one arm and an inner one in another is not a lateral LIMIT 1 child: they are not arms of one scan
SELECT same($q$WITH flag_a AS (
	    SELECT e.id, l.derived_cat AS category, count(*) AS n FROM events e LEFT JOIN lookup l ON l.kind = e.kind WHERE l.derived_cat IS NOT NULL GROUP BY e.id, l.derived_cat
	    UNION ALL
	    SELECT e.id, NULL::text, count(*) FROM events e LEFT JOIN lookup l ON l.kind = e.kind GROUP BY e.id)
	  SELECT category, sum(n) AS n, count(DISTINCT id) AS ids FROM flag_a GROUP BY category$q$, scan => false) AS joined_table;

-- an arm that does not join the lookup is not an arm of the same scan, and the refusal says which
CREATE FUNCTION unlike() RETURNS text LANGUAGE sql AS $$
	SELECT $q$WITH flag_a AS (
	    SELECT e.id, rm.derived_cat AS category, count(*) AS n
	    FROM events e LEFT JOIN LATERAL (SELECT derived_cat FROM lookup m WHERE m.kind = e.kind LIMIT 1) rm ON true GROUP BY e.id, rm.derived_cat
	    UNION ALL
	    SELECT e.id, NULL::text, count(*) FROM events e GROUP BY e.id)
	  SELECT category, sum(n) AS n, count(DISTINCT id) AS ids FROM flag_a GROUP BY category$q$
$$;
SELECT same(unlike(), scan => false) AS unlike_arms;
SET sift.explain_refusals = on;
SET client_min_messages = notice;
DO $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || unlike() LOOP
		NULL;
	END LOOP;
END $$;
DROP TABLE events, lookup;
DROP FUNCTION pushed(text), report(text,text,text), same(text,boolean,boolean), spined(text), unlike();
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
