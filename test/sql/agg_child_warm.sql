-- A correlated child (a LEFT JOIN LATERAL .. LIMIT 1 into a mapping table) runs for each distinct input, a probe at a random place of the table's index; when the runs read a
-- good share of a relation from the OS and take long, the scan asks the OS for the rest of it in block order (combo.c child_warm). The test lowers that to a few runs
-- (sift.test_child_warm), so that the path runs on a small table in memory: the rows are those of the normal plan, before and after updates and deletes of the table and
-- of the facts.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 100; SET random_page_cost = 4;
CREATE TABLE dim_b (id int PRIMARY KEY, kind text);
INSERT INTO dim_b SELECT i, CASE WHEN i % 7 = 0 THEN 'b' ELSE 'a' END FROM generate_series(1, 60) i;
-- raw (tag, sub) pairs of a kind and the pair a report shows for them; some unmapped, some mapped to NULL, a few kept for one scope only
CREATE TABLE tag_map (scope int[], kind text, tag text, sub text, to_tag text, to_sub text);
INSERT INTO tag_map SELECT NULL, k, 'tag' || t, 'sub' || s, CASE WHEN (t + s) % 9 = 0 THEN NULL ELSE 'to' || (t % 5) END, 'tosub' || (s % 3)
FROM unnest(ARRAY['a', 'b']) k, generate_series(0, 299) t, generate_series(0, 7) s WHERE (t * 3 + s) % 11 <> 0;
INSERT INTO tag_map SELECT ARRAY[t], 'a', 'tag' || t, 'sub0', 'scoped', 'x' FROM generate_series(0, 299) t;
CREATE UNIQUE INDEX tag_map_unique ON tag_map (COALESCE(scope, '{}'), kind, tag, sub);
CREATE TABLE entries (id serial, dim_b_id int, day date, doc jsonb);
INSERT INTO entries (dim_b_id, day, doc)
SELECT 1 + g % 60, date '2025-01-01' + g % 400,
       CASE WHEN g % 5 = 0 THEN jsonb_build_object('resolved', true, 'items', (SELECT jsonb_agg(jsonb_build_object('tag', 'to' || (g + j) % 5, 'sub', 'tosub' || j % 3, 'score', (g * j) % 10)) FROM generate_series(1, 1 + g % 4) j))
            WHEN g % 53 = 0 THEN NULL
            ELSE jsonb_build_object('items', (SELECT jsonb_agg(jsonb_build_object('tag', 'tag' || (g * j) % 320, 'sub', 'sub' || (g + j) % 9, 'score', (g * j) % 10)) FROM generate_series(1, 1 + g % 5) j)) END
FROM generate_series(1, 6000) g;
CREATE INDEX entries_col ON entries USING sift_col (id, dim_b_id, day, doc, jsonb_exists(doc, 'resolved')) WITH (paths = 'doc.items[]{tag text, sub text, score float8}');
VACUUM (FREEZE, ANALYZE) entries, tag_map, dim_b;
\ir test/agg_check_pushed.sql
CREATE FUNCTION wq() RETURNS text LANGUAGE sql AS $$ SELECT $q$
 SELECT CASE WHEN jsonb_exists(e.doc, 'resolved') THEN o.tag ELSE m.to_tag END AS tag, CASE WHEN jsonb_exists(e.doc, 'resolved') THEN o.sub ELSE m.to_sub END AS sub,
        count(*) AS n, round(avg(o.score)::numeric, 6) AS score
 FROM entries e JOIN dim_b p ON p.id = e.dim_b_id
 INNER JOIN LATERAL jsonb_to_recordset(e.doc['items']) AS o(tag text, sub text, score float) ON true
 LEFT JOIN LATERAL (SELECT to_tag, to_sub FROM tag_map WHERE COALESCE(scope, '{}') = '{}' AND to_tag IS NOT NULL AND kind = p.kind AND tag = o.tag AND sub = o.sub
                    AND NOT jsonb_exists(e.doc, 'resolved') LIMIT 1) m ON true
 WHERE e.day >= '2025-03-01' AND e.day < '2025-11-01' AND e.dim_b_id <= 50
 GROUP BY 1, 2
$q$ $$;
CREATE FUNCTION prefetched() RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE l text; n bigint := 0;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || wq() LOOP
    IF l LIKE '%child blocks prefetched%' THEN n := (regexp_match(l, '(\d+) child blocks prefetched'))[1]::bigint; END IF;
  END LOOP;
  RETURN n;
END $$;
SELECT * FROM check_query(wq());
SELECT prefetched() AS warmed_by_default;
SET sift.test_child_warm = on;
SELECT * FROM check_query(wq());
SELECT prefetched() > 0 AS warmed;
UPDATE tag_map SET to_tag = NULL WHERE kind = 'a' AND tag LIKE 'tag1%' AND sub = 'sub3';
DELETE FROM tag_map WHERE kind = 'b' AND tag LIKE 'tag2%';
INSERT INTO tag_map SELECT NULL, 'b', 'tag2' || t, 'sub' || s, 'late', 'lsub' FROM generate_series(0, 9) t, generate_series(0, 3) s;
UPDATE entries SET doc = jsonb_set(doc, '{items,0,tag}', '"tag7"') WHERE id % 11 = 0 AND doc ? 'items';
DELETE FROM entries WHERE id % 13 = 0;
SELECT * FROM check_query(wq());
SELECT prefetched() > 0 AS warmed_after_changes;
VACUUM tag_map, entries;
SELECT * FROM check_query(wq());
DROP FUNCTION wq(), prefetched(), check_query(text);
DROP TABLE entries, tag_map, dim_b;
DROP EXTENSION pg_sift;
