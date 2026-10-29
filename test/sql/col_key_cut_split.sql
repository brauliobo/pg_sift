-- key_cut with a second key: a key of at least 8,192 rows is cut into units of 4,096, so a window of the second key skips most of its units (a unit of the whole aim
-- spans most of the key's dates); a smaller key keeps the units it had. The answers are those of the table (sift.enable_agg off) after updates, deletes, a promotion,
-- a merge and a REINDEX: integers and numeric exact, float8 within rounding, NULL keys and NULL dates included.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

-- tenants of 4,096, 8,191, 8,192, 8,193, 12,288 and 30,000 rows around the cut, 300 small ones, a NULL tenant; the dates are independent of the tenant and of the heap order
CREATE TABLE ks (id int, tenant_id int, d date, n numeric(12,2), f float8, pad text) WITH (autovacuum_enabled = false);
SELECT setseed(0.25);
INSERT INTO ks SELECT i, t, CASE WHEN random() < 0.02 THEN NULL ELSE date '2020-01-01' + (random() * 2000)::int END, (random() * 100000)::numeric(12,2), random() * 1000, 'x'
FROM (SELECT row_number() OVER (ORDER BY random()) AS i, t FROM (
	SELECT t, generate_series(1, CASE t WHEN 1 THEN 4096 WHEN 2 THEN 8191 WHEN 3 THEN 8192 WHEN 4 THEN 8193 WHEN 5 THEN 12288 WHEN 6 THEN 30000 WHEN 7 THEN 70 ELSE 20 END) FROM generate_series(1, 306) t) x) y;
INSERT INTO ks SELECT 100000 + i, NULL, date '2021-01-01' + i % 300, i, i, 'n' FROM generate_series(1, 500) i;
VACUUM (FREEZE, ANALYZE) ks;
CREATE INDEX ks_idx ON ks USING sift_col (id, tenant_id, d, n, f) WITH (key_cut = 'tenant_id, d');
CREATE INDEX ks_one ON ks USING sift_col (id, tenant_id, d, n, f) WITH (key_cut = 'tenant_id');
SELECT sift.col_verify('ks_idx', true);

CREATE FUNCTION kunits(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; u text := '';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Units: %' THEN u := u || ' ' || trim(l); END IF;
	END LOOP;
	RETURN u;
END $$;
CREATE FUNCTION kread(q text) RETURNS int LANGUAGE sql AS $$ SELECT (regexp_match(kunits(q), 'Units: (\d+) read'))[1]::int $$;

-- the keys of 8,192 rows and more are cut into units of 4,096; the other keys fill the units of the key alone
SELECT sum(units) AS units FROM sift.col_segments('ks_idx') WHERE kind = 'immutable';
SELECT sum(units) AS units_key_only FROM sift.col_segments('ks_one') WHERE kind = 'immutable';

-- the plan reads the index of the second key and the one of the key alone in turn
CREATE FUNCTION kuse(which text) RETURNS void LANGUAGE sql AS $$
	UPDATE pg_index SET indisvalid = (indexrelid = which::regclass) WHERE indexrelid IN ('ks_idx'::regclass, 'ks_one'::regclass) $$;
SELECT kuse('ks_idx');
SELECT kread($$SELECT count(*), sum(n) FROM ks WHERE tenant_id = 6 AND d >= '2022-01-01' AND d < '2022-02-01'$$) AS window_30000_units,
       kread($$SELECT count(*), sum(n) FROM ks WHERE tenant_id = 3 AND d >= '2022-01-01' AND d < '2022-02-01'$$) AS window_8192_units,
       kread($$SELECT count(*), sum(n) FROM ks WHERE tenant_id = 2 AND d >= '2022-01-01' AND d < '2022-02-01'$$) AS window_8191_units;
SELECT kuse('ks_one');
SELECT kread($$SELECT count(*), sum(n) FROM ks WHERE tenant_id = 6 AND d >= '2022-01-01' AND d < '2022-02-01'$$) AS window_30000_key_only;
SELECT kuse('ks_idx');

-- the answers are those of the table
CREATE TABLE kq (t int, lo date, hi date);
INSERT INTO kq SELECT t, date '2020-01-01' + w, date '2020-01-01' + w + len FROM unnest(ARRAY[1, 2, 3, 4, 5, 6, 7, 120, 306]) t, (VALUES (0, 2000), (100, 30), (900, 365), (1500, 120), (3000, 10)) v(w, len);
CREATE FUNCTION kres(sift bool) RETURNS TABLE (t int, lo date, c bigint, s1 bigint, sn numeric, sf float8, nulls bigint) LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN sift THEN 'on' ELSE 'off' END, true);
	RETURN QUERY SELECT q.t, q.lo, count(*), coalesce(sum(id), 0)::bigint, sum(n), sum(f), count(*) FILTER (WHERE ks.d IS NULL)
	FROM kq q LEFT JOIN ks ON ks.tenant_id = q.t AND ks.d >= q.lo AND ks.d < q.hi GROUP BY q.t, q.lo ORDER BY 1, 2;
END $$;
CREATE FUNCTION kwhole(sift bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN sift THEN 'on' ELSE 'off' END, true);
	SELECT md5(string_agg(coalesce(tenant_id::text, 'null') || ':' || c || ':' || s || ':' || coalesce(mx::text, ''), ',' ORDER BY tenant_id NULLS LAST)) INTO r
	FROM (SELECT tenant_id, count(*) c, sum(n) s, max(d) mx FROM ks GROUP BY tenant_id) x;
	RETURN r;
END $$;
CREATE FUNCTION kcheck(step text) RETURNS TABLE (after_step text, cells bigint, exact bool, float_within_rounding bool, whole_table_same bool) LANGUAGE sql AS $$
	SELECT step, count(*), bool_and(a.c = b.c AND a.s1 = b.s1 AND a.sn IS NOT DISTINCT FROM b.sn AND a.nulls = b.nulls),
	       bool_and(abs(a.sf - b.sf) <= 1e-9 * abs(b.sf) OR (a.sf IS NULL AND b.sf IS NULL)), kwhole(true) = kwhole(false)
	FROM kres(true) a JOIN kres(false) b USING (t, lo) $$;
SELECT * FROM kcheck('built');

UPDATE ks SET d = d + 400 WHERE id % 13 = 0;
UPDATE ks SET tenant_id = 6 WHERE tenant_id = 2 AND id % 5 = 0;
UPDATE ks SET d = NULL WHERE id % 23 = 0;
DELETE FROM ks WHERE id % 17 = 0;
SELECT * FROM kcheck('updated and deleted');
VACUUM ks;
SELECT * FROM kcheck('vacuumed');
INSERT INTO ks SELECT 200000 + i, CASE WHEN i % 3 = 0 THEN 6 WHEN i % 3 = 1 THEN 307 ELSE NULL END, date '2020-01-01' + (i * 37) % 2100, i, i / 3.0, 'p' FROM generate_series(1, 20000) i;
SELECT * FROM kcheck('write buffer');
SELECT sift.col_promote('ks_idx') IS NOT NULL AS promoted;
SELECT * FROM kcheck('promoted');
SELECT sift.col_merge('ks_idx', true) IS NOT NULL AS merged;
SELECT * FROM kcheck('merged');
SELECT sift.col_verify('ks_idx', true);
REINDEX INDEX ks_idx;
SELECT * FROM kcheck('reindexed');
SELECT sum(units) AS units_after_reindex FROM sift.col_segments('ks_idx') WHERE kind = 'immutable';
SELECT sift.col_verify('ks_idx', true);

DROP TABLE kq, ks;
DROP FUNCTION kunits(text), kread(text), kuse(text), kres(bool), kwhole(bool), kcheck(text);
DROP EXTENSION pg_sift;
