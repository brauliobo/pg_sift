-- A page of rows with the total of the set it is a page of: `SELECT cols, count(*) OVER () FROM t WHERE .. ORDER BY k LIMIT n OFFSET m`. A window
-- function that is an aggregate over the whole set (no PARTITION BY, no ORDER BY, a frame of all the rows) is computed by the top-N scan from every
-- row that passes, besides the heap of the n best. Every query runs twice, the plain plan as the reference (sift.enable_agg off) and the pushed one,
-- on a sift_col index and on a sift_table, and the rows are compared in order: after the build and after each kind of write. Any other window
-- keeps the normal plan, with its reason.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET client_min_messages = warning;

-- pad makes a page hold about 5 rows: 12000 rows are many superblocks, more than a window of the visit order. The floats are quarters, so that their sums do not depend on the order of the additions
CREATE TABLE pw (id int PRIMARY KEY, kind text, grp int, ts timestamp, amount numeric(10,2), score float8, n int, note text, pad text DEFAULT repeat('x', 1500));
ALTER TABLE pw ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO pw (id, kind, grp, ts, amount, score, n, note)
SELECT i,
	   CASE WHEN i % 23 = 0 THEN NULL ELSE 'cat_' || chr(97 + (hashint4(i) & 2147483647) % 4) END,
	   CASE WHEN i % 31 = 0 THEN NULL ELSE (hashint4(i + 5) & 2147483647) % 12 END,
	   timestamp '2026-01-01' + ((hashint4(i + 11) & 2147483647) % 4000000) * interval '1 second',
	   CASE WHEN i % 997 = 0 THEN 'NaN'::numeric WHEN i % 29 = 0 THEN NULL ELSE ((hashint4(i + 3) & 2147483647) % 30000)::numeric / 100 END,
	   CASE WHEN i % 37 = 0 THEN NULL ELSE ((hashint4(i + 13) & 2147483647) % 4000) / 4.0 END,
	   (hashint4(i + 17) & 2147483647) % 900,
	   CASE WHEN i % 41 = 0 THEN NULL ELSE 'note_' || (i % 50) END
FROM generate_series(1, 12000) i;
CREATE INDEX pw_col ON pw USING sift_col (id, kind, grp, ts, amount, score, n, note) WITH (write_buffer_limit = 256);
VACUUM (ANALYZE) pw;
CREATE TABLE pw_t (LIKE pw) USING sift_table;
ALTER TABLE pw_t ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO pw_t SELECT * FROM pw;
VACUUM (ANALYZE) pw_t;

CREATE FUNCTION pw_mode(pushed bool) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', pushed::text, true);
	PERFORM set_config('enable_seqscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_indexscan', (NOT pushed)::text, true);
	PERFORM set_config('enable_bitmapscan', (NOT pushed)::text, true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
END $$;

-- 'same' / 'DIFFERENT' (the rows and their order), '+topn' when the pushed plan is the top-N scan, '+totals' when it computes the window functions, and the rows
CREATE FUNCTION pw_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n bigint; plan text := ''; line text; res text;
BEGIN
	DROP TABLE IF EXISTS pw_a, pw_b;
	PERFORM pw_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP plan := plan || line || E'\n'; END LOOP;
	FOR line IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF) ' || q LOOP NULL; END LOOP;
	EXECUTE 'CREATE TEMP TABLE pw_a AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	PERFORM pw_mode(false);
	EXECUTE 'CREATE TEMP TABLE pw_b AS SELECT row_number() OVER () AS pos, row_to_json(x)::text AS r FROM (' || q || ') x';
	SELECT count(*) INTO n FROM ((SELECT pos, r FROM pw_a EXCEPT ALL SELECT pos, r FROM pw_b) UNION ALL (SELECT pos, r FROM pw_b EXCEPT ALL SELECT pos, r FROM pw_a)) d;
	res := CASE WHEN n = 0 THEN 'same' ELSE 'DIFFERENT' END;
	RETURN res || CASE WHEN plan ~ 'Top-N: ' THEN '+topn' ELSE '' END || CASE WHEN plan ~ 'Pushed Aggregates: ' THEN '+totals' ELSE '' END || ' ' || (SELECT count(*) FROM pw_b);
