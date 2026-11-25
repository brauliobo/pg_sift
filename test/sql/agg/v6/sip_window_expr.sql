-- The vectorized execution engines of the aggregation (src/core/sbbf.c, src/agg/sip.c, window.c, vexpr.c), each held to PostgreSQL's own
-- evaluation of the same query over the same rows: NULLs, edge values, ties, empty partitions, errors.
-- The data is made by arithmetic, so that the output does not depend on the version.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET extra_float_digits = 1;

-- the 44 kernels of every level the CPU runs give the same answers as the scalar ones, the Bloom filter included
SELECT sift._test_aggkern6_equiv(seed, 60) AS wrong FROM generate_series(1, 3) AS seed;

-- the filter of a join: 8 seeds of random sizes, key types, budgets, parts merged from four builders; a miss, a NULL that passes or a filter that
-- is not exact for the 64 keys it keeps exactly is a failure
SELECT sum(sift._test_sip(seed, 40)) AS wrong FROM generate_series(1, 8) AS seed;

-- the filter as the semi-filter of the join `fact JOIN dim ON fact.k = dim.k`: no match is dropped, no NULL passes, up to 64 distinct keys it is exact,
-- over more the false positives stay below 5% of the rows without a match
CREATE TABLE sip_fact AS
SELECT g AS id, CASE WHEN g % 17 = 0 THEN NULL ELSE (g * 7919) % 5000 END::int8 AS k FROM generate_series(1, 20000) AS g;
CREATE TABLE sip_dim AS
SELECT n, (g * 13) % 5000 AS k FROM (VALUES (1), (10), (64), (65), (1000), (4000)) AS v(n), LATERAL generate_series(1, n) AS g;
INSERT INTO sip_dim SELECT 65, NULL;
CREATE TABLE sip_in AS
SELECT d.n, (SELECT array_agg(k ORDER BY id) FROM sip_fact) AS fact, (SELECT array_agg(k) FROM sip_dim x WHERE x.n = d.n) AS dim
FROM (SELECT DISTINCT n FROM sip_dim) AS d;
INSERT INTO sip_in SELECT 0, (SELECT array_agg(k ORDER BY id) FROM sip_fact), '{}';

SELECT sip_in.n, as_int4, count(*) FILTER (WHERE m.k IS NOT NULL AND NOT pass) AS missed,
       count(*) FILTER (WHERE f.k IS NULL AND pass) AS nulls_passed,
       count(*) FILTER (WHERE sip_in.n <= 64 AND f.k IS NOT NULL AND m.k IS NULL AND pass) AS inexact,
       count(*) FILTER (WHERE sip_in.n > 64 AND f.k IS NOT NULL AND m.k IS NULL AND pass)
         < 0.05 * count(*) FILTER (WHERE f.k IS NOT NULL AND m.k IS NULL) AS fpr_below_5pct,
       count(*) FILTER (WHERE pass) < count(*) AS filters
FROM sip_in CROSS JOIN (VALUES (false), (true)) AS t(as_int4)
     CROSS JOIN LATERAL sift._test_sip_filter(fact, dim, as_int4) AS r
     JOIN sip_fact f ON f.id = r.ord
     LEFT JOIN (SELECT DISTINCT n, k FROM sip_dim WHERE k IS NOT NULL) AS m ON m.n = sip_in.n AND m.k = f.k
GROUP BY sip_in.n, as_int4 ORDER BY sip_in.n, as_int4;

