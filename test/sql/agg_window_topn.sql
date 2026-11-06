-- `SELECT .. FROM (SELECT .., row_number() OVER (PARTITION BY k ORDER BY o) rn FROM t) s WHERE rn <= N` for N > 1 on NOT NULL partition columns of a table whose keys hold
-- many rows is a probe of the table for each key: `(SELECT DISTINCT k ..) g, LATERAL (SELECT .. WHERE k = g.k ORDER BY o LIMIT N)`, the window over the N rows of a key on top
-- (agg/latest.c, topn_lateral), where the plan with the sort of every row is the other choice. A nullable partition column, a table of fewer than 100,000 rows, a key of fewer than 100 and any other shape keep
-- the arrays or the window. Every statement runs with sift on and off and the rows are compared (md5 of the sorted rows; the orders have no ties); `topn` says the plan has
-- a top-N of the key, `in_scan` that the scan ran.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
CREATE TABLE wt (id int PRIMARY KEY, grp int NOT NULL, grp2 int NOT NULL, gn int, amount int, ts int NOT NULL, note text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO wt SELECT i, i % 40, i % 3, CASE WHEN i % 9 = 0 THEN NULL ELSE i % 25 END, CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 37) % 500 END, (i * 101) % 9973, 'n' || i FROM generate_series(1, 120000) i;
CREATE TABLE dim (grp int PRIMARY KEY, label text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, 'g' || i FROM generate_series(0, 39) i;
CREATE TABLE sparse AS SELECT i AS id, i AS grp, (i * 7) % 100 AS amount FROM generate_series(1, 150000) i;
CREATE INDEX wt_col ON wt USING sift_col (id, grp, grp2, gn, amount, ts, note) WITH (key_cut = grp);
CREATE INDEX sparse_col ON sparse USING sift_col (id, grp, amount);
VACUUM (FREEZE, ANALYZE) wt;
VACUUM (FREEZE, ANALYZE) dim;
VACUUM (FREEZE, ANALYZE) sparse;

CREATE TABLE shapes (ord serial, name text, q text);
INSERT INTO shapes (name, q) VALUES
  ('top 3',                 $q$SELECT * FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY amount DESC NULLS LAST, id) rn FROM wt) t WHERE rn <= 3$q$),
  ('top 2 ascending',       $q$SELECT * FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t WHERE rn < 3$q$),
  ('bound on the left',     $q$SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts DESC, id) rn FROM wt) t WHERE 4 >= rn$q$),
  ('more than a group',     $q$SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY id) rn FROM wt) t WHERE rn <= 5000$q$),
  ('two partition columns', $q$SELECT * FROM (SELECT id, grp, grp2, row_number() OVER (PARTITION BY grp, grp2 ORDER BY amount NULLS FIRST, id) rn FROM wt) t WHERE rn <= 2$q$),
  ('condition inside',      $q$SELECT * FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY amount DESC NULLS LAST, id) rn FROM wt WHERE amount > 100 AND grp < 30) t WHERE rn <= 3$q$),
  ('condition outside',     $q$SELECT * FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t WHERE rn <= 3 AND id > 2000$q$),
  ('row number read',       $q$SELECT id, grp, rn, rn * 10 AS x FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t WHERE rn <= 3$q$),
  ('partition not read',    $q$SELECT id, amount FROM (SELECT id, amount, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t WHERE rn <= 3$q$),
  ('order by an expression', $q$SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts + id DESC, id) rn FROM wt) t WHERE rn <= 3$q$),
  ('order by the partition', $q$SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY grp, ts, id) rn FROM wt) t WHERE rn <= 3$q$),
  ('nullable partition',    $q$SELECT * FROM (SELECT id, gn, row_number() OVER (PARTITION BY gn ORDER BY ts, id) rn FROM wt) t WHERE rn <= 3$q$),
  ('join inside',           $q$SELECT * FROM (SELECT w.id, d.label, row_number() OVER (PARTITION BY w.grp ORDER BY w.ts, w.id) rn FROM wt w JOIN dim d ON d.grp = w.grp) t WHERE rn <= 3$q$),
  ('join outside',          $q$SELECT t.id, d.label, t.rn FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t JOIN dim d ON d.grp = t.grp WHERE t.rn <= 2 AND d.label <> 'g3'$q$),
  ('aggregate on top',      $q$SELECT grp, sum(amount), count(*) FROM (SELECT grp, amount, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t WHERE rn <= 3 GROUP BY grp$q$),
  ('nothing left',          $q$SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt WHERE amount > 1000) t WHERE rn <= 3$q$),
  ('keys of one row',       $q$SELECT * FROM (SELECT id, grp, amount, row_number() OVER (PARTITION BY grp ORDER BY amount, id) rn FROM sparse) t WHERE rn <= 3$q$),
  ('rank stays',            $q$SELECT * FROM (SELECT id, grp, rank() OVER (PARTITION BY grp ORDER BY amount, id) rn FROM wt) t WHERE rn <= 3$q$),
  ('second row only',       $q$SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t WHERE rn = 2$q$),
  ('first row',             $q$SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t WHERE rn = 1$q$);

CREATE FUNCTION shape_check(q text) RETURNS TABLE (in_scan bool, topn bool, same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  in_scan := plan LIKE '%Custom Scan (SiftAgg)%';
  topn := plan LIKE '%Top-N:%';
  PERFORM set_config('sift.enable_agg', 'on', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO a;
  PERFORM set_config('sift.enable_agg', 'off', true);
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO b;
  PERFORM set_config('sift.enable_agg', 'on', true);
  same := a IS NOT DISTINCT FROM b;
  RETURN NEXT;
END $$;
SELECT s.name, c.* FROM shapes s, LATERAL shape_check(s.q) c ORDER BY s.ord;

SELECT count(*), min(rn), max(rn) FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t WHERE rn <= 3;

DROP FUNCTION shape_check(text);
DROP TABLE shapes, wt, dim, sparse;
DROP EXTENSION pg_sift;
