-- The arms of a two-level aggregate that read a lateral LIMIT 1 lookup only through its columns (no CASE around them) have the lookup's NOT NULL test
-- in some arms and not in a third: PostgreSQL turns its outer join into an inner one in the arms that test, and leaves the condition on the other
-- table (a pulled up sub-select's WHERE) at the join in those arms and in the WHERE of the third. The arms are still one scan: its outer join, the
-- NOT NULL test as the filter of the arms that have it. The answers are the normal plan's.
CREATE EXTENSION pg_sift;
-- the tuples through the arms are in the price of the scan: on these few rows the normal plan is the cheaper one, and the test asks about the scan
SET sift.agg_force = on;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE events (id int PRIMARY KEY, kind text, ts date, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, 'k' || i % 5, date '2025-01-01' + i % 400,
       jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'sub', 'sub_' || (i * j) % 3,
                                                                       'score', CASE WHEN (i + j) % 7 = 0 THEN NULL ELSE (i * j) % 10 END)) FROM generate_series(1, 1 + i % 4) j))
FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, sub text, score float8}');
CREATE TABLE lookup (kind text, cat text, sub text, derived_cat text, derived_sub text) WITH (autovacuum_enabled = false);
INSERT INTO lookup SELECT 'k' || k, 'cat_' || c, 'sub_' || s, 'm' || (k + c) % 3, 'ms' || (c + s) % 2
FROM generate_series(0, 4) k, generate_series(0, 5) c, generate_series(0, 2) s WHERE (k + c + s) % 4 <> 0;
CREATE UNIQUE INDEX lookup_key ON lookup (kind, cat, sub);
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) lookup;

-- three arms over the rows of the sub-select that keeps the parts with a score and the lookup; w1, w2, w3 are the WHERE of each
CREATE FUNCTION arms(w1 text DEFAULT 'mo.category IS NOT NULL', w2 text DEFAULT 'mo.category IS NOT NULL', w3 text DEFAULT 'true') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH flag_a AS (
	    SELECT s.id, mo.category, mo.sub, avg(mo.score) AS score, count(s.id) AS n
	    FROM events s INNER JOIN LATERAL (
	      SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	      FROM LATERAL (SELECT cat, sub, score FROM jsonb_to_recordset(s.doc['parts']) p(cat text, sub text, score float8) WHERE p.score IS NOT NULL) o
	      LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat AND m.sub = o.sub LIMIT 1) rm ON true) mo ON true
	    WHERE %s GROUP BY s.id, mo.category, mo.sub
	    UNION ALL
	    SELECT s.id, mo.category, NULL::text, avg(mo.score), count(s.id)
	    FROM events s INNER JOIN LATERAL (
	      SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	      FROM LATERAL (SELECT cat, sub, score FROM jsonb_to_recordset(s.doc['parts']) p(cat text, sub text, score float8) WHERE p.score IS NOT NULL) o
	      LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat AND m.sub = o.sub LIMIT 1) rm ON true) mo ON true
	    WHERE %s GROUP BY s.id, mo.category
	    UNION ALL
	    SELECT s.id, NULL::text, NULL::text, avg(mo.score), count(s.id)
	    FROM events s INNER JOIN LATERAL (
	      SELECT rm.derived_cat AS category, rm.derived_sub AS sub, o.score
	      FROM LATERAL (SELECT cat, sub, score FROM jsonb_to_recordset(s.doc['parts']) p(cat text, sub text, score float8) WHERE p.score IS NOT NULL) o
	      LEFT JOIN LATERAL (SELECT derived_cat, derived_sub FROM lookup m WHERE m.kind = s.kind AND m.cat = o.cat AND m.sub = o.sub LIMIT 1) rm ON true) mo ON true
	    WHERE %s GROUP BY s.id)
	  SELECT coalesce(category, 'all_cats') AS category, sub, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, count(DISTINCT id) AS ids
	  FROM flag_a GROUP BY category, sub ORDER BY 1, 2$q$, w1, w2, w3)
$$;

-- the plan is the two-level scan: the joins in the node, and neither the grouping of PostgreSQL nor the node that groups rows
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN scan := true; END IF;
		IF l ~ '(SiftAggRows|GroupAggregate|HashAggregate)' THEN RETURN false; END IF;
	END LOOP;
	RETURN scan;
END $$;

-- the rows of the scan and of the normal plan are the same; the plan is the scan's
CREATE FUNCTION same(q text, scan bool DEFAULT true) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_off;
	RETURN a = 0 AND n > 0 AND (NOT scan OR pushed(q));
END $$;

-- the lookup's test in the first two arms, none in the third (the sub-select's WHERE is in the inner join's condition of the first two)
SELECT same(arms()) AS inner_in_the_first_two;
-- a condition on the other table's columns besides the test
SELECT same(arms('mo.category IS NOT NULL AND mo.score > 2', 'mo.category IS NOT NULL AND mo.score > 2', 'mo.score > 2')) AS own_and_common;
SELECT same(arms('mo.category IS NOT NULL AND mo.score > 2', 'mo.category IS NOT NULL', 'true')) AS own_in_one;
-- every arm tests
SELECT same(arms('mo.category IS NOT NULL', 'mo.category IS NOT NULL', 'mo.category IS NOT NULL')) AS inner_in_all;
-- a comparison of the lookup's column is a strict test too
SELECT same(arms('mo.category <> ''m0''', 'mo.category <> ''m0''')) AS strict_comparison;

-- rows that were updated and deleted since the index was built are read from the heap; the lookup has NULL values and missing rows
UPDATE events SET doc = jsonb_set(doc, '{parts,0,score}', '9') WHERE id % 17 = 0;
DELETE FROM events WHERE id % 23 = 0;
SELECT same(arms()) AS dirty_rows;
UPDATE lookup SET derived_cat = NULL WHERE cat = 'cat_3';
DELETE FROM lookup WHERE kind = 'k2';
SELECT same(arms()) AS null_and_missing_lookup;
SELECT same(arms('mo.category IS NOT NULL AND mo.score > 2', 'mo.category IS NOT NULL', 'true')) AS dirty_own_condition;

DROP TABLE events, lookup;
DROP FUNCTION arms(text,text,text), pushed(text), same(text,boolean);
DROP EXTENSION pg_sift;
