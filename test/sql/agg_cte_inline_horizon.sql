-- sift.agg_scale_horizon: where the scans of an inline shared CTE cost less than the work they replace but not by the margin, the statement is planned again at
-- the square root of the horizon times the rows and at the horizon times the rows (sift.plan_scale), and the inline plan is kept if it wins at both. Here the
-- scans win by 1.46 at the size of the table and are refused for their cost from three times the rows up (the group table of their per-row arms outgrows
-- work_mem): the choice at the size it is does not depend on the setting, and a size at which the scans lose is never promoted by it.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
-- the node under test: the two-level scan (agg_two_level_arm_where) takes the same shape in one node, with no rows to group
SET sift.agg_two_level = off;
CREATE TABLE events (id int PRIMARY KEY, parent_id int NOT NULL, ts date NOT NULL, removed_at timestamptz, payload jsonb);
INSERT INTO events
SELECT g, 1 + g % 50, date '2025-01-01' + (g * 7919 % 360), CASE WHEN g % 97 = 0 THEN now() END,
       CASE WHEN g % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || chr(97 + (g + j) % 4), 'asp', 'asp_' || chr(97 + (g * j) % 5), 'sign', CASE WHEN (g + j) % 3 = 0 THEN 'pos' WHEN (g + j) % 3 = 1 THEN 'neg' ELSE 'zero' END, 'score', (g * j) % 10)) FROM generate_series(1, 1 + g % 4) j))
                 || CASE WHEN g % 5 = 0 THEN jsonb_build_object('mapped', true) ELSE '{}' END END
FROM generate_series(1, 130000) g;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, ts, removed_at, payload) WITH (paths = 'payload.mapped astext, payload.parts[]{cat astext, asp astext, sign astext, score float8}');
CREATE TABLE dims (id int PRIMARY KEY, kind text NOT NULL);
INSERT INTO dims SELECT g, 'kind_' || chr(97 + g % 3) FROM generate_series(1, 50) g;
CREATE TABLE maps (kind text, cat text, asp text, mapped_cat text, mapped_asp text);
INSERT INTO maps SELECT k, 'cat_' || chr(97 + c), 'asp_' || chr(97 + a), 'cat_' || chr(97 + (c + 1) % 4), 'asp_' || chr(97 + (a + 1) % 5)
FROM unnest(ARRAY['kind_a', 'kind_b', 'kind_c']) k, generate_series(0, 3) c, generate_series(0, 4) a;
CREATE UNIQUE INDEX maps_key ON maps (kind, cat, asp);
VACUUM (FREEZE, ANALYZE) events;
ANALYZE dims; ANALYZE maps;

