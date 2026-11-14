-- isempty(r), lower_inf(r), upper_inf(r), lower_inc(r) and upper_inc(r) of a range column in a WHERE conjunct (as the function, its NOT, `IS TRUE` and `IS FALSE`) are tests on
-- the first and last position of the range that the index keeps (col/shape.c) as the containment operators are: empty ranges, ranges with a bound that is no value, inclusive and exclusive
-- bounds of the continuous subtypes, the canonical form of the discrete ones, NULL ranges, and a bound too large for a lane (the engine keeps those rows and PostgreSQL's function decides
-- them) give the rows PostgreSQL gives. Each statement is run with the scan on and off; `scan` says the aggregate is in the scan.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE rg (id int PRIMARY KEY, i4 int4range, i8 int8range, d daterange, ts tsrange, tz tstzrange) WITH (autovacuum_enabled = false);
INSERT INTO rg
SELECT i, r.i4, r.i8, r.d, r.ts, r.tz
FROM generate_series(1, 4000) i
CROSS JOIN LATERAL (SELECT i % 20 AS k) m
CROSS JOIN LATERAL (VALUES
  (CASE m.k
     WHEN 0 THEN NULL WHEN 1 THEN 'empty'::int4range WHEN 2 THEN int4range(NULL, NULL) WHEN 3 THEN int4range(5, NULL) WHEN 4 THEN int4range(NULL, 9)
     WHEN 5 THEN int4range(i, i + 3, '[]') WHEN 6 THEN int4range(i, i + 3, '()') WHEN 7 THEN int4range(i, i, '[]') WHEN 8 THEN int4range(i, i, '[)')
     ELSE int4range(i % 97, i % 97 + 1 + i % 13) END,
   CASE m.k
     WHEN 0 THEN NULL WHEN 1 THEN 'empty'::int8range WHEN 2 THEN int8range(NULL, NULL) WHEN 3 THEN int8range(5, NULL) WHEN 4 THEN int8range(NULL, 9)
     WHEN 5 THEN int8range(i, i + 3, '(]') WHEN 6 THEN int8range(4611686018427387904, 4611686018427387999) WHEN 7 THEN int8range(-4611686018427388000, 5)
     WHEN 8 THEN int8range(-9223372036854775807, 9223372036854775806, '[]') ELSE int8range(i, i + 1 + i % 7, '[]') END,
   CASE m.k
     WHEN 0 THEN NULL WHEN 1 THEN 'empty'::daterange WHEN 2 THEN daterange(NULL, NULL) WHEN 3 THEN daterange(date '2025-01-01', NULL) WHEN 4 THEN daterange(NULL, date '2025-02-01')
     WHEN 5 THEN daterange(date '2025-01-01' + i % 50, date '2025-01-01' + i % 50, '[]') WHEN 6 THEN daterange('-infinity', 'infinity', '[]') WHEN 7 THEN daterange('2025-01-01', 'infinity')
     ELSE daterange(date '2025-01-01' + i % 90, date '2025-01-01' + i % 90 + 1 + i % 20, CASE WHEN i % 3 = 0 THEN '[]' WHEN i % 3 = 1 THEN '()' ELSE '(]' END) END,
   CASE m.k
     WHEN 0 THEN NULL WHEN 1 THEN 'empty'::tsrange WHEN 2 THEN tsrange(NULL, NULL) WHEN 3 THEN tsrange(timestamp '2025-01-01', NULL, '(]') WHEN 4 THEN tsrange(NULL, timestamp '2025-02-01', '[]')
     WHEN 5 THEN tsrange(timestamp '2025-01-01' + i * interval '1 hour', timestamp '2025-01-01' + i * interval '1 hour', '[]') WHEN 6 THEN tsrange('infinity', 'infinity', '[]')
     WHEN 7 THEN tsrange('-infinity', timestamp '2025-01-01') WHEN 8 THEN tsrange(timestamp '2025-01-01', 'infinity', '()')
     ELSE tsrange(timestamp '2025-01-01' + i * interval '1 hour', timestamp '2025-01-01' + i * interval '1 hour' + interval '3 hours',
                  CASE i % 4 WHEN 0 THEN '[]' WHEN 1 THEN '[)' WHEN 2 THEN '()' ELSE '(]' END) END,
   CASE m.k
     WHEN 0 THEN NULL WHEN 1 THEN 'empty'::tstzrange WHEN 2 THEN tstzrange(NULL, NULL) WHEN 3 THEN tstzrange(timestamptz '2025-01-01 00:00+00', NULL, '(]')
     WHEN 4 THEN tstzrange(NULL, timestamptz '2025-02-01 00:00+00', '[]') WHEN 5 THEN tstzrange(timestamptz '2025-01-01 00:00+00' + i * interval '1 hour', timestamptz '2025-01-01 00:00+00' + i * interval '1 hour', '[]')
     ELSE tstzrange(timestamptz '2025-01-01 00:00+00' + i * interval '1 hour', timestamptz '2025-01-01 00:00+00' + i * interval '1 hour' + interval '3 hours',
                    CASE i % 4 WHEN 0 THEN '[]' WHEN 1 THEN '[)' WHEN 2 THEN '()' ELSE '(]' END) END)
) r (i4, i8, d, ts, tz);
-- the continuous subtypes before 1970 (negative positions), at the ends of the lanes (past 2^62 microseconds a row has none: PostgreSQL decides it) and infinite, in the four forms of bounds
INSERT INTO rg (id, ts, tz)
SELECT 5000 + row_number() OVER (ORDER BY a.t, b.t, m.f), tsrange(a.t, b.t, m.f), tstzrange(a.t::timestamptz, b.t::timestamptz, m.f)
FROM (VALUES (timestamp '-infinity'), ('4713-01-01 BC'), ('0100-03-04'), ('1900-01-01'), ('1969-12-31 23:59:59.999999'), ('1970-01-01'), ('2025-06-01'), ('200000-01-01'), ('infinity')) a (t)
CROSS JOIN (VALUES (timestamp '4713-01-01 BC'), ('0100-03-04'), ('1900-01-01'), ('1969-12-31 23:59:59.999999'), ('1970-01-01'), ('2025-06-01'), ('200000-01-01'), ('infinity')) b (t)
CROSS JOIN (VALUES ('[]'), ('[)'), ('(]'), ('()')) m (f)
WHERE a.t < b.t OR (a.t = b.t AND m.f = '[]');
CREATE INDEX rg_col ON rg USING sift_col (id, i4, i8, d, ts, tz);
VACUUM (FREEZE, ANALYZE) rg;

