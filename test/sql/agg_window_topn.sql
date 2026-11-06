-- `SELECT .. FROM (SELECT .., row_number() OVER (PARTITION BY k ORDER BY o) rn FROM t) s WHERE rn <= N` for N > 1 is the groups' arrays cut at N inside the group,
-- `(SELECT k, (array_agg(c ORDER BY o))[1:N] a FROM t GROUP BY k) g, LATERAL unnest(g.a)` (agg/latest.c), where the scan keeps only the first N rows of each group
-- as the rows pass (SiftAggGen.topn, agg/gen_run.c) and the plan with the sort of every row is the other choice. Every statement runs with sift on and off and the rows are
-- compared (md5 of the sorted rows; the orders have no ties, but for the rank() and dense_rank() ones); `sliced` says the plan holds the first rows of the groups (a pushed array_agg[1:N]), `in_scan` that the scan ran.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
CREATE TABLE wt (id int PRIMARY KEY, grp int NOT NULL, grp2 int NOT NULL, gn int, amount int, ts int NOT NULL, note text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO wt SELECT i, i % 40, i % 3, CASE WHEN i % 9 = 0 THEN NULL ELSE i % 25 END, CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 37) % 500 END, (i * 101) % 9973, 'n' || i FROM generate_series(1, 120000) i;
CREATE TABLE wtab (id int PRIMARY KEY, grp int NOT NULL, amount int, ts int NOT NULL, memo text NOT NULL) USING sift_table;
INSERT INTO wtab SELECT i, i % 40, CASE WHEN i % 7 = 0 THEN NULL ELSE (i * 37) % 500 END, (i * 101) % 9973, 'm' || (i % 977) FROM generate_series(1, 120000) i;
CREATE TABLE dim (grp int PRIMARY KEY, label text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i, 'g' || i FROM generate_series(0, 39) i;
CREATE TABLE sparse AS SELECT i AS id, i AS grp, (i * 7) % 100 AS amount FROM generate_series(1, 150000) i;
CREATE INDEX wt_col ON wt USING sift_col (id, grp, grp2, gn, amount, ts, note) WITH (key_cut = grp);
CREATE INDEX sparse_col ON sparse USING sift_col (id, grp, amount);
VACUUM (FREEZE, ANALYZE) wt;
VACUUM (FREEZE, ANALYZE) wtab;
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
  ('rank over a unique order', $q$SELECT * FROM (SELECT id, grp, rank() OVER (PARTITION BY grp ORDER BY amount, id) rn FROM wt) t WHERE rn <= 3$q$),
  ('second row only',       $q$SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t WHERE rn = 2$q$),
  ('first row',             $q$SELECT * FROM (SELECT id, grp, row_number() OVER (PARTITION BY grp ORDER BY ts, id) rn FROM wt) t WHERE rn = 1$q$);

-- a slice a[1:n] or an element a[n] of an array_agg with ORDER BY that the query writes itself is read the same way; the aggregate read whole anywhere is not
INSERT INTO shapes (name, q) VALUES
  ('slice',                 $q$SELECT grp, (array_agg(id ORDER BY amount DESC NULLS LAST, id))[1:3] FROM wt GROUP BY grp$q$),
  ('element',               $q$SELECT grp, (array_agg(id ORDER BY ts, id))[2] FROM wt GROUP BY grp$q$),
  ('element past the group', $q$SELECT grp, (array_agg(id ORDER BY ts, id))[900] FROM wt GROUP BY grp$q$),
  ('two slices',            $q$SELECT grp, (array_agg(id ORDER BY ts, id))[1:2], (array_agg(id ORDER BY ts, id))[1:4] FROM wt GROUP BY grp$q$),
  ('slice and whole',       $q$SELECT grp, (array_agg(id ORDER BY ts, id))[1:2], cardinality(array_agg(id ORDER BY ts, id)) FROM wt GROUP BY grp$q$),
  ('slice with a filter',   $q$SELECT grp, (array_agg(id ORDER BY ts, id) FILTER (WHERE amount > 100))[1:3] FROM wt GROUP BY grp$q$),
  ('text and NULLs first',  $q$SELECT grp, (array_agg(note ORDER BY gn NULLS FIRST, id DESC))[1:3] FROM wt GROUP BY grp$q$),
  ('ties',                  $q$SELECT grp, (array_agg(amount ORDER BY amount))[1:5] FROM wt GROUP BY grp$q$),
  ('descending, NULLs first', $q$SELECT grp, (array_agg(amount ORDER BY amount DESC NULLS FIRST))[1:5] FROM wt GROUP BY grp$q$),
  ('slice of a join',       $q$SELECT d.label, (array_agg(w.id ORDER BY w.ts, w.id))[1:3] FROM wt w JOIN dim d ON d.grp = w.grp GROUP BY d.label$q$),
  ('slice in HAVING',       $q$SELECT grp FROM wt GROUP BY grp HAVING (array_agg(id ORDER BY ts, id))[1:2] = ARRAY[(SELECT min(id) FROM wt WHERE ts = 0), 1]$q$),
  ('no group',              $q$SELECT (array_agg(id ORDER BY ts, id))[1:3] FROM wt$q$),
  ('many groups, a growing table', $q$SELECT ts, (array_agg(note ORDER BY id))[1:2], (array_agg(grp ORDER BY id))[1:2] FROM wt GROUP BY ts$q$);

-- rank() and dense_rank() keep the rows that tie: the scan holds the rows of rank <= N (the N best and every row that ties with the N-th) or of dense_rank <= N (the N best
-- values), and the window of the query ranks them. The orders below tie on purpose (amount has 500 values in groups of 3,000 rows, NULLs among them).
INSERT INTO shapes (name, q) VALUES
  ('rank, ties',            $q$SELECT * FROM (SELECT id, grp, amount, rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt) t WHERE rn <= 3$q$),
  ('rank, descending NULLs first', $q$SELECT * FROM (SELECT id, grp, amount, rank() OVER (PARTITION BY grp ORDER BY amount DESC NULLS FIRST) rn FROM wt) t WHERE rn <= 4$q$),
  ('rank, NULLs last',      $q$SELECT * FROM (SELECT id, grp, amount, rank() OVER (PARTITION BY grp ORDER BY amount DESC NULLS LAST) rn FROM wt) t WHERE rn <= 2$q$),
  ('rank = 1',              $q$SELECT * FROM (SELECT id, grp, amount, rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt) t WHERE rn = 1$q$),
  ('rank, N past the ties', $q$SELECT * FROM (SELECT id, grp, amount, rank() OVER (PARTITION BY grp ORDER BY ts / 40) rn FROM wt) t WHERE rn <= 100$q$),
  ('rank, two order keys',  $q$SELECT * FROM (SELECT id, grp, amount, rank() OVER (PARTITION BY grp ORDER BY amount, ts / 100) rn FROM wt) t WHERE rn < 5$q$),
  ('rank, two partition columns', $q$SELECT * FROM (SELECT id, grp, grp2, rank() OVER (PARTITION BY grp, grp2 ORDER BY amount NULLS FIRST) rn FROM wt) t WHERE rn <= 2$q$),
  ('rank, nullable partition', $q$SELECT * FROM (SELECT id, gn, amount, rank() OVER (PARTITION BY gn ORDER BY amount) rn FROM wt) t WHERE rn <= 2$q$),
  ('rank, read and ordered', $q$SELECT id, grp, rn * 10 AS x FROM (SELECT id, grp, rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt) t WHERE rn <= 3 ORDER BY grp, rn, id$q$),
  ('rank, condition inside', $q$SELECT * FROM (SELECT id, grp, amount, rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt WHERE ts > 3000 AND grp < 30) t WHERE rn <= 3$q$),
  ('rank, join outside',    $q$SELECT t.id, d.label, t.rn FROM (SELECT id, grp, rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt) t JOIN dim d ON d.grp = t.grp WHERE t.rn <= 2$q$),
  ('rank, aggregate on top', $q$SELECT grp, count(*), max(rn) FROM (SELECT grp, rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt) t WHERE rn <= 3 GROUP BY grp$q$),
  ('rank, order by the partition', $q$SELECT * FROM (SELECT id, grp, rank() OVER (PARTITION BY grp ORDER BY grp) rn FROM wt) t WHERE rn <= 3$q$),
  ('rank, many groups',     $q$SELECT * FROM (SELECT id, ts, rank() OVER (PARTITION BY ts ORDER BY id % 5) rn FROM wt) t WHERE rn <= 2$q$),
  ('dense_rank, many groups', $q$SELECT * FROM (SELECT id, ts, dense_rank() OVER (PARTITION BY ts ORDER BY id % 5 DESC) rn FROM wt) t WHERE rn <= 2$q$),
  ('rank, past 1,000',      $q$SELECT * FROM (SELECT id, grp, rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt) t WHERE rn <= 1500$q$),
  ('dense_rank, ties',      $q$SELECT * FROM (SELECT id, grp, amount, dense_rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt) t WHERE rn <= 3$q$),
  ('dense_rank, descending NULLs first', $q$SELECT * FROM (SELECT id, grp, amount, dense_rank() OVER (PARTITION BY grp ORDER BY amount DESC NULLS FIRST) rn FROM wt) t WHERE rn <= 4$q$),
  ('dense_rank = 1',        $q$SELECT * FROM (SELECT id, grp, amount, dense_rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt) t WHERE rn = 1$q$),
  ('dense_rank, two order keys', $q$SELECT * FROM (SELECT id, grp, amount, dense_rank() OVER (PARTITION BY grp ORDER BY amount, ts / 100) rn FROM wt) t WHERE rn <= 5$q$),
  ('dense_rank, two partition columns', $q$SELECT * FROM (SELECT id, grp, grp2, dense_rank() OVER (PARTITION BY grp, grp2 ORDER BY amount) rn FROM wt) t WHERE rn <= 2$q$),
  ('dense_rank, nullable partition', $q$SELECT * FROM (SELECT id, gn, amount, dense_rank() OVER (PARTITION BY gn ORDER BY amount) rn FROM wt) t WHERE rn <= 2$q$),
  ('dense_rank, past 1,000', $q$SELECT * FROM (SELECT id, grp, dense_rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt) t WHERE rn <= 1500$q$),
  ('rank and row_number',   $q$SELECT * FROM (SELECT id, grp, rank() OVER w rn, row_number() OVER w r2 FROM wt WINDOW w AS (PARTITION BY grp ORDER BY amount)) t WHERE rn <= 3$q$),
  ('sift_table, row_number', $q$SELECT id, grp, memo, rn FROM (SELECT id, grp, memo, row_number() OVER (PARTITION BY grp ORDER BY amount DESC NULLS LAST, id) rn FROM wtab) w WHERE rn <= 3$q$),
  ('sift_table, rank',      $q$SELECT id, grp, memo, rn FROM (SELECT id, grp, memo, rank() OVER (PARTITION BY grp ORDER BY amount DESC NULLS LAST) rn FROM wtab) w WHERE rn <= 3$q$),
  ('sift_table, dense_rank', $q$SELECT id, grp, memo, rn FROM (SELECT id, grp, memo, dense_rank() OVER (PARTITION BY grp ORDER BY amount DESC NULLS LAST) rn FROM wtab) w WHERE rn <= 2$q$),
  ('percent_rank stays',    $q$SELECT * FROM (SELECT id, grp, percent_rank() OVER (PARTITION BY grp ORDER BY amount) rn FROM wt) t WHERE rn <= 0.01$q$);

CREATE FUNCTION shape_check(q text) RETURNS TABLE (in_scan bool, sliced bool, same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; a text; b text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
  in_scan := plan LIKE '%Custom Scan (SiftAgg)%';
  sliced := plan LIKE '%array_agg(%)[1:%' OR plan LIKE '%array_agg(%)[rank <=%' OR plan LIKE '%array_agg(%)[dense_rank <=%';
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
DROP TABLE shapes, wt, wtab, dim, sparse;
DROP EXTENSION pg_sift;
