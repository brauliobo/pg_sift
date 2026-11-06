-- A statement with no aggregate, group, window, DISTINCT or top-N shape is planned as PostgreSQL plans it: sift.agg_planner has nothing to rewrite or cost for it, and the checks it makes of
-- the statement (the column indexes of its tables, the calls it could rewrite) read a cache or nothing at all. The plan of each of the 20 statements below is the same with sift.enable_agg
-- on and off, and the statements that have a shape of the scan (aggregates, groups, a top-N, DISTINCT ON, a set of keys) still run in it.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE ft (id int PRIMARY KEY, g int NOT NULL, a int, note text NOT NULL, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO ft SELECT i, i % 50, CASE WHEN i % 7 = 0 THEN NULL ELSE i % 1000 END, 'n' || i, jsonb_build_object('k', i % 5) FROM generate_series(1, 20000) i;
CREATE TABLE fd (g int PRIMARY KEY, label text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO fd SELECT i, 'g' || i FROM generate_series(0, 49) i;
CREATE INDEX ft_col ON ft USING sift_col (id, g, a, note, doc) WITH (key_cut = g);
VACUUM (FREEZE, ANALYZE) ft;
VACUUM (FREEZE, ANALYZE) fd;

CREATE TABLE plain_q (ord serial, name text, q text);
INSERT INTO plain_q (name, q) VALUES
  ('point',                 $q$SELECT * FROM ft WHERE id = 5$q$),
  ('range of keys',         $q$SELECT id, g FROM ft WHERE id BETWEEN 100 AND 120$q$),
  ('in list',               $q$SELECT * FROM ft WHERE id IN (1, 2, 3, 400)$q$),
  ('limit without order',   $q$SELECT id FROM ft WHERE g = 3 LIMIT 5$q$),
  ('filter on a column',    $q$SELECT id FROM ft WHERE a > 990$q$),
  ('order by the key',      $q$SELECT id, a FROM ft WHERE g = 4 ORDER BY id$q$),
  ('order by limit btree',  $q$SELECT * FROM ft ORDER BY id LIMIT 10$q$),
  ('join on the key',       $q$SELECT t.id, d.label FROM ft t JOIN fd d ON d.g = t.g WHERE t.id < 50$q$),
  ('left join',             $q$SELECT d.g, t.id FROM fd d LEFT JOIN ft t ON t.g = d.g AND t.id < 10$q$),
  ('exists',                $q$SELECT g FROM fd d WHERE EXISTS (SELECT 1 FROM ft t WHERE t.g = d.g AND t.id = 7)$q$),
  ('in a sub-select',       $q$SELECT id FROM ft WHERE g IN (SELECT g FROM fd WHERE label = 'g3') AND id < 100$q$),
  ('union all',             $q$SELECT id FROM ft WHERE id < 3 UNION ALL SELECT g FROM fd WHERE g < 3$q$),
  ('values',                $q$SELECT t.id FROM (VALUES (1), (2)) v(k) JOIN ft t ON t.id = v.k$q$),
  ('plain cte',             $q$WITH c AS (SELECT id, a FROM ft WHERE id < 20) SELECT * FROM c WHERE a > 3$q$),
  ('for update',            $q$SELECT * FROM ft WHERE id = 5 FOR UPDATE$q$),
  ('function of a column',  $q$SELECT lower(note) FROM ft WHERE id = 5$q$),
  ('scalar sub-select',     $q$SELECT id, (SELECT label FROM fd d WHERE d.g = t.g) FROM ft t WHERE id < 5$q$),
  ('like',                  $q$SELECT id FROM ft WHERE note LIKE 'n19%'$q$),
  ('delete',                $q$DELETE FROM ft WHERE id = 5$q$),
  ('insert select',         $q$INSERT INTO fd SELECT g + 100, label FROM fd WHERE g < 3$q$);

CREATE FUNCTION plan_text(q text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := '';
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  RETURN plan;
END $$;
CREATE FUNCTION plan_same(q text) RETURNS bool LANGUAGE plpgsql AS
$$ DECLARE a text; b text;
BEGIN
  PERFORM set_config('sift.enable_agg', 'on', true);
  a := plan_text(q);
  PERFORM set_config('sift.enable_agg', 'off', true);
  b := plan_text(q);
  PERFORM set_config('sift.enable_agg', 'on', true);
  RETURN a = b AND a NOT LIKE '%SiftAgg%';
END $$;
SELECT name, plan_same(q) AS same_plan FROM plain_q ORDER BY ord;

-- the shapes of the scan are still planned by it
SET sift.agg_force = on;
CREATE TABLE scan_q (ord serial, name text, q text);
INSERT INTO scan_q (name, q) VALUES
  ('count',              $q$SELECT count(*) FROM ft WHERE a > 10$q$),
  ('group',              $q$SELECT g, sum(a) FROM ft GROUP BY g$q$),
  ('count distinct',     $q$SELECT count(DISTINCT g) FROM ft$q$),
  ('top-n',              $q$SELECT id, a FROM ft WHERE a IS NOT NULL ORDER BY a DESC, id LIMIT 5$q$),
  ('distinct on',        $q$SELECT DISTINCT ON (g) g, id FROM ft ORDER BY g, id DESC$q$),
  ('case pivot',         $q$SELECT g, sum(CASE WHEN a > 500 THEN a ELSE 0 END) FROM ft GROUP BY g$q$),
  ('aggregate in a cte', $q$WITH c AS (SELECT g, count(*) n FROM ft GROUP BY g) SELECT * FROM c WHERE n > 1$q$),
  ('aggregate in a sub-link', $q$SELECT d.g FROM fd d WHERE (SELECT count(*) FROM ft t WHERE t.g = d.g) > 399$q$);
CREATE FUNCTION in_scan(q text) RETURNS bool LANGUAGE sql AS $$ SELECT plan_text(q) LIKE '%SiftAgg%' $$;
SELECT name, in_scan(q) AS in_scan FROM scan_q ORDER BY ord;

DROP FUNCTION plan_text(text), plan_same(text), in_scan(text);
DROP TABLE plain_q, scan_q, ft, fd;
DROP EXTENSION pg_sift;
