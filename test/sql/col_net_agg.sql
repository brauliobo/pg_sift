-- The sidecar of a segment with dead rows (docs/COL-PREAGG.md section 13): VACUUM writes the pre-aggregates of each dirty unit net of its dead rows after the segment's own
-- stream, and a unit whose pages are all-visible and whose dead rows are the ones the sidecar was made for answers from them, whatever the aggregate a cube can give: a minimum and a
-- maximum too, where sift.agg_dead_delta takes the dead rows off counts and integer sums only. The answers never change: from the sidecar, from the units' rows (sift.agg_preagg off)
-- and with the scan planner off they are the same. Shapes 9, 13, 14 and 18 are not answered from the pre-aggregates of this index at all (a ctid range, a float sum, a calendar rollup it lacks):
-- they scan with or without dead rows; 12 is a distinct count, which reads key lists that still hold the dead rows' values.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET sift.col_auto_merge = off;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE ov (id int, grp int, kind text, amount numeric(12,2), qty int, big int8, f float8, ts date, pad text) WITH (autovacuum_enabled = false);
INSERT INTO ov SELECT g, CASE WHEN g % 31 = 0 THEN NULL ELSE g % 7 END, CASE WHEN g % 23 = 0 THEN NULL WHEN g % 50 = 1 THEN 'rare' ELSE 'k' || g % 5 END,
       CASE WHEN g % 19 = 0 THEN NULL ELSE (g % 997) * 1.07 - 40 END, CASE WHEN g % 29 = 0 THEN NULL ELSE g % 100 - 30 END,
       4000000000000000000::int8 + g::int8 * 1000003, g * 0.1, date '2024-01-01' + g % 200, repeat('x', 1000)
FROM generate_series(1, 20000) g;
CREATE INDEX ov_col ON ov USING sift_col (id, grp, kind, amount, qty, big, f, ts)
	WITH (preagg = 'cube(grp: amount, qty, big, f; kind: amount, qty; ts: amount, qty; qty: amount)', rollup_dims = 'kind, grp: amount, qty', calendar_columns = 'ts');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ov;

-- the units that answer net of dead rows (from the sidecar or by the delta), as EXPLAIN ANALYZE counts them, and the ones it scans
CREATE FUNCTION ov_net(q text, OUT net int, OUT scanned int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	net := 0; scanned := 0;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) ' || q LOOP
		IF l ~ '(\d+) net of dead rows' THEN net := substring(l from '(\d+) net of dead rows')::int; END IF;
		IF l ~ '(\d+) with dead rows scanned' THEN scanned := substring(l from '(\d+) with dead rows scanned')::int; END IF;
	END LOOP;
END $$;

-- the answers of a query from the pre-aggregates, from the rows of the units and with the scan planner off
CREATE FUNCTION ov_same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a text; b text; c text;
BEGIN
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO a;
	SET LOCAL sift.agg_preagg = off;
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO b;
	SET LOCAL sift.agg_preagg = on;
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('SELECT string_agg(x::text, E''\n'' ORDER BY x::text) FROM (%s) x', q) INTO c;
	RETURN a IS NOT DISTINCT FROM b AND b IS NOT DISTINCT FROM c;
END $$;

CREATE TABLE ov_q (n int, q text);
INSERT INTO ov_q VALUES
	(1, 'SELECT count(*), count(grp), count(amount), sum(amount), sum(qty), avg(amount), sum(big) FROM ov'),
	(2, 'SELECT grp, count(*), count(amount), sum(amount), sum(qty), avg(qty), sum(big) FROM ov GROUP BY grp'),
	(3, 'SELECT kind, count(*), sum(amount) FROM ov GROUP BY kind'),
	(4, 'SELECT kind, grp, count(*), sum(amount), count(qty) FROM ov GROUP BY kind, grp'),
	(5, 'SELECT ts, count(*), sum(qty) FROM ov GROUP BY ts'),
	(6, 'SELECT ts, count(*), sum(amount) FROM ov WHERE ts >= date ''2024-02-01'' AND ts < date ''2024-04-01'' GROUP BY ts'),
	(7, 'SELECT count(*), sum(amount) FROM ov WHERE grp IS NULL'),
	(8, 'SELECT kind, count(*), sum(amount) FROM ov WHERE grp IN (1, 2, 3) GROUP BY kind'),
	(9, 'SELECT count(*), sum(amount) FROM ov WHERE ctid >= ''(800,1)'' AND ctid < ''(3000,1)'''),
	(10, 'SELECT grp, min(amount), max(amount), count(*) FROM ov GROUP BY grp'),
	(11, 'SELECT min(qty), max(qty), min(ts), max(ts) FROM ov'),
	(12, 'SELECT count(DISTINCT kind), count(DISTINCT grp) FROM ov'),
	(13, 'SELECT grp, sum(f), count(*) FROM ov GROUP BY grp'),
	(14, 'SELECT sum(f), count(*) FROM ov'),
	(15, 'SELECT count(*), sum(amount), sum(qty) FROM ov WHERE grp = 3'),
	(16, 'SELECT count(*), sum(amount) FROM ov WHERE qty > 10'),
	(17, 'SELECT date_trunc(''month'', ts)::date AS m, count(*), sum(amount), sum(qty) FROM ov GROUP BY 1'),
	(18, 'SELECT date_trunc(''month'', ts)::date AS m, kind, count(*), sum(amount) FROM ov WHERE ts >= date ''2024-03-01'' GROUP BY 1, 2');

