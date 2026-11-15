-- A correlated child (a LEFT JOIN LATERAL .. LIMIT 1 into a mapping table) runs for each distinct input, a probe at a random place of the table's index; once the probes read
-- their heap pages from the OS, the scan asks for the pages of the entries it has found before it fetches them (combo/probe_bt.c dp_prefetch), and for no others: a few keys
-- of a large table ask for a few pages, not for the table. The test makes the scan ask from the first probe (sift.test_child_warm), so that the path runs on a small table
-- in memory: the rows are those of the normal plan, before and after updates and deletes of the table and of the facts.
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
CREATE FUNCTION lookup_line(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    IF l LIKE '%Lookup:%' THEN RETURN l; END IF;
  END LOOP;
  RETURN '';
END $$;
CREATE FUNCTION prefetched() RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce((regexp_match(lookup_line(wq()), '(\d+) child blocks prefetched'))[1]::bigint, 0) $$;
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
-- a few keys of a large lookup table: the blocks asked are those of the entries of the keys probed (one entry for each key of the unique index), not the table's
INSERT INTO tag_map SELECT NULL, 'z', 'tag' || t, 'sub' || s, 'n', 'n' FROM generate_series(0, 2999) t, generate_series(0, 9) s;
VACUUM (ANALYZE) tag_map;
CREATE FUNCTION few_keys() RETURNS text LANGUAGE sql AS $$ SELECT replace(wq(), 'e.dim_b_id <= 50', 'e.dim_b_id <= 50 AND e.id BETWEEN 100 AND 130') $$;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
CREATE TEMP TABLE few AS SELECT lookup_line(few_keys()) AS l;
SELECT (regexp_match(l, '(\d+) child blocks prefetched'))[1]::bigint BETWEEN 1 AND (regexp_match(l, '(\d+) child direct probes'))[1]::bigint AS asked_for_the_keys_probed,
       (regexp_match(l, '(\d+) child blocks prefetched'))[1]::bigint * 4 < (pg_relation_size('tag_map') + pg_relation_size('tag_map_unique')) / 8192 AS not_the_table FROM few;
SELECT * FROM check_query(few_keys());
DROP TABLE few;
DROP FUNCTION few_keys(), wq(), prefetched(), lookup_line(text), check_query(text);
DROP TABLE entries, tag_map, dim_b;
DROP EXTENSION pg_sift;
