-- A time window of a scoped aggregate that starts or ends inside a month: the whole months the window holds come from the monthly cubes of the summary, and the pieces of the months it cuts are
-- read from the rows of the key (the granules of the key, the rows the summary lacks, the rows of the members whose pairs changed), exact for count, sum, avg, min and max of every measure whatever
-- the table did since the summary: new rows, deleted rows (also those that held a minimum or maximum), rows moved in or out of the window, pair edits. Every statement runs over the facts with the
-- index and over a heap twin; each state must give the same rows, and every window with two bounds must be answered by the summary, and one with a single bound when the statement groups by month (from_summary
-- counts the 14 windows with two bounds of 3 time types and 5 shapes, and the 2 with one bound of the shape that groups by month: 216; the shape that groups by month is the join's while the pairs
-- are edited and the summary not yet refreshed: 168). A window with one bound that no month groups is the join's.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.test_create_format1 = off;
SET sift.async_maintenance = off;
SET TimeZone = 'UTC';
CREATE TABLE wp (member int NOT NULL, grp int NOT NULL, PRIMARY KEY (member, grp)) WITH (autovacuum_enabled = false);
INSERT INTO wp SELECT m, 1 + m % 6 FROM generate_series(1, 300) m;
INSERT INTO wp SELECT m, 1 + (m + 2) % 6 FROM generate_series(1, 300, 7) m;
CREATE TABLE w_rels (kind text PRIMARY KEY, fact text, heap text, at_sql text);
INSERT INTO w_rels VALUES
	('timestamp', 'wf_ts', 'wh_ts', 'TIMESTAMP ''2025-01-01'' + (%1$s %% 30000) * interval ''7 minutes'''),
	('timestamptz', 'wf_tz', 'wh_tz', 'TIMESTAMPTZ ''2025-01-01'' + (%1$s %% 30000) * interval ''7 minutes'''),
	('date', 'wf_d', 'wh_d', 'DATE ''2025-01-01'' + ((%1$s %% 30000) / 200)::int');