-- dead rows: deletes spread over every superblock, one group dead in every unit, rows of the NULL group dead; vacuumed with every page visible afterwards
DELETE FROM ov WHERE id % 29 = 1 OR kind = 'rare' OR (kind IS NULL AND id % 2 = 0);
VACUUM (DISABLE_PAGE_SKIPPING) ov;
SELECT sift.col_verify('ov_col', true);
SELECT n, ov_same(q) AS same FROM ov_q ORDER BY n;
-- every shape that a cube or the sums of the unit answer is answered net of the dead rows: the minima and maxima too (a unit left to its scan is one the cost model chose to scan)
SELECT n, (ov_net(q)).net > 0 AS net, (ov_net(q)).scanned AS scanned FROM ov_q ORDER BY n;
-- the sidecar is pages of the segment's stream, which sift.col_sections counts on their own
SELECT section, bytes > 0 AS has_bytes FROM sift.col_sections('ov_col') WHERE section = 'net';
SELECT sum(bytes) = pg_relation_size('ov_col') AS sections_add_up FROM sift.col_sections('ov_col');
-- ... and the group that no row is left in is not one of the groups of the answer
SELECT kind, count(*) FROM ov GROUP BY kind ORDER BY kind;

-- a second round of churn: the sidecar of the first no longer fits the dead rows, VACUUM writes the next and the old one's pages are released
DELETE FROM ov WHERE id % 37 = 2;
VACUUM (DISABLE_PAGE_SKIPPING) ov;
SELECT sift.col_verify('ov_col', true);
SELECT n, ov_same(q) AS same FROM ov_q ORDER BY n;
SELECT n, (ov_net(q)).net > 0 AS net, (ov_net(q)).scanned AS scanned FROM ov_q ORDER BY n;
SELECT sum(bytes) = pg_relation_size('ov_col') AS sections_add_up FROM sift.col_sections('ov_col');

-- a page that is not all-visible: the unit scans and the answer is the heap's
UPDATE ov SET qty = qty + 1 WHERE id % 1000 = 5;
SELECT n, ov_same(q) AS same FROM ov_q WHERE n IN (1, 2, 3, 9, 11) ORDER BY n;
VACUUM (DISABLE_PAGE_SKIPPING) ov;
SELECT n, ov_same(q) AS same FROM ov_q ORDER BY n;

-- an index written at an older column format minor (no minima and maxima in the cube, no calendar cells) gets the sidecar too, and verifies
SET sift.col_format_minor = 6;
CREATE TABLE ov6 AS SELECT id, grp, kind, amount, qty FROM ov;
CREATE INDEX ov6_col ON ov6 USING sift_col (id, grp, kind, amount, qty);
RESET sift.col_format_minor;
SELECT format, count(*) FROM sift.col_segments('ov6_col') GROUP BY 1;
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) ov6;
DELETE FROM ov6 WHERE id % 11 = 3;
VACUUM (DISABLE_PAGE_SKIPPING) ov6;
SELECT sift.col_verify('ov6_col', true);
SELECT section FROM sift.col_sections('ov6_col') WHERE section = 'net';
SELECT sum(bytes) = pg_relation_size('ov6_col') AS sections_add_up FROM sift.col_sections('ov6_col');
SELECT ov_same('SELECT kind, count(*), sum(amount), sum(qty) FROM ov6 GROUP BY kind') AS same6;
SELECT (ov_net('SELECT kind, count(*), sum(amount), sum(qty) FROM ov6 GROUP BY kind')).net > 0 AS net6;
DROP TABLE ov6;

-- the merge of everything leaves no dead rows and no sidecar: every unit is exact again
SELECT sift.col_merge('ov_col', true) >= 0 AS merged;
SELECT n, ov_same(q) AS same FROM ov_q ORDER BY n;
SELECT ov_net('SELECT kind, count(*), sum(amount) FROM ov GROUP BY kind') AS after_merge;
SELECT count(*) AS net_sections FROM sift.col_sections('ov_col') WHERE section = 'net';
SELECT sift.col_verify('ov_col', true);

DROP FUNCTION ov_net(text), ov_same(text);
DROP TABLE ov, ov_q;
DROP EXTENSION pg_sift;
