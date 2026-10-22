-- A CTE that three aggregates read (the arms of a UNION ALL, each over the elements of a jsonb array, the rows then counted DISTINCT above a spine) is planned
-- inline when the scan makes that cheaper: the plan keeps the three SiftAgg arms below one SiftAggRows node, which costs far less than the arms added up
-- (the node reads the table once). The choice weighs the cost of the scans in the inline plan against what the shared plan costs for them, so it must
-- take the cost of the SiftAggRows node, not the sum of the arms it fused, or a plan that is a third of the shared one stays on the shared CTE.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE events (id int PRIMARY KEY, parent_id int NOT NULL, ts date NOT NULL, removed_at timestamptz, payload jsonb);
INSERT INTO events
SELECT g, 1 + g % 50, date '2025-01-01' + (g * 7919 % 360), CASE WHEN g % 97 = 0 THEN now() END,
       CASE WHEN g % 13 = 0 THEN NULL
            ELSE jsonb_build_object('parts', (SELECT jsonb_agg(jsonb_build_object('cat', 'cat_' || chr(97 + (g + j) % 4), 'catg', 'catg_' || chr(97 + (g * j) % 5), 'dir_a', CASE WHEN (g + j) % 3 = 0 THEN 'pos' WHEN (g + j) % 3 = 1 THEN 'neg' ELSE 'zero' END, 'score', (g * j) % 10)) FROM generate_series(1, 1 + g % 4) j))
                 || CASE WHEN g % 5 = 0 THEN jsonb_build_object('flag_a', true) ELSE '{}' END END
