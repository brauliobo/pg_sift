-- The arms of a two-level aggregate joined to a spine (a VALUES list, a series or a small table of buckets: `spine JOIN arms ON key equalities`, or
-- `spine LEFT JOIN arms`, which keeps the buckets no arm row has): each cell is added to the group of each spine row its keys equal, and the
-- columns of the spine are group keys. Every query runs as the two-level scan, as the arms alone (sift.agg_two_level off) and as the normal
-- plan; the answers must be the same.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
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
FROM generate_series(1, 12000) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, score float8}');
CREATE TABLE buckets (kind text, ts date, label text) WITH (autovacuum_enabled = false);
INSERT INTO buckets VALUES ('k0', '2025-01-01', 'a'), ('k0', '2025-01-02', 'b'), ('k1', '2025-01-01', 'c'), ('k1', '2025-01-01', 'd'),
                           ('k2', '2025-01-04', 'e'), (NULL, '2025-01-03', 'f'), (NULL, NULL, 'g'), ('k9', '2025-01-01', 'h');
CREATE TABLE ticks (a int, b int) WITH (autovacuum_enabled = false);
INSERT INTO ticks SELECT i, i + 1 FROM generate_series(0, 3000) i;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) buckets;
VACUUM (FREEZE, ANALYZE) ticks;

-- the arms: per row and category, and per row (a column of the table of rows can be a key of the cells)
CREATE FUNCTION arms() RETURNS text LANGUAGE sql AS $$
	SELECT $q$(SELECT e.id, e.parent_id, e.kind, e.ts, o.cat AS category, avg(o.score) AS score, count(*) AS n
	           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.kind, e.ts, o.cat
	           UNION ALL
	           SELECT e.id, e.parent_id, e.kind, e.ts, NULL::text, avg(o.score), count(*)
	           FROM events e CROSS JOIN LATERAL jsonb_to_recordset(e.doc['parts']) o(cat text, score float8) GROUP BY e.id, e.parent_id, e.kind, e.ts) m$q$
$$;

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
	SET sift.agg_two_level = off;
	EXECUTE format('CREATE TEMP TABLE r_arms AS %s', q);
	RESET sift.agg_two_level;
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
INSERT INTO ticks_big SELECT i, i % 700 FROM generate_series(0, 12000) i;
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
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
