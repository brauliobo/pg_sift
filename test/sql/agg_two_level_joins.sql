-- The per-row aggregates of an outer aggregate (UNION ALL arms grouped by a row id) read joined tables: a VALUES list of spans, a table
-- joined by a key, and a LIMIT 1 lookup. The joins are the scan's, not a plan of their own level that the node could not run. Every query
-- runs as the two-level scan, as the arms alone (sift.agg_two_level off) and as the normal plan; the answers must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE items (id int PRIMARY KEY, kind text, enabled bool) WITH (autovacuum_enabled = false);
INSERT INTO items SELECT i, 'k' || i % 5, i % 7 <> 3 FROM generate_series(0, 599) i;
CREATE TABLE events (id int PRIMARY KEY, parent_id int, kind text, ts date, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 600 END, 'k' || i % 5, date '2025-01-01' + i % 400,
       CASE WHEN i % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || (i + j) % 6, 'sign', (array['p', 'n'])[1 + (i + j) % 2],
                                                                                 'score', (i * j) % 10)) FROM generate_series(1, 1 + i % 4) j)) END
FROM generate_series(1, 12000) i;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, kind, ts, doc) WITH (paths = 'doc.parts[]{cat text, sign text, score float8}');
CREATE TABLE lookup (kind text, cat text, mcat text) WITH (autovacuum_enabled = false);
INSERT INTO lookup SELECT 'k' || k, 'cat_' || c, 'm' || (k + c) % 3 FROM generate_series(0, 4) k, generate_series(0, 5) c WHERE (k + c) % 5 <> 0;
CREATE TABLE lookup_empty (kind text, cat text, mcat text);
CREATE TABLE lookup_alt (kind text, cat text, mcat text) WITH (autovacuum_enabled = false);
INSERT INTO lookup_alt SELECT kind, cat, 'x' || mcat FROM lookup WHERE cat <> 'cat_2';
VACUUM (FREEZE, ANALYZE) items;
VACUUM (FREEZE, ANALYZE) events;
VACUUM (FREEZE, ANALYZE) lookup;
ANALYZE lookup_empty;
ANALYZE lookup_alt;

-- the outer aggregate over arms per row and category, and per row; scoped is the rows with their spans and parent, the lookup is LIMIT 1
CREATE FUNCTION report(lk text DEFAULT 'lookup', spans text DEFAULT $p$(VALUES ('span_a', daterange('2025-07-01', '2026-01-31', '[]')),
                       ('span_b', daterange('2025-01-01', '2025-06-30', '[]'))) p(span, range)$p$,
                       parent_join text DEFAULT 'JOIN items pa ON pa.id = e.parent_id AND pa.enabled', lk2 text DEFAULT NULL) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH scoped AS NOT MATERIALIZED (SELECT e.id, e.parent_id, e.doc, e.kind, p.span FROM events e JOIN %s ON e.ts <@ p.range %s),
	  mapped AS (
	    SELECT s.id, s.parent_id, s.span, rm.mcat AS category, avg(o.score) AS score, count(s.id) AS n
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, sign text, score float8)
	    LEFT JOIN LATERAL (SELECT mcat FROM %s m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE rm.mcat IS NOT NULL GROUP BY s.id, s.parent_id, s.span, rm.mcat
	    UNION ALL
	    SELECT s.id, s.parent_id, s.span, NULL::text, avg(o.score), count(s.id)
	    FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, sign text, score float8)
	    LEFT JOIN LATERAL (SELECT mcat FROM %s m WHERE m.kind = s.kind AND m.cat = o.cat LIMIT 1) rm ON true
	    WHERE rm.mcat IS NOT NULL GROUP BY s.id, s.parent_id, s.span)
	  SELECT coalesce(category, 'all_cats') AS category, span, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n,
	         count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	  FROM mapped GROUP BY category, span ORDER BY 1, 2$q$, spans, parent_join, lk, coalesce(lk2, lk))
$$;

-- the spans alone, no lookup: a row in two overlapping spans is two cells of each arm
CREATE FUNCTION overlapping() RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH scoped AS NOT MATERIALIZED (
	    SELECT e.id, e.parent_id, e.doc, p.span
	    FROM events e JOIN (VALUES ('a', daterange('2025-01-01', '2025-09-30', '[]')), ('b', daterange('2025-06-01', '2026-01-31', '[]'))) p(span, range)
	         ON e.ts <@ p.range)
	  SELECT span, category, round(avg(score)::numeric, 6) AS value, sum(n)::int AS n, count(DISTINCT id) AS ids, count(DISTINCT parent_id) AS parents
	  FROM (SELECT s.id, s.parent_id, s.span, o.cat AS category, avg(o.score) AS score, count(*) AS n
	        FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, sign text, score float8) GROUP BY s.id, s.parent_id, s.span, o.cat
	        UNION ALL
	        SELECT s.id, s.parent_id, s.span, NULL::text, avg(o.score), count(*)
	        FROM scoped s CROSS JOIN LATERAL jsonb_to_recordset(s.doc['parts']) o(cat text, sign text, score float8) GROUP BY s.id, s.parent_id, s.span) t
	  GROUP BY span, category ORDER BY 1, 2$q$)
$$;

-- the plan reads the joins in the scan and has neither the grouping of PostgreSQL nor the node that groups rows
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
-- (the two-level scan has no workers: with them the plan may be the arms' scans, whose rows are the same too)
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

-- the spans, the table joined by its key and the lookup
SELECT same(report()) AS spans_parent_lookup;
-- without the table joined by its key: the NULL parents are no value of count(DISTINCT parent_id)
SELECT same(report(parent_join => '')) AS spans_lookup;
-- overlapping spans: the row of both counts in each
SELECT same(overlapping()) AS overlapping_spans;
-- a lookup without a row: no arm row, no group
SELECT same(report('lookup_empty'), true) AS empty_lookup;

-- arms with the same shape whose lookups are other tables are not one scan: each arm has its own rows
SELECT same(report(lk2 => 'lookup_alt'), scan => false) AS different_lookups;

-- the plan shows the joins
CREATE FUNCTION join_lines(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '(Pushed Aggregates|Lookup Joins|Name)' THEN RETURN NEXT trim(l); END IF;
	END LOOP;
END $$;
SELECT * FROM join_lines(report());

-- rows that were updated and deleted since the index was built are read from the heap
UPDATE events SET doc = jsonb_set(doc, '{parts,0,score}', '9') WHERE id % 17 = 0 AND doc IS NOT NULL;
UPDATE events SET ts = ts + 30, parent_id = (parent_id + 1) % 600 WHERE id % 19 = 0;
DELETE FROM events WHERE id % 23 = 0;
SELECT same(report()) AS dirty_rows;
SELECT same(overlapping()) AS dirty_overlapping;
UPDATE lookup SET mcat = 'm9' WHERE cat = 'cat_1';
SELECT same(report()) AS dirty_lookup;

-- parallel workers, and a hash that does not fit the memory
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SELECT same(report()) AS parallel;
SELECT same(overlapping(), scan => false) AS parallel_overlapping;
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT same(report()) AS small_memory;
RESET work_mem;
RESET hash_mem_multiplier;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;

DROP FUNCTION join_lines(text), same(text, bool, bool), pushed(text), overlapping(), report(text, text, text, text);
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift;
