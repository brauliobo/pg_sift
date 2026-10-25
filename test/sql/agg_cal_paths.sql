-- The calendar cells of a date or timestamp column are written beside jsonb paths when calendar_columns names them (not by default there), and a unit whose date has too many days for a histogram answers a range from its cells:
-- a comparison, an AND or an OR of them is decided cell by cell by the cell's span (inside it passes, outside it does not, a bound that cuts a cell leaves the unit to
-- a scan). Every shape is checked against the plain plan.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET cpu_tuple_cost = 0.5;
\ir test/agg_preagg_units.sql

-- the first half of the rows spans 3,000 days in every unit (no histogram of d), the second 20 days (a histogram); ts has NULLs and spans months
CREATE TABLE ct (id int, d date, ts timestamp, doc jsonb, v int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE ct ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ct SELECT i, CASE WHEN i <= 15000 THEN date '2000-01-01' + (i * 7) % 3000 ELSE date '2012-03-01' + i % 20 END,
	CASE WHEN i % 17 = 0 THEN NULL ELSE timestamp '2005-01-01 00:00:00' + (i * 37) % 900 * interval '1 day' + (i % 24) * interval '1 hour' END,
	jsonb_build_object('k', i % 9, 's', 'x' || i % 4), i % 100, repeat('x', 200) FROM generate_series(1, 30000) i;
VACUUM (FREEZE, ANALYZE) ct;
CREATE INDEX ct_col ON ct USING sift_col (id, d, ts, v, doc) WITH (paths = 'doc.k int4, doc.s text', calendar_columns = 'all');

CREATE FUNCTION ca_rows(q text, push bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', push::text, true);
	PERFORM set_config(s, (NOT push)::text, true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	EXECUTE format('SELECT coalesce(string_agg(x::text, E''\n'' ORDER BY x::text), ''no rows'') FROM (%s) x', q) INTO r;
	RETURN r;
END $$;

-- 120 shapes: one to three ranges on d or ts, joined by AND or OR, with the four operators, a count with or without a sum, by month or not
CREATE TABLE ca_q (n int, q text);
DO $$
DECLARE
	ops text[] := ARRAY['>=', '>', '<=', '<'];
	c text; lo1 text; lo2 text; hi1 text; hi2 text; w text; sel text;
BEGIN
	FOR n IN 1..120 LOOP
		IF n % 4 = 3 THEN
			c := 'ts';
			lo1 := (timestamp '2005-01-01' + (n * 53 % 800) * interval '1 day')::text;
			hi1 := (timestamp '2005-01-01' + (n * 53 % 800 + n * 13 % 200) * interval '1 day')::text;
			lo2 := (timestamp '2005-01-01' + (n * 97 % 800) * interval '1 day')::text;
			hi2 := (timestamp '2005-01-01' + (n * 97 % 800 + n * 7 % 300) * interval '1 day')::text;
		ELSE
			c := 'd';
			lo1 := (date '2000-01-01' + (n * 131) % 3000)::text;
			hi1 := (date '2000-01-01' + (n * 131) % 3000 + (n * 29) % 500)::text;
			lo2 := (date '2000-01-01' + (n * 71) % 3000)::text;
			hi2 := (date '2000-01-01' + (n * 71) % 3000 + (n * 41) % 700)::text;
		END IF;
		w := format('(%s %s %L AND %s %s %L)', c, ops[1 + n % 4], lo1, c, ops[1 + (n / 4) % 4], hi1);
		IF n % 5 > 0 THEN
			w := w || format(' %s (%s %s %L AND %s %s %L)', CASE WHEN n % 2 = 0 THEN 'OR' ELSE 'AND' END, c, ops[1 + (n / 3) % 4], lo2, c, ops[1 + (n / 7) % 4], hi2);
		END IF;
		IF n % 11 = 0 THEN
			w := w || format(' OR %s %s %L', c, ops[1 + n % 4], hi1);
		END IF;
		sel := CASE WHEN n % 3 = 0 THEN format('date_trunc(''month'', %s), count(*)', c) WHEN n % 3 = 1 THEN 'count(*), sum(v)' ELSE 'count(*)' END;
		INSERT INTO ca_q VALUES (n, format('SELECT %s FROM ct WHERE %s%s', sel, w, CASE WHEN n % 3 = 0 THEN ' GROUP BY 1' ELSE '' END));
	END LOOP;
END $$;
SELECT count(*) AS shapes, count(*) FILTER (WHERE ca_rows(q, true) = ca_rows(q, false)) AS same FROM ca_q;

-- the cells answer units the histogram of d cannot: a range aligned to months, an OR of ranges, and the cells of a timestamp
SELECT q.n, u.units, u.pre > 0 AS from_cells FROM (VALUES
	(1, 'SELECT count(*) FROM ct WHERE d >= ''2000-01-01'' AND d < ''2004-01-01'''),
	(2, 'SELECT count(*), sum(v) FROM ct WHERE d >= ''2000-01-01'' AND d < ''2001-01-01'' OR d >= ''2002-01-01'' AND d < ''2003-01-01'''),
	(3, 'SELECT count(*) FROM ct WHERE ts >= ''2005-01-01'' AND ts < ''2006-01-01'' OR ts >= ''2006-07-01'' AND ts < ''2007-01-01''')
) q(n, q), LATERAL pa_units(q.q) u ORDER BY q.n;

-- the cells are there beside the paths, and the answers are the plain plan's after a rebuild
SELECT sift.col_verify('ct_col', true);
REINDEX INDEX ct_col;
SELECT count(*) FILTER (WHERE ca_rows(q, true) = ca_rows(q, false)) AS same_after_reindex FROM ca_q;

-- the same on an index without paths, whose cells are written by default beside the histogram of the date
CREATE TABLE ct2 AS SELECT id, d, ts, v, pad FROM ct;
ALTER TABLE ct2 ALTER COLUMN pad SET STORAGE PLAIN;
VACUUM (FREEZE, ANALYZE) ct2;
CREATE INDEX ct2_col ON ct2 USING sift_col (id, d, ts, v);
SELECT count(*) AS shapes, count(*) FILTER (WHERE ca_rows(replace(q, 'FROM ct ', 'FROM ct2 '), true) = ca_rows(replace(q, 'FROM ct ', 'FROM ct2 '), false)) AS same FROM ca_q;
SELECT u.units, u.pre > 0 AS from_cells FROM pa_units('SELECT count(*) FROM ct2 WHERE d >= ''2000-01-01'' AND d < ''2004-01-01'' OR d >= ''2007-01-01'' AND d < ''2008-01-01''') u;

DROP TABLE ct, ct2, ca_q;
DROP FUNCTION ca_rows(text, bool), pa_units(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
