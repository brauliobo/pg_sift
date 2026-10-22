-- A CTE that several aggregates read is planned inline when that puts the aggregates in the scan (agg_cte_inline_upper). The CTE the arms read need not
-- select from the table itself: here it unnests the rows of another CTE of the same WITH (which PostgreSQL inlines, one query reads it), and that one
-- selects from the table. The shared CTE is still a shared CTE over the table, so the arms are scans and the rows are the normal plan's. An explicit
-- MATERIALIZED stays: the user asked for the copy.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, parent_id int NOT NULL, ts date NOT NULL, payload jsonb) WITH (autovacuum_enabled = false);
INSERT INTO events
SELECT g, 1 + g % 50, date '2025-01-01' + (g * 7919 % 360),
       CASE WHEN g % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || chr(97 + (g + j) % 4), 'sub', 'asp_' || chr(97 + (g * j) % 3), 'score', (g * j) % 10))
                                              FROM generate_series(1, 1 + g % 3) j)) END
FROM generate_series(1, 30000) g;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, ts, payload) WITH (paths = 'payload.parts[]{cat text, sub text, score float8}');
CREATE TABLE dims (id int PRIMARY KEY, kind text NOT NULL);
INSERT INTO dims SELECT g, 'cat_' || chr(97 + g % 4) FROM generate_series(1, 50) g;
VACUUM (FREEZE, ANALYZE) events;
ANALYZE dims;

-- base selects from the table; part_rows unnests base; the two arms of per_cat read part_rows (read twice: PostgreSQL materializes it)
CREATE FUNCTION series_query(cte text) RETURNS text LANGUAGE sql AS $$
	SELECT format($q$WITH periods AS (SELECT * FROM (VALUES ('cur', daterange(DATE '2025-03-01', DATE '2025-12-31', '[]')),
	                                                        ('prev', daterange(DATE '2024-03-01', DATE '2024-12-31', '[]'))) p(period, range)),
	  base AS (SELECT e.*, periods.period AS bucket_period
	           FROM events e JOIN periods ON e.ts <@ periods.range JOIN dims ON dims.id = e.parent_id AND dims.kind <> 'cat_d'),
	  part_rows AS %s (SELECT b.id, b.bucket_period, o.cat, o.sub, o.score
	                   FROM base b CROSS JOIN LATERAL jsonb_to_recordset(b.payload['parts']) o(cat text, sub text, score float8) WHERE o.score IS NOT NULL),
	  per_cat AS (
	    SELECT bucket_period, cat, sub, avg(score) AS score, count(*) AS n FROM part_rows GROUP BY bucket_period, cat, sub
	    UNION ALL
	    SELECT bucket_period, NULL, NULL, avg(score), count(*) FROM part_rows GROUP BY bucket_period)
	SELECT p.period, coalesce(x.cat, 'overall') AS cat, x.sub, round(x.score::numeric, 6) AS value, x.n
	FROM periods p LEFT JOIN per_cat x ON x.bucket_period = p.period$q$, cte)
$$;

CREATE FUNCTION plan_nodes(q text) RETURNS TABLE (scan_nodes int, cte_scans int) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	scan_nodes := 0; cte_scans := 0;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l ~ 'Custom Scan \(SiftAgg\)' THEN scan_nodes := scan_nodes + 1; END IF;
		IF l ~ 'CTE Scan on part_rows' THEN cte_scans := cte_scans + 1; END IF;
	END LOOP;
	RETURN NEXT;
END $$;
CREATE FUNCTION differences(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE d bigint;
BEGIN
	SET sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	IF (SELECT count(*) FROM r_off) < 10 THEN RAISE EXCEPTION 'the series has few rows'; END IF;
	SELECT count(*) INTO d FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) x;
	DROP TABLE r_on, r_off;
	RETURN d;
END $$;

-- the default (PostgreSQL's own materialization), the same written NOT MATERIALIZED, and an explicit MATERIALIZED
SELECT 'default' AS cte, * FROM plan_nodes(series_query(''));
SELECT 'not materialized' AS cte, * FROM plan_nodes(series_query('NOT MATERIALIZED'));
SELECT 'materialized' AS cte, * FROM plan_nodes(series_query('MATERIALIZED'));
SELECT differences(series_query('')) AS default_differences, differences(series_query('NOT MATERIALIZED')) AS inline_differences;
DROP TABLE dims, events;
DROP FUNCTION differences(text), plan_nodes(text), series_query(text);
DROP EXTENSION pg_sift;