FROM generate_series(1, 30000) g;
CREATE INDEX events_col ON events USING sift_col (id, parent_id, ts, removed_at, payload) WITH (paths = 'payload.flag_a astext, payload.parts[]{cat astext, catg astext, dir_a astext, score float8}');
CREATE TABLE dims (id int PRIMARY KEY, kind text NOT NULL);
INSERT INTO dims SELECT g, 'kind_' || chr(97 + g % 3) FROM generate_series(1, 50) g;
CREATE TABLE maps (kind text, cat text, catg text, derived_cat text, mapped_catg text);
INSERT INTO maps SELECT k, 'cat_' || chr(97 + c), 'catg_' || chr(97 + a), 'cat_' || chr(97 + (c + 1) % 4), 'catg_' || chr(97 + (a + 1) % 5)
FROM unnest(ARRAY['kind_a', 'kind_b', 'kind_c']) k, generate_series(0, 3) c, generate_series(0, 4) a;
CREATE UNIQUE INDEX maps_key ON maps (kind, cat, catg);
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
	u AS (SELECT r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat AS cat, mo.catg AS catg, avg(score) AS score,
	       count(r.id) FILTER (WHERE dir_a = 'pos') AS pos_count, count(r.id) FILTER (WHERE dir_a = 'neg') AS neg_count, count(r.id) AS n
	FROM r INNER JOIN LATERAL (SELECT (CASE WHEN jsonb_exists(r.payload, 'flag_a') THEN o.src_cat ELSE m.derived_cat END) AS cat,
	                                 (CASE WHEN jsonb_exists(r.payload, 'flag_a') THEN o.src_catg ELSE m.mapped_catg END) AS catg, o.dir_a, o.score
	          FROM LATERAL (SELECT cat AS src_cat, catg AS src_catg, dir_a, score FROM jsonb_to_recordset(r.payload['parts']) AS o(cat text, catg text, dir_a text, score float) WHERE score IS NOT NULL) o
	          LEFT JOIN LATERAL (SELECT derived_cat, mapped_catg FROM maps WHERE maps.kind = r.kind AND maps.cat = o.src_cat AND maps.catg = o.src_catg AND NOT jsonb_exists(r.payload, 'flag_a') LIMIT 1) m ON true) mo ON true
	WHERE mo.cat IS NOT NULL
	GROUP BY r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat, mo.catg
	      UNION ALL
	      SELECT r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat AS cat, CAST(NULL AS text) AS catg, avg(score) AS score,
	       count(r.id) FILTER (WHERE dir_a = 'pos') AS pos_count, count(r.id) FILTER (WHERE dir_a = 'neg') AS neg_count, count(r.id) AS n
	FROM r INNER JOIN LATERAL (SELECT (CASE WHEN jsonb_exists(r.payload, 'flag_a') THEN o.src_cat ELSE m.derived_cat END) AS cat,
	                                 (CASE WHEN jsonb_exists(r.payload, 'flag_a') THEN o.src_catg ELSE m.mapped_catg END) AS catg, o.dir_a, o.score
	          FROM LATERAL (SELECT cat AS src_cat, catg AS src_catg, dir_a, score FROM jsonb_to_recordset(r.payload['parts']) AS o(cat text, catg text, dir_a text, score float) WHERE score IS NOT NULL) o
	          LEFT JOIN LATERAL (SELECT derived_cat, mapped_catg FROM maps WHERE maps.kind = r.kind AND maps.cat = o.src_cat AND maps.catg = o.src_catg AND NOT jsonb_exists(r.payload, 'flag_a') LIMIT 1) m ON true) mo ON true
	WHERE mo.cat IS NOT NULL
	GROUP BY r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat
	      UNION ALL
	      SELECT r.id, r.parent_id, r.bucket_date, r.bucket_period, CAST(NULL AS text) AS cat, CAST(NULL AS text) AS catg, avg(score) AS score,
	       count(r.id) FILTER (WHERE dir_a = 'pos') AS pos_count, count(r.id) FILTER (WHERE dir_a = 'neg') AS neg_count, count(r.id) AS n
	FROM r INNER JOIN LATERAL (SELECT (CASE WHEN jsonb_exists(r.payload, 'flag_a') THEN o.src_cat ELSE m.derived_cat END) AS cat,
	                                 (CASE WHEN jsonb_exists(r.payload, 'flag_a') THEN o.src_catg ELSE m.mapped_catg END) AS catg, o.dir_a, o.score
	          FROM LATERAL (SELECT cat AS src_cat, catg AS src_catg, dir_a, score FROM jsonb_to_recordset(r.payload['parts']) AS o(cat text, catg text, dir_a text, score float) WHERE score IS NOT NULL) o
	          LEFT JOIN LATERAL (SELECT derived_cat, mapped_catg FROM maps WHERE maps.kind = r.kind AND maps.cat = o.src_cat AND maps.catg = o.src_catg AND NOT jsonb_exists(r.payload, 'flag_a') LIMIT 1) m ON true) mo ON true
	
	GROUP BY r.id, r.parent_id, r.bucket_date, r.bucket_period),
	g AS (SELECT coalesce(cat, 'mhead') AS cat, catg, day, period, avg(score) AS value, CAST(sum(pos_count) AS int) AS pos_count, CAST(sum(neg_count) AS int) AS neg_count,
	             coalesce(count(DISTINCT id), 0) AS items, count(DISTINCT parent_id) AS parents,
	             json_build_object('value', 100 * (CAST(count(DISTINCT id) FILTER (WHERE score >= 8.0) AS float) / nullif(count(DISTINCT id), 0)),
	                               'high', count(DISTINCT id) FILTER (WHERE score >= 8.0), 'low', count(DISTINCT id) FILTER (WHERE score <= 6.0)) AS split
	      FROM (SELECT x.*, d.day, d.period AS period FROM all_dates d LEFT JOIN u x ON jsonb_build_array(x.bucket_period, x.bucket_date) = jsonb_build_array(d.period, d.day)) t
	      GROUP BY GROUPING SETS ((cat, catg, day, period))),
	t AS (SELECT g.*, lag(g.value) OVER w AS prev, lag(g.items) OVER w AS prev_n FROM g WINDOW w AS (PARTITION BY day, cat, catg ORDER BY period DESC))
SELECT cat, catg, period, round(value::numeric, 6) AS value, pos_count, neg_count, items, parents, split::text, round(prev::numeric, 6) AS prev, prev_n FROM t WHERE period = 'cur' ORDER BY (cat = 'mhead') DESC, cat, catg$q$
$$;

-- the nodes of the scan in the plan, and the rows against the normal plan
CREATE FUNCTION scan_nodes(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP IF l ~ 'Custom Scan \(SiftAgg' THEN n := n + 1; END IF; END LOOP;
	RETURN n;
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

-- the shared CTE takes the scan (the arms and the SiftAggRows node above them), and the rows are the normal plan's
SELECT scan_nodes(series_query()) AS scan_nodes, differences(series_query()) AS differences;
