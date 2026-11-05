\set ECHO none
-- Shared by the agg_probe_direct tests (\ir test/agg_probe_helpers.sql): the facts the lookups are joined to, probe_check(q) (the rows of a query with the plan of the database, with the
-- child's plan run for every key and with the direct probe of the child's btree, and whether the probe ran), case_query() and step().
CREATE TABLE entries (id serial, dim_b_id int, day date, doc jsonb);
-- items carry a text tag, a second text, a number n (missing in every seventh item, so its key is NULL) and a score
INSERT INTO entries (dim_b_id, day, doc)
SELECT 1 + g % 60, date '2025-01-01' + g % 400,
       jsonb_build_object('items', (SELECT jsonb_agg(jsonb_strip_nulls(jsonb_build_object('tag', 'tag' || (g * j) % 320, 'sub', 'sub' || (g + j) % 9,
                                    'n', CASE WHEN (g + j) % 7 <> 0 THEN (g * j) % 300 END, 'score', (g * j) % 10))) FROM generate_series(1, 1 + g % 5) j))
FROM generate_series(1, 6000) g;
CREATE INDEX entries_col ON entries USING sift_col (id, dim_b_id, day, doc) WITH (paths = 'doc.items[]{tag text, sub text, n float8, score float8}');
\ir agg_check_pushed.sql
\set ECHO none
CREATE FUNCTION probe_check(q text) RETURNS TABLE (pushed bool, direct bool, vs_child bigint, vs_native bigint) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  -- the direct probe goes first: the entries of dead versions are not yet marked dead by a scan of the index (the two other runs mark them)
  SET LOCAL sift.enable_agg = on;
  SET LOCAL sift.agg_probe_direct = on;
  EXECUTE 'CREATE TEMP TABLE r_dir ON COMMIT DROP AS ' || q;
  SET LOCAL sift.agg_probe_direct = off;
  EXECUTE 'CREATE TEMP TABLE r_child ON COMMIT DROP AS ' || q;
  SET LOCAL sift.enable_agg = off;
  EXECUTE 'CREATE TEMP TABLE r_nat ON COMMIT DROP AS ' || q;
  SELECT count(*) INTO vs_child FROM ((TABLE r_child EXCEPT ALL TABLE r_dir) UNION ALL (TABLE r_dir EXCEPT ALL TABLE r_child)) d;
  SELECT count(*) INTO vs_native FROM ((TABLE r_nat EXCEPT ALL TABLE r_dir) UNION ALL (TABLE r_dir EXCEPT ALL TABLE r_nat)) d;
  SET LOCAL sift.enable_agg = on;
  SET LOCAL sift.agg_probe_direct = on;
  pushed := false;
  direct := false;
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
    pushed := pushed OR l LIKE '%Custom Scan (SiftAgg)%';
    direct := direct OR l ~ '[1-9][0-9]* child direct probes';
  END LOOP;
  DROP TABLE r_nat, r_child, r_dir;
  RETURN NEXT;
END $$;
-- the query over a lookup table: the condition on its columns k and v (the key built from an item o), and the end of the lateral subquery
CREATE FUNCTION case_query(tab text, cond text, tail text DEFAULT 'LIMIT 1') RETURNS text LANGUAGE sql AS $$
  SELECT format($q$
 SELECT m.v, count(*) AS n, sum(o.score) AS score
 FROM entries e INNER JOIN LATERAL jsonb_to_recordset(e.doc['items']) AS o(tag text, sub text, n float, score float) ON true
 LEFT JOIN LATERAL (SELECT v FROM %s WHERE %s %s) m ON true
 WHERE e.day >= '2025-03-01' AND e.dim_b_id <= 50
 GROUP BY 1$q$, tab, cond, tail) $$;

CREATE FUNCTION step(label text, tab text, cond text, tail text DEFAULT 'LIMIT 1') RETURNS TABLE (step text, pushed bool, direct bool, vs_child bigint, vs_native bigint) LANGUAGE sql AS
$$ SELECT label, p.* FROM probe_check(case_query(tab, cond, tail)) p $$;
-- the same over any select list: the child is an Index Only Scan where the index holds the columns; ios says whether it is
CREATE FUNCTION case_query_col(tab text, cond text, tail text, col text) RETURNS text LANGUAGE sql AS $$
  SELECT format($q$
 SELECT m.v, count(*) AS n, sum(o.score) AS score
 FROM entries e INNER JOIN LATERAL jsonb_to_recordset(e.doc['items']) AS o(tag text, sub text, n float, score float) ON true
 LEFT JOIN LATERAL (SELECT %s AS v FROM %s WHERE %s %s) m ON true
 WHERE e.day >= '2025-03-01' AND e.dim_b_id <= 50
 GROUP BY 1$q$, col, tab, cond, tail) $$;

CREATE FUNCTION step_io(label text, tab text, cond text, tail text DEFAULT 'LIMIT 1', col text DEFAULT 'v') RETURNS TABLE (step text, pushed bool, direct bool, ios bool, vs_child bigint, vs_native bigint) LANGUAGE plpgsql AS
$$ DECLARE q text := case_query_col(tab, cond, tail, col); l text;
BEGIN
  SELECT p.pushed, p.direct, p.vs_child, p.vs_native INTO pushed, direct, vs_child, vs_native FROM probe_check(q) p;
  step := label;
  ios := false;
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP ios := ios OR l LIKE '%Index Only Scan%'; END LOOP;
  RETURN NEXT;
END $$;
\set ECHO all