END $$;

-- why the top-N arm kept the normal plan
CREATE FUNCTION pw_why(q text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM pw_mode(true);
	RETURN coalesce((SELECT slug || ': ' || reason FROM sift.explain_why(q) WHERE arm = 'topn'), 'none');
END $$;

CREATE TABLE pw_q (tag text PRIMARY KEY, q text NOT NULL);
INSERT INTO pw_q VALUES
	('count',        $q$SELECT id, ts, count(*) OVER () AS total FROM {t} WHERE kind = 'cat_a' ORDER BY ts DESC, id LIMIT 10$q$),
	('count_asc',    $q$SELECT id, ts, count(*) OVER () AS total FROM {t} WHERE kind = 'cat_b' AND grp > 3 ORDER BY ts, id LIMIT 25$q$),
	('aggregates',   $q$SELECT id, n, count(*) OVER () AS c, count(amount) OVER () AS ca, sum(amount) OVER () AS sa, avg(amount) OVER () AS aa, sum(score) OVER () AS ss, avg(score) OVER () AS vs,
							  min(ts) OVER () AS lo, max(ts) OVER () AS hi, min(note) OVER () AS nlo, max(n) OVER () AS nhi, sum(n) OVER () AS sn
					   FROM {t} WHERE grp IS NOT NULL AND n < 600 ORDER BY n DESC, id LIMIT 12$q$),
	('offset',       $q$SELECT id, grp, count(*) OVER () AS total FROM {t} WHERE kind = 'cat_c' ORDER BY id LIMIT 7 OFFSET 40$q$),
	('past_end',     $q$SELECT id, grp, count(*) OVER () AS total FROM {t} WHERE kind = 'cat_c' ORDER BY id LIMIT 7 OFFSET 100000$q$),
	('last_page',    $q$SELECT id, grp, count(*) OVER () AS total FROM {t} WHERE kind = 'cat_c' AND id > 11000 ORDER BY id LIMIT 50 OFFSET 200$q$),
	('empty',        $q$SELECT id, count(*) OVER () AS total, sum(amount) OVER () AS s FROM {t} WHERE kind = 'cat_a' AND n > 100000 ORDER BY id LIMIT 10$q$),
	('few_rows',     $q$SELECT id, count(*) OVER () AS total, sum(amount) OVER () AS s FROM {t} WHERE id < 20 ORDER BY id LIMIT 10$q$),
	('no_filter',    $q$SELECT id, count(*) OVER () AS total FROM {t} ORDER BY ts DESC, id LIMIT 5$q$),
	('only_window',  $q$SELECT count(*) OVER () AS total, sum(n) OVER () AS s FROM {t} WHERE kind = 'cat_d' ORDER BY ts, id LIMIT 3$q$),
	('ties',         $q$SELECT id, grp, count(*) OVER () AS total FROM {t} WHERE n < 300 ORDER BY grp, id LIMIT 30$q$),
	('nulls_first',  $q$SELECT id, grp, count(grp) OVER () AS nonnull, count(*) OVER () AS total FROM {t} WHERE n < 300 ORDER BY grp NULLS FIRST, id DESC LIMIT 30$q$),
	('text_key',     $q$SELECT id, note, count(*) OVER () AS total FROM {t} WHERE n < 300 ORDER BY note DESC NULLS LAST, id LIMIT 20$q$),
	('expression',   $q$SELECT id, round(100 * amount / nullif(sum(amount) OVER (), 0), 4) AS share, count(*) OVER () - 1 AS others FROM {t} WHERE kind = 'cat_a' AND amount < 'NaN' ORDER BY id LIMIT 10$q$),
	('filter',       $q$SELECT id, count(*) FILTER (WHERE grp = 3) OVER () AS g3, sum(n) FILTER (WHERE kind = 'cat_a') OVER () AS na FROM {t} WHERE n < 500 ORDER BY id LIMIT 10$q$),
	('nan',          $q$SELECT id, sum(amount) OVER () AS s, avg(amount) OVER () AS a, max(amount) OVER () AS m FROM {t} WHERE id % 997 < 3 ORDER BY id LIMIT 6$q$),
	('whole_frame',  $q$SELECT id, count(*) OVER (ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS c, sum(n) OVER (RANGE BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS s FROM {t} WHERE kind = 'cat_b' ORDER BY id LIMIT 8$q$),
	('named',        $q$SELECT id, count(*) OVER w AS c, max(n) OVER w AS m FROM {t} WHERE kind = 'cat_b' WINDOW w AS () ORDER BY id LIMIT 8$q$),
	('two_windows',  $q$SELECT id, count(*) OVER () AS c, sum(n) OVER (ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE NO OTHERS) AS s FROM {t} WHERE kind = 'cat_b' ORDER BY id LIMIT 8$q$),
	('same_twice',   $q$SELECT id, count(*) OVER () AS c, count(*) OVER () + 1 AS c1 FROM {t} WHERE kind = 'cat_b' ORDER BY id LIMIT 8$q$),
	('many_rows',    $q$SELECT id, count(*) OVER () AS total FROM {t} WHERE grp < 9 ORDER BY ts, id LIMIT 1500$q$),
	('many_offset',  $q$SELECT id, count(*) OVER () AS total FROM {t} WHERE grp < 9 ORDER BY ts DESC, id LIMIT 30 OFFSET 2000$q$),
	('two_keys',     $q$SELECT id, count(*) OVER () AS total FROM {t} WHERE kind <> 'cat_a' ORDER BY grp DESC, ts, id LIMIT 40$q$),
	('in_list',      $q$SELECT id, count(*) OVER () AS total FROM {t} WHERE kind IN ('cat_a', 'cat_d') AND n BETWEEN 100 AND 200 ORDER BY id DESC LIMIT 15$q$),
	('arg_is_key',   $q$SELECT id, sum(n) OVER () AS s, count(n) OVER () AS c, sum(grp) OVER () AS g FROM {t} WHERE n IS NOT NULL ORDER BY grp NULLS FIRST, n, id LIMIT 6$q$),
	('arg_is_key0',  $q$SELECT id, sum(n) OVER () AS s, max(n) OVER () AS m FROM {t} WHERE n IS NOT NULL ORDER BY n DESC, id LIMIT 6$q$),
	('arg_is_key3',  $q$SELECT id, sum(amount) OVER () AS s, count(*) FILTER (WHERE n > 300) OVER () AS c FROM {t} WHERE amount IS NOT NULL ORDER BY kind, grp, amount, n, id LIMIT 4$q$),
	('expr_out',     $q$SELECT id, n * 2 AS twice, upper(kind) AS k, count(*) OVER () AS total FROM {t} WHERE grp = 4 ORDER BY id LIMIT 9$q$);

SELECT tag, pw_check(replace(q, '{t}', 'pw')) AS sift_col, pw_check(replace(q, '{t}', 'pw_t')) AS sift_table FROM pw_q ORDER BY tag;

-- windows that are not aggregates over the whole set keep the normal plan
CREATE TABLE pw_r (tag text PRIMARY KEY, q text NOT NULL);
INSERT INTO pw_r VALUES
	('row_number',   $q$SELECT id, row_number() OVER () AS rn FROM pw WHERE kind = 'cat_a' ORDER BY id LIMIT 5$q$),
	('partition',    $q$SELECT id, count(*) OVER (PARTITION BY kind) AS c FROM pw WHERE n < 100 ORDER BY id LIMIT 5$q$),
	('ordered',      $q$SELECT id, sum(n) OVER (ORDER BY id) AS c FROM pw WHERE n < 100 ORDER BY id LIMIT 5$q$),
	('rows_frame',   $q$SELECT id, sum(n) OVER (ROWS BETWEEN 1 PRECEDING AND CURRENT ROW) AS c FROM pw WHERE n < 100 ORDER BY id LIMIT 5$q$),
	('excluding',    $q$SELECT id, count(*) OVER (ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING EXCLUDE CURRENT ROW) AS c FROM pw WHERE n < 100 ORDER BY id LIMIT 5$q$),
	('first_value',  $q$SELECT id, first_value(n) OVER () AS c FROM pw WHERE n < 100 ORDER BY id LIMIT 5$q$),
	('distinct',     $q$SELECT DISTINCT count(*) OVER () AS c FROM pw WHERE n < 100 ORDER BY 1 LIMIT 5$q$);
SELECT tag, pw_check(q) AS result, pw_why(q) AS why FROM pw_r ORDER BY tag;

-- LIMIT and OFFSET by parameter of a generic plan: the scan reads their values when it starts
SET plan_cache_mode = force_generic_plan;
PREPARE pw_page(int, int) AS SELECT id, ts, count(*) OVER () AS total FROM pw WHERE kind = 'cat_a' ORDER BY ts, id LIMIT $1 OFFSET $2;
SELECT pw_mode(true);
CREATE TEMP TABLE pw_g1 AS EXECUTE pw_page(5, 0);
SELECT pw_mode(false);
CREATE TEMP TABLE pw_g2 AS EXECUTE pw_page(5, 0);
SELECT count(*) AS differences FROM ((TABLE pw_g1 EXCEPT ALL TABLE pw_g2) UNION ALL (TABLE pw_g2 EXCEPT ALL TABLE pw_g1)) d;
RESET plan_cache_mode;
DROP TABLE pw_g1, pw_g2;

-- EXPLAIN (VERBOSE) prints the window functions the scan computes as the aggregates they are (pw_check runs it for every query above): ruleutils prints a WindowFunc only under a WindowAgg
CREATE FUNCTION pw_output(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM pw_mode(true);
	FOR line IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF) ' || q LOOP
		IF line ~ 'Output:' THEN RETURN NEXT line; END IF;
	END LOOP;
END $$;
SELECT pw_output(replace(q, '{t}', 'pw')) AS output FROM pw_q WHERE tag IN ('count', 'aggregates') ORDER BY tag;

-- writes: the rows of the write buffer, the rows an UPDATE moved, the rows a DELETE left
UPDATE pw SET n = n + 1000, kind = 'cat_a' WHERE id % 53 = 0;
UPDATE pw SET amount = amount + 1 WHERE id % 59 = 0 AND amount < 'NaN';
DELETE FROM pw WHERE id % 61 = 0;
INSERT INTO pw (id, kind, grp, ts, amount, score, n, note) SELECT i, 'cat_a', i % 5, timestamp '2026-02-01' + i * interval '1 minute', i / 10.0, i / 4.0, i % 700, 'note_new' FROM generate_series(12001, 12300) i;
UPDATE pw_t SET n = n + 1000, kind = 'cat_a' WHERE id % 53 = 0;
UPDATE pw_t SET amount = amount + 1 WHERE id % 59 = 0 AND amount < 'NaN';
DELETE FROM pw_t WHERE id % 61 = 0;
INSERT INTO pw_t (id, kind, grp, ts, amount, score, n, note) SELECT i, 'cat_a', i % 5, timestamp '2026-02-01' + i * interval '1 minute', i / 10.0, i / 4.0, i % 700, 'note_new' FROM generate_series(12001, 12300) i;
SELECT tag, pw_check(replace(q, '{t}', 'pw')) AS sift_col, pw_check(replace(q, '{t}', 'pw_t')) AS sift_table FROM pw_q ORDER BY tag;
DROP TABLE pw, pw_q, pw_r, pw_t;
DROP FUNCTION pw_check(text), pw_mode(boolean), pw_why(text), pw_output(text);
DROP EXTENSION pg_sift;
