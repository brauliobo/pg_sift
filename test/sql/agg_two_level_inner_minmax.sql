-- max and min of a float8 column or of a float8 key of a jsonb document can be the inner aggregates of the arms of the two-level scan: a query that groups the rows of a
-- table by their id and takes the largest score of each, then averages the scores of a group, runs as one scan. A cell with no value (NULL, a missing key) gives NULL, which
-- the outer avg and sum skip; a NaN is the largest value and an infinity compares as itself. Every query runs as the two-level scan, as the arms alone
-- (sift.agg_two_level off) and as the normal plan; the answers must be the same.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;

CREATE TABLE rv (id int PRIMARY KEY, grp int, src int, f float8, doc jsonb, deleted bool) WITH (autovacuum_enabled = false);
INSERT INTO rv
SELECT i, CASE WHEN i % 29 = 0 THEN NULL ELSE i % 70 END, i % 4,
       CASE WHEN i % 41 = 0 THEN NULL WHEN i % 313 = 0 THEN 'NaN' WHEN i % 331 = 0 THEN 'Infinity' WHEN i % 337 = 0 THEN '-Infinity' ELSE ((i * 7919) % 1009) / 11.0 END,
       CASE WHEN i % 13 = 0 THEN NULL WHEN i % 7 = 0 THEN '{}'::jsonb
            ELSE jsonb_build_object('mhead', ((i * 31) % 97) / 7.0, 'service', CASE WHEN i % 5 = 0 THEN NULL ELSE (i % 10)::numeric END) END,
       i % 11 = 0
FROM generate_series(1, 12000) i;
CREATE INDEX rv_col ON rv USING sift_col (id, grp, src, f, doc, deleted) WITH (paths = 'doc.* float8');
VACUUM (FREEZE, ANALYZE) rv;

CREATE FUNCTION pushed(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; scan bool := false;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN scan := true; END IF;
		IF l ~ '(SiftAggRows|GroupAggregate|HashAggregate)' THEN RETURN false; END IF;
	END LOOP;
	RETURN scan;
END $$;

CREATE FUNCTION same(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE a bigint; b bigint; n bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET sift.agg_two_level = off;
	EXECUTE format('CREATE TEMP TABLE r_arms AS %s', q);
	RESET sift.agg_two_level;
	SELECT count(*) INTO a FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d;
	SELECT count(*) INTO b FROM ((TABLE r_arms EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_arms)) d;
	SELECT count(*) INTO n FROM r_off;
	DROP TABLE r_on, r_arms, r_off;
	RETURN a = 0 AND b = 0 AND n > 0 AND pushed(q);
END $$;

-- the outer query over per-row inner aggregates: the aggregate, the key and the outer tail
CREATE FUNCTION report(inner_agg text, outer_agg text, key text DEFAULT 'grp', inner_where text DEFAULT 'true', inner_having text DEFAULT '') RETURNS text LANGUAGE sql AS $$
	SELECT format($q$SELECT %s, count(id) AS n, %s AS v FROM (SELECT id, grp, src, %s AS x FROM rv WHERE %s GROUP BY id, grp, src %s) t GROUP BY %s$q$,
	              key, outer_agg, inner_agg, inner_where, inner_having, key)
$$;

-- the cases: kind of inner aggregate x outer aggregate; both round to 6 digits because the sums of the plans differ in the last bit
SELECT same(report(a, o, k)) AS ok, a, o, k
FROM (VALUES ('max(f)', 'round(avg(x)::numeric, 6)'), ('min(f)', 'round(avg(x)::numeric, 6)'),
             ('max((doc->>''mhead'')::float8)', 'round(avg(x)::numeric, 6)'), ('min((doc->>''mhead'')::float8)', 'round(sum(x)::numeric, 6)'),
             ('max((doc->>''service'')::float8)', 'round(avg(x)::numeric, 6)'),
             ('max(f)', 'round(sum(x)::numeric, 6)')) c(a, o),
     (VALUES ('grp'), ('src'), ('grp, src')) k(k)
ORDER BY a, o, k;

-- a WHERE of the arm, a HAVING on the inner aggregate, a NULL group key, a join-free filter on deleted rows
SELECT same(report('max(f)', 'round(avg(x)::numeric, 6)', 'grp', 'NOT deleted')) AS inner_where;
SELECT same(report('max((doc->>''mhead'')::float8)', 'round(avg(x)::numeric, 6)', 'src', 'true', 'HAVING max((doc->>''mhead'')::float8) > 5')) AS inner_having;
SELECT same(report('max((doc->>''mhead'')::float8)', 'round(avg(x)::numeric, 6)', 'src', 'src > 0', 'HAVING min((doc->>''mhead'')::float8) < 8')) AS inner_having_min;

-- two arms, one with a WHERE
SELECT same($q$SELECT grp, count(id) AS n, round(avg(x)::numeric, 6) AS v
	FROM (SELECT id, grp, max(f) AS x FROM rv GROUP BY id, grp
	      UNION ALL
	      SELECT id, grp, max((doc->>'mhead')::float8) FROM rv WHERE src = 1 GROUP BY id, grp) t GROUP BY grp$q$) AS two_arms;

DROP TABLE rv;
DROP FUNCTION report(text, text, text, text, text), same(text), pushed(text);
DROP EXTENSION pg_sift;
