-- A test of a range, a network or a wide value that the source runs on the stored lanes (the conjunct of a WHERE) is run there as a branch of an OR, in an aggregate's FILTER, in a CASE and as
-- `IS NOT TRUE` too: the source gives the rows that pass as a bitmap of the unit and the executor tests nothing. The rows must be those of the scan off (NULL, empty and infinite
-- ranges, bounds too large for a lane, IPv6 and IPv4 networks, NULL values), and `src` says the scan counts the tests it runs in the source. The scan is forced: it is the plan being tested.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.agg_force = on;

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

CREATE TABLE wd (id int PRIMARY KEY, ip inet, n numeric(30,4), m money, iv interval) WITH (autovacuum_enabled = false);
INSERT INTO wd
SELECT i,
  CASE i % 11 WHEN 0 THEN NULL WHEN 1 THEN '203.1.2.3'::inet WHEN 2 THEN '203.1.0.0/16'::inet WHEN 3 THEN '203.0.0.0/8'::inet WHEN 4 THEN '2001:db8::1'::inet
    WHEN 5 THEN '2001:db8::/32'::inet WHEN 6 THEN '203.1.1.1/24'::inet WHEN 7 THEN '198.51.0.0/16'::inet
    ELSE ('203.' || i % 5 || '.' || i % 250 || '.' || i % 200 || '/' || 16 + i % 17)::inet END,
  CASE i % 13 WHEN 0 THEN NULL WHEN 1 THEN 5000000000000000000000.5 ELSE i::numeric * 1000000000000000000 + i % 17 END,
  CASE i % 17 WHEN 0 THEN NULL ELSE (i % 500)::numeric::money END,
  CASE i % 19 WHEN 0 THEN NULL ELSE (i % 30 || ' days')::interval + (i % 7 || ' hours')::interval END
FROM generate_series(1, 4000) i;
CREATE INDEX wd_col ON wd USING sift_col (id, ip, n, m, iv);
VACUUM (FREEZE, ANALYZE) wd;

CREATE TABLE q (ord serial, tbl text, pred text);
INSERT INTO q (tbl, pred) SELECT 'rg', replace(replace(replace(p, '$', c), '#r', r), '#e', e) FROM (VALUES
  ('i4', 'int4range(3, 40)', '17'), ('i8', 'int8range(3, 40)', '17::bigint'), ('d', 'daterange(''2025-01-10'', ''2025-02-10'')', 'date ''2025-01-20'''),
  ('ts', 'tsrange(timestamp ''2025-01-05'', timestamp ''2025-01-20'')', 'timestamp ''2025-01-08 12:00''')) t (c, r, e) CROSS JOIN
  unnest(ARRAY['isempty($)', 'NOT isempty($)', 'lower_inf($)', 'upper_inc($)', 'NOT upper_inc($)', 'lower_inc($)', 'NOT lower_inc($)', '$ && #r', '$ @> #e', '$ <@ #r',
               'isempty($) OR upper_inf($)', 'lower_inc($) AND NOT upper_inc($)']) p;
INSERT INTO q (tbl, pred) VALUES
  ('rg', 'isempty(i4) OR isempty(i8)'), ('rg', 'upper_inc(tz) AND lower_inc(ts)'), ('rg', 'NOT isempty(d) AND d && daterange(''2025-01-10'', ''2025-02-10'')'),
  ('wd', 'ip << inet ''203.1.0.0/16'''), ('wd', 'ip >>= inet ''203.1.2.3'''), ('wd', 'ip && inet ''203.0.0.0/8'''), ('wd', 'ip = inet ''203.1.1.1/24'''), ('wd', 'ip << inet ''2001:db8::/32'''),
  ('wd', 'n > 5000000000000000000000.5'), ('wd', 'n IN (1000000000000000000.0001, 2000000000000000000.0002)'), ('wd', 'n IS NULL'), ('wd', 'n IS NOT NULL'),
  ('wd', 'm > 100::numeric::money'), ('wd', 'iv > interval ''10 days'''), ('wd', 'iv IS NULL'),
  ('wd', 'n > 5000000000000000000000.5 OR ip << inet ''203.1.0.0/16'''), ('wd', 'm > 100::numeric::money AND iv > interval ''10 days''');

-- every form is run with the scan on and off, whatever the table: the rows are compared by their md5
CREATE TABLE forms (form text, tpl text);
INSERT INTO forms VALUES
  ('filter', 'SELECT count(*) AS n, count(*) FILTER (WHERE @P) AS k, sum(id) FILTER (WHERE @P) AS s FROM @T'),
  ('filter group', 'SELECT id % 5 AS g, count(*) FILTER (WHERE @P) AS k, count(*) FILTER (WHERE NOT @P) AS nk, count(*) FILTER (WHERE @P IS NOT TRUE) AS nt FROM @T GROUP BY 1'),
  ('case else 0', 'SELECT sum(CASE WHEN @P THEN id ELSE 0 END) AS s, sum(CASE WHEN @P THEN 1 ELSE 0 END) AS k FROM @T'),
  ('case else null', 'SELECT count(CASE WHEN @P THEN 1 END) AS k, max(CASE WHEN @P THEN id END) AS m FROM @T'),
  ('or', 'SELECT count(*) AS n, sum(id) AS s FROM @T WHERE @P OR id % 7 = 0'),
  ('not true', 'SELECT count(*) AS n, sum(id) AS s FROM @T WHERE @P IS NOT TRUE'),
  ('not true and', 'SELECT count(*) AS n, sum(id) AS s FROM @T WHERE @P IS NOT TRUE AND id % 3 = 0'),
  ('conjunct', 'SELECT count(*) AS n, sum(id) AS s FROM @T WHERE @P');

CREATE FUNCTION rows_md5(q text) RETURNS text LANGUAGE plpgsql AS
$$ DECLARE r text;
BEGIN
  EXECUTE 'SELECT md5(string_agg(r::text, ''|'' ORDER BY r::text)) FROM (' || q || ') r' INTO r;
  RETURN r;
END $$;
-- src: the plan is a Custom Scan (SiftAgg) that runs tests in the source (EXPLAIN ANALYZE counts them)
CREATE FUNCTION form_check(tpl text, tbl text, pred text) RETURNS TABLE (scan bool, src bool, same bool) LANGUAGE plpgsql AS
$$ DECLARE l text; plan text := ''; sql text := replace(replace(tpl, '@T', tbl), '@P', '(' || pred || ')'); off text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || sql LOOP plan := plan || l || E'\n'; END LOOP;
  scan := plan LIKE '%Custom Scan (SiftAgg)%';
  src := plan ~ '[1-9][0-9]* wide filters in source';
  PERFORM set_config('sift.enable_agg', 'off', true);
  off := rows_md5(sql);
  PERFORM set_config('sift.enable_agg', 'on', true);
  same := rows_md5(sql) IS NOT DISTINCT FROM off;
  RETURN NEXT;
END $$;
SELECT q.tbl, q.pred, f.form, c.* FROM q CROSS JOIN forms f, LATERAL form_check(f.tpl, q.tbl, q.pred) c ORDER BY q.ord, f.form;

DROP FUNCTION form_check(text, text, text), rows_md5(text);
DROP TABLE q, forms, rg, wd;
DROP EXTENSION pg_sift;
