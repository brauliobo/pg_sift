-- The arms of a two-level aggregate joined to a spine, grouped by grouping sets (GROUPING SETS, ROLLUP, CUBE) over the keys of the arms and the
-- spine's columns: each cell is added to the group of each spine row its keys equal, in every set, and a row of a LEFT JOIN spine no cell
-- matched is a group of NULL arm columns in every set. Every query runs as the two-level scan, as the arms alone (sift.agg_two_level off) and as
-- the normal plan; the answers must be the same.
CREATE EXTENSION pg_sift;
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
FROM generate_series(1, 6000) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, score float8}');
CREATE TABLE buckets (kind text, ts date, label text) WITH (autovacuum_enabled = false);
INSERT INTO buckets VALUES ('k0', '2025-01-01', 'a'), ('k0', '2025-01-02', 'b'), ('k1', '2025-01-01', 'c'), ('k1', '2025-01-01', 'd'),
                           ('k2', '2025-01-04', 'e'), (NULL, '2025-01-03', 'f'), (NULL, NULL, 'g'), ('k9', '2025-01-01', 'h');
CREATE TABLE ticks (a int, b int) WITH (autovacuum_enabled = false);
INSERT INTO ticks SELECT i, i + 1 FROM generate_series(0, 3000) i;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) buckets;
VACUUM (FREEZE, ANALYZE) ticks;

-- the plan is the scan's: no grouping of PostgreSQL's above it and no node that groups rows
CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN scan := true; END IF;
		IF l ~ '(SiftAggRows|GroupAggregate|HashAggregate)' THEN RETURN false; END IF;
	END LOOP;
	RETURN scan;
END $$;

-- the rows of the two-level scan, of the arms' scans and of the normal plan are the same; empty: the answer has no row; scan: the plan is the scan's
CREATE FUNCTION same(q text, empty bool DEFAULT false, scan bool DEFAULT true) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.agg_two_level = off;
	EXECUTE format('CREATE TEMP TABLE r_arms AS %s', q);
	RESET sift.agg_two_level;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM ((TABLE r_arms EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_arms)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_arms, r_off;
	RETURN a = 0 AND b = 0 AND (n > 0) <> empty AND (NOT scan OR pushed(q));
END $$;

-- the arms: per row and category, and per row
CREATE FUNCTION arms() RETURNS text LANGUAGE sql AS $$
	SELECT $q$(SELECT e.id, e.parent_id, e.kind, e.ts, o.cat AS category, avg(o.score) AS score, count(*) AS n
	           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.kind, e.ts, o.cat
	           UNION ALL
	           SELECT e.id, e.parent_id, e.kind, e.ts, NULL::text, avg(o.score), count(*)
	           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.kind, e.ts) m$q$
$$;

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
DROP EXTENSION pg_sift;