CREATE TABLE q (ord serial, col text, pred text);
INSERT INTO q (col, pred) SELECT c, replace(p, '$', c) FROM unnest(ARRAY['i4', 'i8', 'd', 'ts', 'tz']) c CROSS JOIN
  unnest(ARRAY['isempty($)', 'NOT isempty($)', 'isempty($) IS TRUE', 'isempty($) IS FALSE', 'lower_inf($)', 'NOT lower_inf($)', 'lower_inf($) IS FALSE', 'upper_inf($)', 'NOT upper_inf($)',
               'upper_inf($) IS TRUE', 'lower_inc($)', 'upper_inc($)', 'lower_inc($) IS TRUE', 'upper_inc($) IS TRUE', 'NOT lower_inc($)', 'NOT upper_inc($)', 'lower_inc($) IS FALSE', 'upper_inc($) IS FALSE', 'lower_inc($) IS NOT TRUE',
               'NOT isempty($) AND NOT upper_inf($)', 'lower_inc($) AND NOT upper_inc($)', 'NOT lower_inc($) AND NOT upper_inc($)', 'isempty($) OR lower_inf($)']) p;

CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE r text;
BEGIN
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO r;
  RETURN r;
END $$;
CREATE FUNCTION unary_check(pred text) RETURNS TABLE (scan bool, same bool, n bigint) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; sel text := 'SELECT id FROM rg WHERE ' || pred; agg text := 'SELECT count(*) AS n, sum(id) AS s FROM rg WHERE ' || pred; off text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || agg LOOP plan := plan || l || E'\n'; END LOOP;
  scan := plan LIKE '%Custom Scan (SiftAgg)%';
  PERFORM set_config('sift.enable_agg', 'off', true);
  off := rows_md5(agg);
  PERFORM set_config('sift.enable_agg', 'on', true);
  same := rows_md5(agg) IS NOT DISTINCT FROM off;
  EXECUTE 'SELECT count(*) FROM rg WHERE ' || pred INTO n;
  RETURN NEXT;
END $$;
SELECT q.col, q.pred, c.* FROM q, LATERAL unary_check(q.pred) c ORDER BY q.ord;

DROP FUNCTION unary_check(text), rows_md5(text);
DROP TABLE q, rg;
DROP EXTENSION pg_sift;