CREATE FUNCTION w_row(kind text, g text) RETURNS text LANGUAGE sql AS $$
	SELECT format('%1$s, 1 + (%1$s * 7919) %% 300, %2$s, ''k'' || (%1$s %% 8), CASE WHEN %1$s %% 37 = 0 THEN NULL ELSE ((%1$s * 31) %% 4000 - 1500) / 10.0 END::numeric(12,2),
		CASE WHEN %1$s %% 29 = 0 THEN NULL ELSE %1$s %% 16 - 3 END, CASE WHEN %1$s %% 41 = 0 THEN NULL ELSE (%1$s * 104729) %% 1000003 - 500000 END::bigint,
		CASE WHEN %1$s %% 23 = 0 THEN NULL ELSE ((%1$s * 13) %% 997 - 400) / 4.0 END::float8', g, format(at_sql, g)) FROM w_rels WHERE w_rels.kind = w_row.kind $$;
CREATE FUNCTION w_each(stmt text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE r w_rels;
BEGIN
	FOR r IN SELECT * FROM w_rels LOOP
		EXECUTE format(stmt, r.fact, r.kind);
		EXECUTE format(stmt, r.heap, r.kind);
	END LOOP;
END $$;
CREATE FUNCTION w_load(first int, last int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE r w_rels;
BEGIN
	FOR r IN SELECT * FROM w_rels LOOP
		EXECUTE format('INSERT INTO %s SELECT %s FROM generate_series(%s::bigint, %s::bigint) g', r.fact, w_row(r.kind, 'g'), first, last);
		EXECUTE format('INSERT INTO %s SELECT %s FROM generate_series(%s::bigint, %s::bigint) g', r.heap, w_row(r.kind, 'g'), first, last);
	END LOOP;
END $$;
CREATE TABLE wf_ts (id bigint PRIMARY KEY, member int NOT NULL, at timestamp, kind text NOT NULL, amount numeric(12,2), qty int, big bigint, f float8) WITH (autovacuum_enabled = false);
CREATE TABLE wf_tz (LIKE wf_ts INCLUDING ALL) WITH (autovacuum_enabled = false);
ALTER TABLE wf_tz ALTER COLUMN at TYPE timestamptz;
CREATE TABLE wf_d (LIKE wf_ts INCLUDING ALL) WITH (autovacuum_enabled = false);
ALTER TABLE wf_d ALTER COLUMN at TYPE date;
CREATE TABLE wh_ts (LIKE wf_ts INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE wh_tz (LIKE wf_tz INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE wh_d (LIKE wf_d INCLUDING ALL) WITH (autovacuum_enabled = false);
SELECT w_load(1, 30000);
SELECT w_each('UPDATE %1$s SET at = NULL WHERE id %% 211 = 0');
SELECT 'VACUUM (ANALYZE) ' || t FROM (SELECT fact AS t FROM w_rels UNION ALL SELECT heap FROM w_rels) s \gexec
ANALYZE wp;
CREATE INDEX wf_ts_ix ON wf_ts USING sift_col (id, member, at, kind, amount, qty, big, f) WITH (scope = 'grp: member -> wp(member, grp)');
CREATE INDEX wf_tz_ix ON wf_tz USING sift_col (id, member, at, kind, amount, qty, big, f) WITH (scope = 'grp: member -> wp(member, grp)');
CREATE INDEX wf_d_ix ON wf_d USING sift_col (id, member, at, kind, amount, qty, big, f) WITH (scope = 'grp: member -> wp(member, grp)');

CREATE TABLE w_windows (label text PRIMARY KEY, pred text NOT NULL);
INSERT INTO w_windows VALUES
	('inside a month', 'f.at >= ''2025-03-05'' AND f.at < ''2025-03-20'''),
	('all but the last day', 'f.at >= ''2025-03-01'' AND f.at < ''2025-03-31'''),
	('the first day', 'f.at >= ''2025-03-01'' AND f.at < ''2025-03-02'''),
	('the last day', 'f.at >= ''2025-03-31'' AND f.at < ''2025-04-01'''),
	('last day of February', 'f.at >= ''2025-02-28'' AND f.at <= ''2025-02-28'''),
	('whole month', 'f.at >= ''2025-03-01'' AND f.at < ''2025-04-01'''),
	('across two months', 'f.at >= ''2025-02-20'' AND f.at < ''2025-03-10'''),
	('across five months', 'f.at >= ''2025-01-15'' AND f.at < ''2025-05-20'''),
	('whole months and one edge', 'f.at >= ''2025-02-01'' AND f.at < ''2025-04-12'''),
	('whole months, the first day out', 'f.at > ''2025-01-31'' AND f.at <= ''2025-03-31'''),
	('a few hours', 'f.at >= ''2025-03-05 13:17:11'' AND f.at < ''2025-03-06 02:00:00'''),
	('empty', 'f.at >= ''2025-03-10'' AND f.at < ''2025-03-10'''),
	('reversed', 'f.at >= ''2025-03-20'' AND f.at < ''2025-03-10'''),
	('after every row', 'f.at >= ''2031-01-05'' AND f.at < ''2031-02-17'''),
	('open end', 'f.at >= ''2025-03-05'''),
	('open start', 'f.at < ''2025-03-05''');

CREATE TABLE w_shapes (n int PRIMARY KEY, sel text NOT NULL, grp text NOT NULL);
INSERT INTO w_shapes VALUES
	(1, 'count(*), sum(f.amount), avg(f.amount)', ''),
	(2, 'min(f.amount), max(f.amount), min(f.qty), max(f.qty), count(f.qty)', ''),
	(3, 'sum(f.big), min(f.big), max(f.big), count(f.big), avg(f.big)', ''),
	(4, 'sum(f.f), min(f.f), max(f.f), sum(f.qty)', ''),
	(5, 'date_trunc(''month'', f.at), count(*), sum(f.qty), min(f.amount), max(f.big)', ' GROUP BY 1');

CREATE FUNCTION w_query(rel text, key int, win w_windows, sh w_shapes) RETURNS text LANGUAGE sql AS $$
	SELECT format('SELECT %s FROM %s f JOIN wp p ON p.member = f.member WHERE p.grp = %s AND %s%s', sh.sel, rel, key, win.pred, sh.grp) $$;
CREATE FUNCTION w_hash(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE h text;
BEGIN
	EXECUTE 'SELECT md5(coalesce(string_agg(r::text, ''|'' ORDER BY r::text), '''')) FROM (' || q || ') r' INTO h;
	RETURN h;
END $$;

CREATE TABLE w_result (phase text, queries int, mismatches int, plan_surprises int);
CREATE FUNCTION w_check(phase text, p int, edited boolean) RETURNS void LANGUAGE plpgsql AS $$
DECLARE
	rels w_rels[] := ARRAY(SELECT w_rels FROM w_rels ORDER BY kind);
	shapes w_shapes[] := ARRAY(SELECT w_shapes FROM w_shapes ORDER BY n);
	keys int[] := ARRAY[1, 2, 5, 7];
	win w_windows; r w_rels; sh w_shapes; key int; q text; a text; b text; pl text; l text;
	n int := 0; bad int := 0; surprises int := 0;
BEGIN
	FOR win IN SELECT * FROM w_windows ORDER BY label LOOP
		r := rels[1 + (n + p) % 3];
		sh := shapes[1 + (n + p) % 5];
		key := CASE WHEN edited AND sh.n = 5 THEN 2 ELSE keys[1 + (n * 3 + p) % 4] END;
		q := w_query(r.fact, key, win, sh);
		n := n + 1;
		SET LOCAL sift.enable_agg = on;
		a := w_hash(q);
		pl := '';
		FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP pl := pl || l; END LOOP;
		SET LOCAL sift.enable_agg = off;
		b := w_hash(w_query(r.heap, key, win, sh));
		IF a IS DISTINCT FROM b THEN
			bad := bad + 1;
			RAISE WARNING 'DIFFERENT % %', phase, q;
		END IF;
		surprises := surprises + ((pl LIKE '%Scope Read: summary%') <> (win.label NOT IN ('open end', 'open start') AND NOT (edited AND sh.n = 5)))::int;
	END LOOP;
	INSERT INTO w_result VALUES (phase, n, bad, surprises);
END $$;

SELECT w_check('clean', 0, false);
-- rows written since the summary: a slab and new parts, in the months the windows cut
SELECT w_load(30001, 33000);
SELECT w_check('rows inserted', 1, false);
-- rows that held an extreme of a window are deleted
SELECT w_each('DELETE FROM %1$s WHERE id %% 7 = 3 OR qty = 12 OR amount > 240');
SELECT w_check('rows deleted', 2, false);
-- rows move in and out of the windows, and take other extremes
SELECT w_each('UPDATE %1$s SET at = at + 17 * interval ''1 day'', amount = amount + 500 WHERE id %% 19 = 4');
SELECT w_each('UPDATE %1$s SET at = at - 25 * interval ''1 day'', big = big + 9999999, qty = qty - 40 WHERE id %% 23 = 5');
SELECT w_check('rows moved', 3, false);
-- the pairs change: members join keys, leave them and move
DELETE FROM wp WHERE member % 50 = 11;
WITH d AS (DELETE FROM wp WHERE member % 50 = 3 RETURNING member) INSERT INTO wp SELECT member, 1 + (member * 7 + 5) % 6 FROM d ON CONFLICT DO NOTHING;
INSERT INTO wp SELECT m, 1 + (m + 1) % 6 FROM generate_series(2, 300, 13) m ON CONFLICT DO NOTHING;
SELECT w_check('pairs edited', 4, true);
SELECT w_each('DELETE FROM %1$s WHERE id %% 11 = 6');
SELECT w_load(33001, 34500);
SELECT w_check('pairs edited, rows deleted and inserted', 5, true);
SELECT 'VACUUM ' || t FROM (SELECT fact AS t FROM w_rels UNION ALL SELECT heap FROM w_rels) s \gexec
SELECT w_check('vacuumed', 6, false);
SELECT w_each('UPDATE %1$s SET amount = amount - 700, f = f + 1e4 WHERE id %% 13 = 2 AND at >= ''2025-03-05''');
DELETE FROM wp WHERE member % 40 = 7;
SELECT w_check('updated again, pairs edited again', 7, true);
SELECT * FROM w_result;
SELECT sum(mismatches) AS total_mismatches FROM w_result;
DROP FUNCTION w_row(text, text), w_each(text), w_load(int, int), w_query(text, int, w_windows, w_shapes), w_hash(text), w_check(text, int, boolean);
DROP TABLE wf_ts, wf_tz, wf_d, wh_ts, wh_tz, wh_d, wp, w_rels, w_windows, w_shapes, w_result;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