CREATE FUNCTION series_query() RETURNS text LANGUAGE sql AS $$
	SELECT $q$WITH dim_ids AS MATERIALIZED (SELECT DISTINCT id FROM dims WHERE id <= 40),
	sel AS (SELECT dims.*, 'base' AS grp FROM dim_ids d INNER JOIN dims ON dims.id = d.id),
	periods AS (SELECT * FROM (VALUES ('cur', daterange(DATE '2025-06-01', DATE '2025-12-31', '[]'), interval '0 day'),
	                                        ('prev', daterange(DATE '2024-06-01', DATE '2024-12-31', '[]'), interval '1 year')) p(period, range, shift)),
	all_dates AS (SELECT period, CAST(NULL AS date) AS day FROM periods),
	r AS (SELECT e.*, dims.kind, periods.period AS bucket_period, CAST(NULL AS date) AS bucket_date
	      FROM events e INNER JOIN periods ON e.ts <@ periods.range INNER JOIN sel dims ON dims.id = e.parent_id
	      WHERE e.removed_at IS NULL AND ((e.ts >= DATE '2025-06-01' AND e.ts <= DATE '2025-12-31') OR (e.ts >= DATE '2024-06-01' AND e.ts <= DATE '2024-12-31'))),
	u AS (SELECT r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat AS cat, mo.asp AS asp, avg(score) AS score,
	       count(r.id) FILTER (WHERE sign = 'pos') AS pos_count, count(r.id) FILTER (WHERE sign = 'neg') AS neg_count, count(r.id) AS n
	FROM r INNER JOIN LATERAL (SELECT (CASE WHEN jsonb_exists(r.payload, 'mapped') THEN o.src_cat ELSE m.mapped_cat END) AS cat,
	                                 (CASE WHEN jsonb_exists(r.payload, 'mapped') THEN o.src_asp ELSE m.mapped_asp END) AS asp, o.sign, o.score
	          FROM LATERAL (SELECT cat AS src_cat, asp AS src_asp, sign, score FROM jsonb_to_recordset(r.payload['parts']) AS o(cat text, asp text, sign text, score float) WHERE score IS NOT NULL) o
	          LEFT JOIN LATERAL (SELECT mapped_cat, mapped_asp FROM maps WHERE maps.kind = r.kind AND maps.cat = o.src_cat AND maps.asp = o.src_asp AND NOT jsonb_exists(r.payload, 'mapped') LIMIT 1) m ON true) mo ON true
	WHERE mo.cat IS NOT NULL
	GROUP BY r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat, mo.asp
	      UNION ALL
	      SELECT r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat AS cat, CAST(NULL AS text) AS asp, avg(score) AS score,
	       count(r.id) FILTER (WHERE sign = 'pos') AS pos_count, count(r.id) FILTER (WHERE sign = 'neg') AS neg_count, count(r.id) AS n
	FROM r INNER JOIN LATERAL (SELECT (CASE WHEN jsonb_exists(r.payload, 'mapped') THEN o.src_cat ELSE m.mapped_cat END) AS cat,
	                                 (CASE WHEN jsonb_exists(r.payload, 'mapped') THEN o.src_asp ELSE m.mapped_asp END) AS asp, o.sign, o.score
	          FROM LATERAL (SELECT cat AS src_cat, asp AS src_asp, sign, score FROM jsonb_to_recordset(r.payload['parts']) AS o(cat text, asp text, sign text, score float) WHERE score IS NOT NULL) o
	          LEFT JOIN LATERAL (SELECT mapped_cat, mapped_asp FROM maps WHERE maps.kind = r.kind AND maps.cat = o.src_cat AND maps.asp = o.src_asp AND NOT jsonb_exists(r.payload, 'mapped') LIMIT 1) m ON true) mo ON true
	WHERE mo.cat IS NOT NULL
	GROUP BY r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat
	      UNION ALL
	      SELECT r.id, r.parent_id, r.bucket_date, r.bucket_period, CAST(NULL AS text) AS cat, CAST(NULL AS text) AS asp, avg(score) AS score,
	       count(r.id) FILTER (WHERE sign = 'pos') AS pos_count, count(r.id) FILTER (WHERE sign = 'neg') AS neg_count, count(r.id) AS n
	FROM r INNER JOIN LATERAL (SELECT (CASE WHEN jsonb_exists(r.payload, 'mapped') THEN o.src_cat ELSE m.mapped_cat END) AS cat,
	                                 (CASE WHEN jsonb_exists(r.payload, 'mapped') THEN o.src_asp ELSE m.mapped_asp END) AS asp, o.sign, o.score
	          FROM LATERAL (SELECT cat AS src_cat, asp AS src_asp, sign, score FROM jsonb_to_recordset(r.payload['parts']) AS o(cat text, asp text, sign text, score float) WHERE score IS NOT NULL) o
	          LEFT JOIN LATERAL (SELECT mapped_cat, mapped_asp FROM maps WHERE maps.kind = r.kind AND maps.cat = o.src_cat AND maps.asp = o.src_asp AND NOT jsonb_exists(r.payload, 'mapped') LIMIT 1) m ON true) mo ON true
	
	GROUP BY r.id, r.parent_id, r.bucket_date, r.bucket_period),
	g AS (SELECT coalesce(cat, 'p_total') AS cat, asp, day, period, avg(score) AS value, CAST(sum(pos_count) AS int) AS pos_count, CAST(sum(neg_count) AS int) AS neg_count,
	             coalesce(count(DISTINCT id), 0) AS items, count(DISTINCT parent_id) AS parents,
	             json_build_object('value', 100 * (CAST(count(DISTINCT id) FILTER (WHERE score >= 8.0) AS float) / nullif(count(DISTINCT id), 0)),
	                               'high', count(DISTINCT id) FILTER (WHERE score >= 8.0), 'low', count(DISTINCT id) FILTER (WHERE score <= 6.0)) AS split
	      FROM (SELECT x.*, d.day, d.period AS period FROM all_dates d LEFT JOIN u x ON jsonb_build_array(x.bucket_period, x.bucket_date) = jsonb_build_array(d.period, d.day)) t
	      GROUP BY GROUPING SETS ((cat, asp, day, period))),
	t AS (SELECT g.*, lag(g.value) OVER w AS prev, lag(g.items) OVER w AS prev_n FROM g WINDOW w AS (PARTITION BY day, cat, asp ORDER BY period DESC))
SELECT cat, asp, period, round(value::numeric, 6) AS value, pos_count, neg_count, items, parents, split::text, round(prev::numeric, 6) AS prev, prev_n FROM t WHERE period = 'cur' ORDER BY (cat = 'p_total') DESC, cat, asp$q$
$$;


CREATE FUNCTION inline_used(scale float8, horizon float8) RETURNS boolean LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config('sift.plan_scale', scale::text, true), set_config('sift.agg_scale_horizon', horizon::text, true);
	RETURN coalesce((SELECT bool_or(used) FROM sift.explain_why(series_query()) WHERE arm = 'cte_inline'), false);
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

SELECT scale, inline_used(scale, 1) AS horizon_off, inline_used(scale, 100) AS horizon_100 FROM unnest(ARRAY[1, 3, 10]) scale ORDER BY scale;
SELECT differences(series_query()) AS differences;

DROP FUNCTION inline_used, differences, series_query;
DROP TABLE events, dims, maps;
DROP EXTENSION pg_sift;
