-- The inline plan of a statement writes the body of each shared CTE at each reference, and PostgreSQL plans every copy (and copies the tree again at each level of
-- sub-selects): 700 ms of planning for a statement of six shared CTEs over tables with a column index, where the scans used the tables of one. A plan in full inlines
-- all of them, sees which tables its scans read (the table of the index and those of the lookups its joins make) and plans again with the CTEs that read them; the
-- others stay shared. The verdict keeps the set, so that the plans that follow from it (and the ones that check it) inline those only.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE fact (id int PRIMARY KEY, k int NOT NULL, grp int NOT NULL, v int NOT NULL, doc jsonb) WITH (autovacuum_enabled = false);
INSERT INTO fact SELECT i, i % 3000, i % 40, i % 17, jsonb_build_object('a', i % 9) FROM generate_series(1, 60000) i;
CREATE INDEX fact_col ON fact USING sift_col (id, k, grp, v, doc);
CREATE TABLE dim (k int PRIMARY KEY, name text NOT NULL, region int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT g, 'n' || g, g % 7 FROM generate_series(0, 2999) g;
CREATE INDEX dim_col ON dim USING sift_col (k, name, region);
CREATE TABLE lbl (region int PRIMARY KEY, label text NOT NULL, long_label text NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO lbl SELECT g, 'r' || g, 'region ' || g FROM generate_series(0, 6) g;
CREATE INDEX lbl_col ON lbl USING sift_col (region, label, long_label);
VACUUM (FREEZE, ANALYZE) fact, dim, lbl;
RESET client_min_messages;

-- ev feeds four aggregates, two of them with a lookup of dm; lb only names the regions of the rows the aggregates leave, in the part PostgreSQL plans
CREATE FUNCTION stmt(cut int, labels bool) RETURNS text LANGUAGE sql AS $f$ SELECT format($q$
 WITH ev AS (SELECT k, grp, v FROM fact WHERE v < %s), dm AS (SELECT k, region FROM dim WHERE region < 5), lb AS (SELECT region, label, long_label FROM lbl)
 SELECT a.grp, a.n, b.s, c.nd, d.m, %s
 FROM (SELECT grp, count(*) AS n FROM ev GROUP BY grp) a
 JOIN (SELECT grp, sum(v) AS s FROM ev GROUP BY grp) b USING (grp)
 JOIN (SELECT ev.grp, count(DISTINCT dm.region) AS nd FROM ev JOIN dm USING (k) GROUP BY ev.grp) c USING (grp)
 JOIN (SELECT ev.grp, count(*) AS m FROM ev JOIN dm USING (k) WHERE dm.region = 1 GROUP BY ev.grp) d USING (grp)
 %s ORDER BY 1 $q$, cut,
 CASE WHEN labels THEN 'l1.label AS short_label, l2.long_label' ELSE 'NULL::text AS short_label, NULL::text AS long_label' END,
 CASE WHEN labels THEN 'LEFT JOIN lb l1 ON l1.region = a.grp % 7 LEFT JOIN lb l2 ON l2.region = b.s % 7' ELSE '' END) $f$;

-- the shared CTEs that are left in the plan, and the scans
CREATE FUNCTION plan_shape(q text) RETURNS text LANGUAGE plpgsql AS $f$
DECLARE l text; ctes text[] := '{}'; scans int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ '^ *CTE ' THEN ctes := ctes || trim(substr(trim(l), 5)); END IF;
		IF l LIKE '%Custom Scan (SiftAgg)%' THEN scans := scans + 1; END IF;
	END LOOP;
	RETURN format('shared: %s; scans: %s', coalesce(nullif(array_to_string(ctes, ' '), ''), 'none'), scans);
END $f$;
CREATE FUNCTION counts() RETURNS text LANGUAGE sql AS
$$ SELECT string_agg(replace(name, 'verdict_cache_', '') || '=' || value, ' ' ORDER BY name) FROM sift.stats() WHERE name IN ('verdict_cache_hits', 'verdict_cache_misses') $$;

-- planned in full (the cache has nothing), then from the verdict twice: the same plan, lb shared
SELECT sift.stats_reset();
SELECT plan_shape(stmt(12, true)) AS in_full;
SELECT plan_shape(stmt(12, true)) AS from_the_verdict;
SELECT plan_shape(stmt(13, true)) AS other_constant;
SELECT counts();
-- without the labels lb is not read and the scans take ev and dm: nothing stays shared
SELECT plan_shape(stmt(12, false)) AS without_labels;

SET sift.agg_verdict_cache = off;
SELECT plan_shape(stmt(12, true)) AS no_cache;
RESET sift.agg_verdict_cache;

-- the same rows with the scan on and off, for several sizes of ev, after updates, deletes, new rows and VACUUM, the verdict kept from the first plan
CREATE FUNCTION same_rows(q text) RETURNS TABLE (result_rows bigint, differing bigint) LANGUAGE plpgsql AS $f$
BEGIN
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	SELECT count(*) INTO result_rows FROM r_on;
	SELECT count(*) INTO differing FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $f$;
CREATE FUNCTION check_rows(step text) RETURNS TABLE (label text, result_rows bigint, differing bigint) LANGUAGE plpgsql AS $f$
DECLARE c int; w bool;
BEGIN
	FOREACH c IN ARRAY ARRAY[0, 1, 6, 12, 17] LOOP
		FOREACH w IN ARRAY ARRAY[false, true] LOOP
			label := format('%s v<%s %s', step, c, CASE WHEN w THEN 'labels' ELSE 'no labels' END);
			SELECT s.result_rows, s.differing INTO result_rows, differing FROM same_rows(stmt(c, w)) s;
			RETURN NEXT;
		END LOOP;
	END LOOP;
END $f$;
SELECT * FROM check_rows('initial');
UPDATE fact SET doc = NULL WHERE id % 11 = 0;
UPDATE fact SET grp = (grp + 1) % 40 WHERE id % 17 = 0;
UPDATE fact SET v = v + 3 WHERE id % 13 = 0;
DELETE FROM fact WHERE id % 19 = 0;
INSERT INTO fact SELECT 100000 + i, i % 3000, i % 40, i % 17, jsonb_build_object('a', i % 4) FROM generate_series(1, 3000) i;
UPDATE dim SET region = (region + 1) % 7 WHERE k % 5 = 0;
DELETE FROM dim WHERE k % 29 = 0;
SELECT * FROM check_rows('after dml');
VACUUM (ANALYZE) fact, dim;
SELECT * FROM check_rows('after vacuum');
SET sift.agg_verdict_cache = off;
SELECT plan_shape(stmt(12, true)) AS after_changes;

RESET sift.agg_verdict_cache;
DROP FUNCTION check_rows(text), same_rows(text), counts(), plan_shape(text), stmt(int, bool);
DROP TABLE fact, dim, lbl;
DROP EXTENSION pg_sift;