-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- Window functions and DISTINCT ON over rows sorted by the partition and the order key
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
CREATE FUNCTION diff(a text, b text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
	EXECUTE format('SELECT count(*) FROM (((%1$s) EXCEPT ALL (%2$s)) UNION ALL ((%2$s) EXCEPT ALL (%1$s))) AS d', a, b) INTO n;
	RETURN n;
END $$;

-- wu: ordered by a unique key, with NULL partitions and arguments, one-row and all-NULL partitions
CREATE TABLE wsrc_u AS
SELECT g AS id, CASE WHEN g % 13 = 0 THEN NULL WHEN g = 2999 THEN 99 ELSE (g * 37) % 11 END::int8 AS part, g::int8 AS ord,
       CASE WHEN g % 5 = 0 OR (g * 37) % 11 = 7 THEN NULL ELSE (g * 31) % 1000 - 400 END::int8 AS arg,
       CASE WHEN g % 7 = 0 THEN NULL ELSE ((g * 17) % 200)::float8 / 4 - 20 END AS argf
FROM generate_series(1, 3000) AS g;
-- wt: peers (ties) and NULL order keys
CREATE TABLE wsrc_t AS
SELECT g AS id, ((g * 7) % 5)::int8 AS part, CASE WHEN g % 11 = 0 THEN NULL ELSE g % 6 END::int8 AS ord,
       CASE WHEN g % 4 = 0 THEN NULL ELSE (g * 13) % 50 - 20 END::int8 AS arg, ((g * 3) % 40)::float8 / 8 AS argf
FROM generate_series(1, 700) AS g;
-- wp: 20 partitions of 1000 rows, for the skipping of the granules
CREATE TABLE wsrc_p AS
SELECT g AS id, (g / 1000)::int8 AS part, g::int8 AS ord, g::int8 AS arg, g::float8 AS argf FROM generate_series(0, 19999) AS g;

-- wg: int8 values to INT64_MAX and from INT64_MIN, NULLs, an order key that is unique (wg) or has ties (wg2): sums beyond 64 bits, averages that float8 cannot hold
CREATE TABLE wsrc_g AS
SELECT g AS id, (g % 3)::int8 AS part, g::int8 AS ord,
       CASE WHEN g % 9 = 0 THEN NULL WHEN g % 5 = 0 THEN (-9223372036854775807 + (g * 7919) % 100003)::int8 ELSE (9223372036854775807 - (g * 7919) % 100003)::int8 END AS arg,
       g::float8 AS argf
FROM generate_series(1, 400) AS g;

CREATE TABLE wd (name text PRIMARY KEY, p int8[], o int8[], a int8[], f float8[], x bool[]);
INSERT INTO wd SELECT 'wu', array_agg(part ORDER BY part NULLS LAST, ord), array_agg(ord ORDER BY part NULLS LAST, ord),
                      array_agg(arg ORDER BY part NULLS LAST, ord), array_agg(argf ORDER BY part NULLS LAST, ord),
                      array_agg(id % 3 <> 1 ORDER BY part NULLS LAST, ord) FROM wsrc_u;
INSERT INTO wd SELECT 'wt', array_agg(part ORDER BY part, ord NULLS LAST, id), array_agg(ord ORDER BY part, ord NULLS LAST, id),
                      array_agg(arg ORDER BY part, ord NULLS LAST, id), array_agg(argf ORDER BY part, ord NULLS LAST, id),
                      array_agg(id % 4 <> 0 ORDER BY part, ord NULLS LAST, id) FROM wsrc_t;
INSERT INTO wd SELECT 'wp', array_agg(part ORDER BY id), array_agg(ord ORDER BY id), array_agg(arg ORDER BY id), array_agg(argf ORDER BY id),
                      array_agg(true ORDER BY id) FROM wsrc_p;
INSERT INTO wd SELECT 'wg', array_agg(part ORDER BY part, ord), array_agg(ord ORDER BY part, ord), array_agg(arg ORDER BY part, ord),
                      array_agg(argf ORDER BY part, ord), array_agg(true ORDER BY part, ord) FROM wsrc_g;
INSERT INTO wd SELECT 'wg2', array_agg(part ORDER BY part, ord / 2, id), array_agg(ord / 2 ORDER BY part, ord / 2, id), array_agg(arg ORDER BY part, ord / 2, id),
                      array_agg(argf ORDER BY part, ord / 2, id), array_agg(true ORDER BY part, ord / 2, id) FROM wsrc_g;
INSERT INTO wd VALUES ('empty', '{}', '{}', '{}', '{}', '{}'), ('one', '{5}', '{1}', '{NULL}', '{2.5}', '{true}'),
                      ('nulls', '{NULL,NULL,NULL}', '{1,1,NULL}', '{3,NULL,5}', '{1,2,3}', '{true,true,true}');

CREATE FUNCTION wrows(n text) RETURNS TABLE (tag int8, part int8, ord int8, arg int8, argf float8, keep bool) LANGUAGE sql STABLE AS $$
	SELECT t.tag - 1, t.part, t.ord, t.arg, t.argf, t.keep
	FROM wd, ROWS FROM (unnest(p), unnest(o), unnest(a), unnest(f), unnest(x)) WITH ORDINALITY AS t(part, ord, arg, argf, keep, tag)
	WHERE name = n $$;

-- (data, engine functions, batch rows, the same in SQL, engine options, filter after the window, order and limit of the output)
CREATE TABLE wcase (label text PRIMARY KEY, ds text, funcs text, batch int, pgcols text, opts text DEFAULT '', after text DEFAULT '', tail text DEFAULT '');
INSERT INTO wcase (label, ds, funcs, batch, pgcols) SELECT 'u-A batch ' || b, 'wu', 'row_number,rank,dense_rank,lag:arg:2,lead:arg:1,first_value:arg,count:*,count:arg', b,
  '(row_number() OVER w)::text, (rank() OVER w)::text, (dense_rank() OVER w)::text, (lag(arg, 2) OVER w)::text, (lead(arg, 1) OVER w)::text, (first_value(arg) OVER w)::text, (count(*) OVER w)::text, (count(arg) OVER w)::text'
FROM unnest(ARRAY[1, 7, 64, 2048]) AS b;
INSERT INTO wcase (label, ds, funcs, batch, pgcols) SELECT 'u-B batch ' || b, 'wu', 'sum:arg,avg:argf,min:arg,max:argf,sum:arg:rows,min:argf:rows,lag:arg:1:-1,lead:argf:3', b,
  '(sum(arg) OVER w)::text, (avg(argf) OVER w)::text, (min(arg) OVER w)::text, (max(argf) OVER w)::text, (sum(arg) OVER (w ROWS UNBOUNDED PRECEDING))::text, (min(argf) OVER (w ROWS UNBOUNDED PRECEDING))::text, (lag(arg, 1, -1) OVER w)::text, (lead(argf, 3) OVER w)::text'
FROM unnest(ARRAY[1, 5, 100, 2048]) AS b;
INSERT INTO wcase (label, ds, funcs, batch, pgcols) SELECT 't-ties batch ' || b, 'wt', 'rank,dense_rank,count:*,count:arg,sum:arg,min:arg,max:argf,avg:argf', b,
  '(rank() OVER w)::text, (dense_rank() OVER w)::text, (count(*) OVER w)::text, (count(arg) OVER w)::text, (sum(arg) OVER w)::text, (min(arg) OVER w)::text, (max(argf) OVER w)::text, (avg(argf) OVER w)::text'
FROM unnest(ARRAY[1, 3, 64, 2048]) AS b;
-- avg and sum of integers are PostgreSQL's numerics: exact beyond 2^53 and 2^64, at the scale numeric_div gives
INSERT INTO wcase (label, ds, funcs, batch, pgcols) SELECT 'avg int ' || ds || ' batch ' || b, ds, 'avg:arg,sum:arg,count:arg', b,
  '(avg(arg) OVER w)::text, (sum(arg) OVER w)::text, (count(arg) OVER w)::text'
FROM unnest(ARRAY['wu', 'wt', 'wg', 'wg2']) AS ds, unnest(ARRAY[1, 64, 2048]) AS b;
INSERT INTO wcase (label, ds, funcs, batch, pgcols) SELECT 'avg int rows ' || ds || ' batch ' || b, ds, 'avg:arg:rows,sum:arg:rows', b,
  '(avg(arg) OVER (w ROWS UNBOUNDED PRECEDING))::text, (sum(arg) OVER (w ROWS UNBOUNDED PRECEDING))::text'
FROM unnest(ARRAY['wu', 'wg']) AS ds, unnest(ARRAY[1, 64, 2048]) AS b;
INSERT INTO wcase (label, ds, funcs, batch, pgcols, opts) VALUES
  ('u-A keys int4', 'wu', 'rank,dense_rank,count:*', 64, '(rank() OVER w)::text, (dense_rank() OVER w)::text, (count(*) OVER w)::text', ', keys_i32 => true'),
  ('t keys int4', 'wt', 'rank,dense_rank,count:*', 7, '(rank() OVER w)::text, (dense_rank() OVER w)::text, (count(*) OVER w)::text', ', keys_i32 => true');
-- the rows a WHERE keeps (the partitions and the peers are those of the kept rows), then OFFSET and LIMIT
INSERT INTO wcase (label, ds, funcs, batch, pgcols, opts) SELECT 'where ' || ds || ' batch ' || b, ds, 'row_number,rank,dense_rank,lag:arg:1,lead:arg:2,count:*,sum:arg,min:argf', b,
  '(row_number() OVER w)::text, (rank() OVER w)::text, (dense_rank() OVER w)::text, (lag(arg, 1) OVER w)::text, (lead(arg, 2) OVER w)::text, (count(*) OVER w)::text, (sum(arg) OVER w)::text, (min(argf) OVER w)::text',
  ', act => x'
FROM unnest(ARRAY['wu', 'wt']) AS ds, unnest(ARRAY[1, 5, 2048]) AS b;
INSERT INTO wcase (label, ds, funcs, batch, pgcols, opts, tail) SELECT 'limit ' || o || '/' || l || ' batch ' || b, 'wu', 'row_number,lead:arg:2,sum:arg', b,
  '(row_number() OVER w)::text, (lead(arg, 2) OVER w)::text, (sum(arg) OVER w)::text', ', off => ' || o || ', lim => ' || l,
  'ORDER BY tag OFFSET ' || o || ' LIMIT ' || l
FROM (VALUES (0, 10), (7, 5), (2990, 100), (3500, 10), (100, 0)) AS v(o, l), unnest(ARRAY[1, 64, 2048]) AS b;
-- top K of each partition by the first function
INSERT INTO wcase (label, ds, funcs, batch, pgcols, opts, after) SELECT 'top ' || k || ' ' || fn || ' ' || ds || ' batch ' || b, ds, fn || ',lag:arg:1,count:arg:rows', b,
  '(' || fn || '() OVER w)::text, (lag(arg, 1) OVER w)::text, (count(arg) OVER (w ROWS UNBOUNDED PRECEDING))::text', ', topk => ' || k, 'WHERE (res[1])::int8 <= ' || k
FROM unnest(ARRAY['wu', 'wt']) AS ds, unnest(ARRAY['row_number', 'rank', 'dense_rank']) AS fn, unnest(ARRAY[1, 3]) AS k, unnest(ARRAY[7, 2048]) AS b
WHERE NOT (ds = 'wt' AND fn = 'row_number');
-- the degenerate inputs
INSERT INTO wcase (label, ds, funcs, batch, pgcols) SELECT 'degenerate ' || ds, ds, 'row_number,rank,dense_rank,lag:arg:1,lead:arg:1,count:*,sum:arg,max:argf', 2048,
  '(row_number() OVER w)::text, (rank() OVER w)::text, (dense_rank() OVER w)::text, (lag(arg, 1) OVER w)::text, (lead(arg, 1) OVER w)::text, (count(*) OVER w)::text, (sum(arg) OVER w)::text, (max(argf) OVER w)::text'
FROM unnest(ARRAY['empty', 'one', 'nulls']) AS ds;

SELECT label, diff(format('SELECT e.tag, e.res FROM wd, sift._test_window(p, o, a, f, %L::text[], batch_rows => %s %s) e WHERE name = %L',
                          '{' || funcs || '}', batch, opts, ds),
                   format('SELECT tag, res FROM (SELECT tag, part, ord, ARRAY[%s]::text[] AS res FROM wrows(%L) %s WINDOW w AS (PARTITION BY part ORDER BY ord)) q %s %s',
                          pgcols, ds, CASE WHEN opts LIKE '%act%' THEN 'WHERE keep' ELSE '' END, after, tail)) AS wrong
FROM wcase ORDER BY label;

-- DISTINCT ON (part): the first or the last row of each partition, over the rows a WHERE keeps, with OFFSET and LIMIT
CREATE TABLE dcase (label text PRIMARY KEY, ds text, mode text, batch int, opts text, tail text);
INSERT INTO dcase SELECT m || ' ' || ds || ' batch ' || b || ' ' || o, ds, m, b, opts, tail
FROM unnest(ARRAY['first', 'last']) AS m, unnest(ARRAY['wu', 'wt', 'wp', 'empty', 'one', 'nulls']) AS ds, unnest(ARRAY[1, 64, 2048]) AS b,
     (VALUES ('plain', '', ''), ('where', ', act => x', ''), ('window', ', off => 1, lim => 4', 'OFFSET 1 LIMIT 4')) AS v(o, opts, tail);

SELECT label, diff(format('SELECT e.tag, NULL::text[] FROM wd, sift._test_window(p, o, a, f, ARRAY[''row_number''], batch_rows => %s, mode => %L %s) e WHERE name = %L',
                          batch, mode, opts, ds),
                   format('SELECT tag, NULL::text[] FROM (SELECT DISTINCT ON (part) tag, part FROM wrows(%L) %s ORDER BY part NULLS LAST, tag %s) q ORDER BY tag %s',
                          ds, CASE WHEN opts LIKE '%act%' THEN 'WHERE keep' ELSE '' END, CASE WHEN mode = 'last' THEN 'DESC' ELSE '' END, tail)) AS wrong
FROM dcase ORDER BY label;

-- a window whose columns and first ring are more than the budget is made all the same
SET work_mem = '64kB';
SELECT count(*) FROM wd, sift._test_window(p, o, a, f, ARRAY['row_number']) AS e WHERE name = 'wp';
SELECT count(*) FROM wd, sift._test_window(p, o, a, f, ARRAY['lag:arg:4096']) AS e WHERE name = 'wp';
RESET work_mem;
-- the output of DISTINCT ON and of a top-K stops at the first row of the partition, and the source passes over the granules of the rest of it
SELECT count(*) AS rows_out FROM wd, sift._test_window(p, o, a, f, ARRAY['row_number'], mode => 'first', granule => 128) AS e WHERE name = 'wp';
SELECT rows_out, granules_read, granules_total, rows_looked < rows_in AS looks_at_less, skips >= 19 AS skipped FROM sift._test_window_stats();
SELECT count(*) AS rows_out FROM wd, sift._test_window(p, o, a, f, ARRAY['rank'], topk => 2, granule => 100) AS e WHERE name = 'wp';
SELECT rows_out, granules_read < granules_total AS reads_less, rows_looked < rows_in AS looks_at_less FROM sift._test_window_stats();
SELECT count(*) AS rows_out FROM wd, sift._test_window(p, o, a, f, ARRAY['row_number'], mode => 'last', granule => 128) AS e WHERE name = 'wp';
SELECT rows_out, rows_looked = rows_in AS looks_at_all FROM sift._test_window_stats();

-- the memory of a window of ROW_NUMBER, RANK, LAG and a LEAD of 3 is the rows between the current row and the one it looks at, not a partition
SELECT count(*) AS rows_out FROM wd, sift._test_window(p, o, a, f, ARRAY['row_number', 'rank', 'lag:arg:2', 'lead:arg:3', 'sum:argf:rows'], batch_rows => 64) AS e
WHERE name = 'wp';
SELECT rows_in, rows_out, max_pending <= 4 AS pending_is_the_lookahead FROM sift._test_window_stats();
-- a RANGE frame holds the peers of the row, and nothing more
SELECT count(*) AS rows_out FROM wd, sift._test_window(p, o, a, f, ARRAY['rank', 'count:*', 'sum:arg'], batch_rows => 64) AS e WHERE name = 'wt';
SELECT rows_in, rows_out, max_pending <= (SELECT max(c) FROM (SELECT count(*) AS c FROM wsrc_t GROUP BY part, ord) AS g) + 1 AS pending_is_a_peer_group
FROM sift._test_window_stats();

-- a window of a function the machine does not run, and the limits, are refused with a message
SELECT sift._test_window('{1}', '{1}', '{1}', '{1}', ARRAY['nth_value']);
SELECT sift._test_window('{1}', '{1}', '{1}', '{1}', ARRAY['lag:arg:100000']);
SELECT sift._test_window('{1}', '{1}', '{1}', '{1}', ARRAY['row_number'], topk => 1, mode => 'first');
SELECT sift._test_window('{1}', '{1}', '{1}', '{1}', ARRAY['row_number', 'lead:arg:1'], topk => 1);
SELECT sift._test_window('{1}', '{1}', '{1}', '{1}', ARRAY['row_number', 'count:*'], topk => 1);

-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- Vectorized expressions: the result of each row, or its error, is PostgreSQL's
-- ---------------------------------------------------------------------------------------------------------------------------------------------------
-- 144 rows: every pair of 12 edge values of an int8 (the i and the j), the edge values of an int4 (k), of two float8 (x and y), a bool (m), a jsonb (d)
CREATE TABLE vd AS
SELECT g + 1 AS id,
       (ARRAY[0, 1, -1, 2, -2, 7, 100, 3037000500, 9223372036854775807, -9223372036854775808, 4611686018427387904, NULL]::int8[])[g % 12 + 1] AS i,
       (ARRAY[0, 1, -1, 2, -2, 7, 100, 3037000500, 9223372036854775807, -9223372036854775808, 4611686018427387904, NULL]::int8[])[(g / 12) % 12 + 1] AS j,
       (ARRAY[0, 1, -1, 2, -2, 46341, 2147483647, -2147483648, NULL]::int4[])[(g * 7) % 9 + 1] AS k,
       (ARRAY[0, -0.0, 1.5, -2.5, 'Infinity', '-Infinity', 'NaN', NULL, 1e308, -1e308, 1e-320, 5e-324]::float8[])[(g * 5) % 12 + 1] AS x,
       (ARRAY[0, -0.0, 1.5, -2.5, 'Infinity', '-Infinity', 'NaN', NULL, 1e308, -1e308, 1e-320, 5e-324]::float8[])[(g / 12 + g) % 12 + 1] AS y,
       (ARRAY[true, false, NULL])[g % 3 + 1] AS m,
       (ARRAY['{"a": 1}', '{"b": 2}', '{"a": 1, "b": 2}', '{}', NULL, '[]']::jsonb[])[(g / 3) % 6 + 1] AS d
FROM generate_series(0, 143) AS g;
CREATE TABLE vda AS
SELECT array_agg(i ORDER BY id) AS i, array_agg(j ORDER BY id) AS j, array_agg(k ORDER BY id) AS k, array_agg(x ORDER BY id) AS x,
       array_agg(y ORDER BY id) AS y, array_agg(m ORDER BY id) AS m, array_agg(d ORDER BY id) AS d FROM vd;

-- the engine over the rows of `live` (all rows when NULL) as the (row, text) PostgreSQL gives for the SQL expression
CREATE FUNCTION vexpr_run(prog text, live int[]) RETURNS SETOF record LANGUAGE sql AS $$
	SELECT e.ord, e.res FROM vda, sift._test_vexpr(prog, i, j, k, x, y, m, d,
	       CASE WHEN live IS NULL THEN NULL ELSE (SELECT array_agg(id = ANY (live) ORDER BY id) FROM vd) END) AS e $$;

-- the number of rows, one by one and all together, where the engine and PostgreSQL differ in the result or the error
CREATE FUNCTION vdiff(prog text, pgx text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE
	r int; e text; p text; bad bigint := 0; ok int[] := '{}'; n bigint; nrows int := (SELECT count(*) FROM vd); clean bool;
BEGIN
	BEGIN
		EXECUTE format('SELECT count((%s)::text) FROM vd', pgx);
		clean := true;
		ok := ARRAY(SELECT id FROM vd ORDER BY id);
	EXCEPTION WHEN others THEN clean := false;
	END;
	EXECUTE format('PREPARE vdq(int) AS SELECT coalesce((%s)::text, ''<null>'') FROM vd WHERE id = $1', pgx);
	FOR r IN SELECT id FROM vd WHERE NOT clean OR id % 29 = 0 ORDER BY id LOOP
		BEGIN
			EXECUTE format('EXECUTE vdq(%s)', r) INTO p;
			IF NOT clean THEN ok := ok || r; END IF;
		EXCEPTION WHEN others THEN p := 'error: ' || SQLERRM;
		END;
		CONTINUE WHEN r % 29 <> 0 AND p NOT LIKE 'error: %';
		BEGIN
			e := coalesce((SELECT t.res FROM vexpr_run(prog, ARRAY[r]) AS t(ord int, res text) WHERE t.ord = r), '<null>');
		EXCEPTION WHEN others THEN e := 'error: ' || SQLERRM;
		END;
		IF e IS DISTINCT FROM p THEN
			bad := bad + 1;
			IF bad <= 3 THEN RAISE NOTICE '% row %: engine %, PostgreSQL %', prog, r, e, p; END IF;
		END IF;
	END LOOP;
	DEALLOCATE vdq;
	EXECUTE format('SELECT count(*) FROM (((SELECT ord, res FROM vexpr_run(%L, %L) AS t(ord int, res text)) EXCEPT ALL (SELECT id, (%s)::text FROM vd WHERE id = ANY (%L))) UNION ALL ((SELECT id, (%s)::text FROM vd WHERE id = ANY (%L)) EXCEPT ALL (SELECT ord, res FROM vexpr_run(%L, %L) AS t(ord int, res text)))) AS d',
	               prog, CASE WHEN cardinality(ok) = nrows THEN NULL ELSE ok END, pgx, ok, pgx, ok, prog, CASE WHEN cardinality(ok) = nrows THEN NULL ELSE ok END) INTO n;
	RETURN bad + n;
END $$;

CREATE TABLE vprog (prog text PRIMARY KEY, pgx text NOT NULL);
INSERT INTO vprog VALUES
  -- integers: overflow of int8 and int4, the division of the smallest by -1, by 0, NULLs
  ('(add i j)', 'i + j'), ('(sub i j)', 'i - j'), ('(mul i j)', 'i * j'), ('(div i j)', 'i / j'), ('(neg i)', '-i'), ('(abs i)', 'abs(i)'),
  ('(add i 1i8)', 'i + 1'), ('(sub 0i8 i)', '0 - i'), ('(mul i 3i8)', 'i * 3'), ('(div i 2i8)', 'i / 2'), ('(div i -1i8)', 'i / (-1)'),
  ('(div 100i8 j)', '100 / j'), ('(add i nulli8)', 'i + NULL::int8'), ('(mul (add i 1i8) (sub j 1i8))', '(i + 1) * (j - 1)'),
  ('(add k k)', 'k + k'), ('(sub k 1i4)', 'k - 1'), ('(mul k 2i4)', 'k * 2'), ('(div k k)', 'k / k'), ('(div k -1i4)', 'k / (-1)'), ('(neg k)', '-k'),
  ('(abs k)', 'abs(k)'), ('(add (cast_i8 k) i)', 'k::int8 + i'), ('(mul (cast_i8 k) (cast_i8 k))', 'k::int8 * k::int8'),
  -- floats: the edge values, the errors of overflow, underflow and the division by 0
  ('(add x y)', 'x + y'), ('(sub x y)', 'x - y'), ('(mul x y)', 'x * y'), ('(div x y)', 'x / y'), ('(neg x)', '-x'), ('(abs x)', 'abs(x)'),
  ('(add x 1.5f8)', 'x + 1.5::float8'), ('(mul x 2.5f8)', 'x * 2.5::float8'), ('(div x 0.0f8)', 'x / 0.0::float8'), ('(add x 1e308f8)', 'x + 1e308::float8'),
  ('(mul x 1e-320f8)', 'x * 1e-320::float8'), ('(add x inff8)', 'x + ''Infinity''::float8'), ('(sub x nanf8)', 'x - ''NaN''::float8'),
  ('(cast_f8 i)', 'i::float8'), ('(cast_f8 k)', 'k::float8'), ('(mul (cast_f8 k) x)', 'k::float8 * x'), ('(div (cast_f8 i) y)', 'i::float8 / y'),
  -- comparisons: NaN is equal to itself and above every number, -0 is 0, NULL is NULL
  ('(eq i j)', 'i = j'), ('(ne i j)', 'i <> j'), ('(lt i j)', 'i < j'), ('(le i j)', 'i <= j'), ('(gt i j)', 'i > j'), ('(ge i j)', 'i >= j'),
  ('(eq k 2i4)', 'k = 2'), ('(lt k 0i4)', 'k < 0'), ('(ge k 46341i4)', 'k >= 46341'), ('(gt i 9223372036854775807i8)', 'i > 9223372036854775807'),
  ('(eq x y)', 'x = y'), ('(ne x y)', 'x <> y'), ('(lt x y)', 'x < y'), ('(le x y)', 'x <= y'), ('(gt x y)', 'x > y'), ('(ge x y)', 'x >= y'),
  ('(eq x nanf8)', 'x = ''NaN''::float8'), ('(lt x nanf8)', 'x < ''NaN''::float8'), ('(gt x nanf8)', 'x > ''NaN''::float8'), ('(eq x 0.0f8)', 'x = 0.0::float8'),
  ('(lt x inff8)', 'x < ''Infinity''::float8'), ('(eq i nulli8)', 'i = NULL::int8'),
  -- three-valued logic
  ('(and m (gt i 0i8))', 'm AND i > 0'), ('(or m (gt i 0i8))', 'm OR i > 0'), ('(not m)', 'NOT m'), ('(and (gt i 0i8) (lt j 0i8))', 'i > 0 AND j < 0'),
  ('(or (gt i 0i8) (lt j 0i8))', 'i > 0 OR j < 0'), ('(not (and m (gt x 0.0f8)))', 'NOT (m AND x > 0.0::float8)'), ('(and (and m m) (not m))', '(m AND m) AND NOT m'),
  ('(isnull i)', 'i IS NULL'), ('(isnotnull x)', 'x IS NOT NULL'), ('(isnull (add i j))', '(i + j) IS NULL'), ('(not (isnull k))', 'NOT (k IS NULL)'),
  ('(has a)', 'jsonb_exists(d, ''a'')'), ('(has b)', 'jsonb_exists(d, ''b'')'), ('(and (has a) (has b))', 'jsonb_exists(d, ''a'') AND jsonb_exists(d, ''b'')'),
  ('(or (has a) m)', 'jsonb_exists(d, ''a'') OR m'), ('(not (has b))', 'NOT jsonb_exists(d, ''b'')'), ('(and (has a) (gt i 0i8))', 'jsonb_exists(d, ''a'') AND i > 0'),
  -- an error only for the rows PostgreSQL evaluates: the right side of an AND where the left is not false, of an OR where it is not true, a COALESCE
  -- argument where the ones before are NULL, a CASE branch of the first condition that is true
  ('(and (ne j 0i8) (gt (div i j) 1i8))', 'j <> 0 AND i / j > 1'), ('(or (eq j 0i8) (gt (div i j) 1i8))', 'j = 0 OR i / j > 1'),
  ('(and (gt (div i j) 1i8) (ne j 0i8))', 'i / j > 1 AND j <> 0'), ('(and m (gt (div i j) 1i8))', 'm AND i / j > 1'), ('(or m (gt (div i j) 1i8))', 'm OR i / j > 1'),
  ('(case (when (ne j 0i8) (div i j)) (else 0i8))', 'CASE WHEN j <> 0 THEN i / j ELSE 0 END'),
  ('(case (when (eq j 0i8) 0i8) (else (div i j)))', 'CASE WHEN j = 0 THEN 0 ELSE i / j END'),
  ('(case (when (gt i 0i8) (add i 1i8)) (when (lt i 0i8) (sub i 1i8)) (else 0i8))', 'CASE WHEN i > 0 THEN i + 1 WHEN i < 0 THEN i - 1 ELSE 0 END'),
  ('(case (when (gt x 0.0f8) x))', 'CASE WHEN x > 0.0::float8 THEN x END'), ('(case (when m x) (else y))', 'CASE WHEN m THEN x ELSE y END'),
  ('(case (when (isnull i) j) (else i))', 'CASE WHEN i IS NULL THEN j ELSE i END'), ('(case (when (has a) k) (else 0i4))', 'CASE WHEN jsonb_exists(d, ''a'') THEN k ELSE 0 END'),
  ('(coalesce i j)', 'COALESCE(i, j)'), ('(coalesce i j 5i8)', 'COALESCE(i, j, 5)'), ('(coalesce x y 0.0f8)', 'COALESCE(x, y, 0.0::float8)'),
  ('(coalesce k 0i4)', 'COALESCE(k, 0)'), ('(coalesce i (div 1i8 (sub j j)))', 'COALESCE(i, 1 / (j - j))'), ('(coalesce (div i j) 0i8)', 'COALESCE(i / j, 0)'),
  ('(coalesce nulli8 i)', 'COALESCE(NULL::int8, i)'), ('(add (coalesce i 0i8) (case (when m 1i8) (else 2i8)))', 'COALESCE(i, 0) + CASE WHEN m THEN 1 ELSE 2 END');

SELECT count(*) AS programs, count(*) FILTER (WHERE bad > 0) AS differ FROM (SELECT vdiff(prog, pgx) AS bad FROM vprog) AS t;

-- the messages of the errors are PostgreSQL's
CREATE FUNCTION vexpr_error(prog text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM * FROM vexpr_run(prog, NULL) AS t(ord int, res text);
	RETURN 'no error';
EXCEPTION WHEN others THEN RETURN SQLERRM;
END $$;
SELECT prog, vexpr_error(prog) AS error
FROM unnest(ARRAY['(add i j)', '(neg i)', '(abs i)', '(div i j)', '(div k k)', '(mul k 2i4)', '(mul x x)', '(div x y)', '(mul x 1e-320f8)', '(add x y)',
                  '(case (when (ne j 0i8) (div i j)) (else 0i8))']) AS prog ORDER BY prog;

-- no row is live: nothing is evaluated, nothing fails
SELECT count(*) AS rows_out FROM vexpr_run('(div i j)', '{}') AS t(ord int, res text);

-- the programs the machine does not run are refused when they are compiled, none while they are evaluated
SELECT prog, sift._test_vexpr_compiles(prog) AS runs
FROM unnest(ARRAY['(add i j)', '(case (when m i) (else j))', '(coalesce m m)', '(case (when m m) (else m))', '(add i k)', '(eq i k)', '(isnull m)',
                  '(add x i)', '(coalesce)', '(coalesce m)', '(neg)', '(neg i i)', '(add i)', '(eq i)', '(isnull)']) AS prog ORDER BY prog;

-- generated programs of depth 3 over the same columns, from a generator of its own (not random())
CREATE FUNCTION lcg() RETURNS int LANGUAGE plpgsql AS $$
DECLARE s bigint := current_setting('vgen.seed')::bigint;
BEGIN
	s := (s * 1103515245 + 12345) % 2147483648;
	PERFORM set_config('vgen.seed', s::text, false);
	RETURN (s / 65536)::int;
END $$;
-- (program, SQL); ty is i (int8), f (float8) or b (boolean); force: the expression has a column, so that PostgreSQL does not fold a constant error;
-- no NULL constant, which PostgreSQL folds a strict function of to NULL before the argument that would fail is evaluated
CREATE FUNCTION vgen(ty text, d int, force bool DEFAULT false) RETURNS text[] LANGUAGE plpgsql AS $$
DECLARE
	r int := lcg() % 100; a text[]; b text[]; c text[]; o int;
	iconst text[] := ARRAY['0i8', '1i8', '-1i8', '7i8', '9223372036854775807i8'];
	iconst_sql text[] := ARRAY['(0)::int8', '(1)::int8', '(-1)::int8', '(7)::int8', '(9223372036854775807)::int8'];
	fconst text[] := ARRAY['0.0f8', '1.5f8', '-2.5f8', 'inff8', 'nanf8', '1e308f8'];
	fconst_sql text[] := ARRAY['(0.0)::float8', '(1.5)::float8', '(-2.5)::float8', '''Infinity''::float8', '''NaN''::float8', '(1e308)::float8'];
BEGIN
	IF ty = 'i' THEN
		IF d = 0 OR r < 20 THEN
			IF force OR r < 12 THEN o := lcg() % 3; RETURN CASE o WHEN 0 THEN ARRAY['i', 'i'] WHEN 1 THEN ARRAY['j', 'j'] ELSE ARRAY['(cast_i8 k)', 'k::int8'] END; END IF;
			o := lcg() % 5; RETURN ARRAY[iconst[o + 1], iconst_sql[o + 1]];
		END IF;
		a := vgen('i', d - 1, true); b := vgen('i', d - 1); o := lcg() % 9;
		IF o < 4 THEN
			RETURN ARRAY['(' || (ARRAY['add', 'sub', 'mul', 'div'])[o + 1] || ' ' || a[1] || ' ' || b[1] || ')', '(' || a[2] || ' ' || (ARRAY['+', '-', '*', '/'])[o + 1] || ' ' || b[2] || ')'];
		ELSIF o = 4 THEN RETURN ARRAY['(neg ' || a[1] || ')', '(-' || a[2] || ')'];
		ELSIF o = 5 THEN RETURN ARRAY['(abs ' || a[1] || ')', 'abs(' || a[2] || ')'];
		ELSIF o = 6 THEN RETURN ARRAY['(coalesce ' || a[1] || ' ' || b[1] || ')', 'COALESCE(' || a[2] || ', ' || b[2] || ')'];
		END IF;
		c := vgen('b', d - 1, true);
		RETURN ARRAY['(case (when ' || c[1] || ' ' || a[1] || ') (else ' || b[1] || '))', '(CASE WHEN ' || c[2] || ' THEN ' || a[2] || ' ELSE ' || b[2] || ' END)'];
	ELSIF ty = 'f' THEN
		IF d = 0 OR r < 20 THEN
			IF force OR r < 12 THEN o := lcg() % 3; RETURN CASE o WHEN 0 THEN ARRAY['x', 'x'] WHEN 1 THEN ARRAY['y', 'y'] ELSE ARRAY['(cast_f8 i)', 'i::float8'] END; END IF;
			o := lcg() % 6; RETURN ARRAY[fconst[o + 1], fconst_sql[o + 1]];
		END IF;
		a := vgen('f', d - 1, true); b := vgen('f', d - 1); o := lcg() % 9;
		IF o < 4 THEN
			RETURN ARRAY['(' || (ARRAY['add', 'sub', 'mul', 'div'])[o + 1] || ' ' || a[1] || ' ' || b[1] || ')', '(' || a[2] || ' ' || (ARRAY['+', '-', '*', '/'])[o + 1] || ' ' || b[2] || ')'];
		ELSIF o = 4 THEN RETURN ARRAY['(neg ' || a[1] || ')', '(-' || a[2] || ')'];
		ELSIF o = 5 THEN RETURN ARRAY['(abs ' || a[1] || ')', 'abs(' || a[2] || ')'];
		ELSIF o = 6 THEN RETURN ARRAY['(coalesce ' || a[1] || ' ' || b[1] || ')', 'COALESCE(' || a[2] || ', ' || b[2] || ')'];
		END IF;
		c := vgen('b', d - 1, true);
		RETURN ARRAY['(case (when ' || c[1] || ' ' || a[1] || ') (else ' || b[1] || '))', '(CASE WHEN ' || c[2] || ' THEN ' || a[2] || ' ELSE ' || b[2] || ' END)'];
	END IF;
	IF d = 0 OR r < 25 THEN
		o := lcg() % 3;
		RETURN CASE o WHEN 0 THEN ARRAY['m', 'm'] WHEN 1 THEN ARRAY['(has a)', 'jsonb_exists(d, ''a'')'] ELSE ARRAY['(has b)', 'jsonb_exists(d, ''b'')'] END;
	END IF;
	o := lcg() % 6;
	IF o = 0 THEN
		a := vgen('i', d - 1, true); b := vgen('i', d - 1); o := lcg() % 6;
		RETURN ARRAY['(' || (ARRAY['eq', 'ne', 'lt', 'le', 'gt', 'ge'])[o + 1] || ' ' || a[1] || ' ' || b[1] || ')', '(' || a[2] || ' ' || (ARRAY['=', '<>', '<', '<=', '>', '>='])[o + 1] || ' ' || b[2] || ')'];
	ELSIF o = 1 THEN
		a := vgen('f', d - 1, true); b := vgen('f', d - 1); o := lcg() % 6;
		RETURN ARRAY['(' || (ARRAY['eq', 'ne', 'lt', 'le', 'gt', 'ge'])[o + 1] || ' ' || a[1] || ' ' || b[1] || ')', '(' || a[2] || ' ' || (ARRAY['=', '<>', '<', '<=', '>', '>='])[o + 1] || ' ' || b[2] || ')'];
	ELSIF o = 2 THEN
		a := vgen('b', d - 1, true); b := vgen('b', d - 1); o := lcg() % 2;
		RETURN ARRAY['(' || (ARRAY['and', 'or'])[o + 1] || ' ' || a[1] || ' ' || b[1] || ')', '(' || a[2] || ' ' || (ARRAY['AND', 'OR'])[o + 1] || ' ' || b[2] || ')'];
	ELSIF o = 3 THEN
		a := vgen('b', d - 1, true); RETURN ARRAY['(not ' || a[1] || ')', '(NOT ' || a[2] || ')'];
	END IF;
	a := vgen(CASE WHEN lcg() % 2 = 0 THEN 'i' ELSE 'f' END, d - 1, true);
	RETURN ARRAY['(isnull ' || a[1] || ')', '(' || a[2] || ' IS NULL)'];
END $$;

CREATE TABLE vgenerated (n int PRIMARY KEY, prog text, pgx text);
SELECT set_config('vgen.seed', '20260101', false) IS NOT NULL AS seeded;
INSERT INTO vgenerated SELECT n, g[1], g[2] FROM (SELECT n, vgen((ARRAY['i', 'f', 'b'])[n % 3 + 1], 3) AS g FROM generate_series(1, 90) AS n) AS t;
SELECT count(*) AS generated, count(DISTINCT prog) > 60 AS varied, count(*) FILTER (WHERE sift._test_vexpr_compiles(prog)) AS compile FROM vgenerated;
SELECT count(*) AS programs, count(*) FILTER (WHERE bad > 0) AS differ FROM (SELECT vdiff(prog, pgx) AS bad FROM vgenerated) AS t;

-- a peer group that outgrows work_mem goes to a file and comes back in order: the results of the RANGE frames are PostgreSQL's, the ring holds a few rows
CREATE TABLE wsrc_b AS
SELECT g AS id, (g / 12000)::int8 AS part, ((g % 12000) / 4000 * 10)::int8 AS ord, (g * 7919 % 1000)::int8 AS arg, (g % 97)::float8 AS argf FROM generate_series(0, 35999) AS g;
INSERT INTO wd SELECT 'wb', array_agg(part ORDER BY id), array_agg(ord ORDER BY id), array_agg(arg ORDER BY id), array_agg(argf ORDER BY id), array_agg(true ORDER BY id) FROM wsrc_b;
CREATE TABLE bcase (label text PRIMARY KEY, funcs text, pgcols text, tail text DEFAULT '');
INSERT INTO bcase (label, funcs, pgcols, tail) VALUES
  ('range', 'rank,dense_rank,count:*,sum:arg,min:argf,max:arg,avg:argf', '(rank() OVER w)::text, (dense_rank() OVER w)::text, (count(*) OVER w)::text, (sum(arg) OVER w)::text, (min(argf) OVER w)::text, (max(arg) OVER w)::text, (avg(argf) OVER w)::text', ''),
  ('range counts and sums', 'count:arg,sum:argf,min:arg,max:argf', '(count(arg) OVER w)::text, (sum(argf) OVER w)::text, (min(arg) OVER w)::text, (max(argf) OVER w)::text', ''),
  ('range with a limit', 'rank,count:*', '(rank() OVER w)::text, (count(*) OVER w)::text', 'ORDER BY tag OFFSET 3000 LIMIT 6000');
SET work_mem = '256kB';
SELECT label, diff(format('SELECT e.tag, e.res FROM wd, sift._test_window(p, o, a, f, %L::text[], batch_rows => 700 %s) e WHERE name = ''wb''', '{' || funcs || '}',
                          CASE WHEN tail = '' THEN '' ELSE ', off => 3000, lim => 6000' END),
                   format('SELECT tag, res FROM (SELECT tag, part, ord, ARRAY[%s]::text[] AS res FROM wrows(''wb'') WINDOW w AS (PARTITION BY part ORDER BY ord)) q %s', pgcols, tail)) AS wrong
FROM bcase ORDER BY label;
SELECT count(*) AS rows_out, count(*) FILTER (WHERE tag <> prev + 1) AS out_of_order
FROM (SELECT e.tag, lag(e.tag) OVER () AS prev FROM wd, sift._test_window(p, o, a, f, ARRAY['count:*', 'sum:arg']) AS e WHERE name = 'wb') AS q;
SELECT rows_out, max_pending < 1000 AS ring_stays_small FROM sift._test_window_stats();
-- a LEAD looks ahead into the ring, which a peer group of that size does not fit
DO $$
BEGIN
	PERFORM count(*) FROM wd, sift._test_window(p, o, a, f, ARRAY['lead:arg:1', 'count:*']) AS e WHERE name = 'wb';
EXCEPTION WHEN program_limit_exceeded THEN RAISE NOTICE 'the frame does not fit work_mem';
END $$;
RESET work_mem;
SELECT count(*) AS rows_out FROM wd, sift._test_window(p, o, a, f, ARRAY['lead:arg:1', 'count:*']) AS e WHERE name = 'wb';

-- the first granule of the tail tie run: the granules before it hold no row equal to the last row, NULLs last
SELECT count(*) AS cases, count(*) FILTER (WHERE sift._test_window_tail_tie(vals::int8[], gr) IS DISTINCT FROM brute) AS wrong
FROM (SELECT vals, gr, (SELECT min((i - 1) / gr) FROM unnest(vals) WITH ORDINALITY AS u(v, i) WHERE v IS NOT DISTINCT FROM vals[cardinality(vals)])::int AS brute
      FROM (SELECT (SELECT array_agg(CASE WHEN x > 0 AND x % 7 = 0 THEN NULL ELSE x * s % 11 END ORDER BY CASE WHEN x > 0 AND x % 7 = 0 THEN 1 ELSE 0 END, x * s % 11)
                    FROM generate_series(1, n) AS x) AS vals, gr
            FROM generate_series(1, 40) AS n, generate_series(1, 4) AS s, generate_series(1, 9) AS gr) AS t) AS c;

DROP TABLE sip_fact, sip_dim, sip_in, wsrc_u, wsrc_t, wsrc_p, wsrc_g, wsrc_b, bcase, wd, wcase, dcase, vd, vda, vprog, vgenerated;
DROP FUNCTION diff(text, text), wrows(text), vexpr_run(text, int[]), vdiff(text, text), vexpr_error(text), lcg(), vgen(text, int, bool);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
