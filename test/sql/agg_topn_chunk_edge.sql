-- The tuples of a chunk whose first sort key comes after that of the last kept tuple of their group are dropped on that key alone (agg/gen_run.c): a tuple that ties with the edge on the
-- first key, a NULL or a NaN key (NULLS FIRST and LAST, ascending and descending), a second key that breaks the tie and a group that appears late give the rows of sift off.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE tc (id int PRIMARY KEY, g int NOT NULL, x float8, y int, z int, d date);
INSERT INTO tc SELECT n, CASE WHEN n < 20000 THEN 1 + n % 7 ELSE 1 + n % 40 END,
  CASE WHEN n % 13 = 0 THEN NULL WHEN n % 101 = 0 THEN 'NaN'::float8 ELSE ((n * 37) % 50) / 4.0 END, CASE WHEN n % 17 = 0 THEN NULL ELSE (n * 11) % 9 END, (n * 5) % 6, date '2024-01-01' + (n * 3) % 20
  FROM generate_series(1, 40000) n;
CREATE INDEX tc_col ON tc USING sift_col (id, g, x, y, z, d);
VACUUM (FREEZE, ANALYZE) tc;

CREATE TABLE q (ord serial, name text, sql text);
INSERT INTO q (name, sql) VALUES
  ('float desc, id',            $q$SELECT g, (array_agg(id ORDER BY x DESC, id))[1:4] FROM tc GROUP BY g$q$),
  ('float desc nulls last, id', $q$SELECT g, (array_agg(id ORDER BY x DESC NULLS LAST, id))[1:4] FROM tc GROUP BY g$q$),
  ('float asc nulls first, id', $q$SELECT g, (array_agg(id ORDER BY x NULLS FIRST, id))[1:4] FROM tc GROUP BY g$q$),
  ('float asc, id',             $q$SELECT g, (array_agg(id ORDER BY x, id))[1:4] FROM tc GROUP BY g$q$),
  ('one key, ties',             $q$SELECT g, (array_agg(y ORDER BY y DESC NULLS LAST))[1:3] FROM tc GROUP BY g$q$),
  ('int key, two keys',         $q$SELECT g, (array_agg(id ORDER BY y DESC NULLS LAST, z, id))[1:5] FROM tc GROUP BY g$q$),
  ('date key',                  $q$SELECT g, (array_agg(id ORDER BY d DESC, id))[1:3] FROM tc GROUP BY g$q$),
  ('rank',                      $q$SELECT * FROM (SELECT g, id, y, rank() OVER (PARTITION BY g ORDER BY y DESC NULLS LAST) r FROM tc) t WHERE r <= 2$q$),
  ('dense_rank',                $q$SELECT * FROM (SELECT g, id, y, dense_rank() OVER (PARTITION BY g ORDER BY y DESC NULLS LAST) r FROM tc) t WHERE r <= 2$q$),
  ('dense_rank, two keys',      $q$SELECT * FROM (SELECT g, id, y, z, dense_rank() OVER (PARTITION BY g ORDER BY y DESC NULLS LAST, z) r FROM tc) t WHERE r <= 3$q$),
  ('row_number, float',         $q$SELECT * FROM (SELECT g, id, x, row_number() OVER (PARTITION BY g ORDER BY x DESC NULLS LAST, id) r FROM tc) t WHERE r <= 3$q$),
  ('filtered',                  $q$SELECT g, (array_agg(id ORDER BY x DESC NULLS LAST, id) FILTER (WHERE id % 3 = 0))[1:4] FROM tc GROUP BY g$q$);
CREATE FUNCTION chunk_check(q text) RETURNS TABLE (engaged bool, same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  engaged := plan LIKE '%Custom Scan (SiftAgg)%';
  PERFORM set_config('sift.enable_agg', 'on', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
  PERFORM set_config('sift.enable_agg', 'on', true);
  same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
SELECT q.name, c.* FROM q, LATERAL chunk_check(q.sql) c ORDER BY q.ord;

DROP FUNCTION chunk_check(text);
DROP TABLE q, tc;
DROP EXTENSION pg_sift;
