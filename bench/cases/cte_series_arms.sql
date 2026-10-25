-- three UNION ALL arms over the elements of a jsonb array, counted DISTINCT above a spine: the shared common table expression or the inline scans
-- the plan of the timed runs is the one asserted; a first statement of a backend planned another way is named in the note
-- label: three `UNION ALL` arms over `jsonb_to_recordset(doc -> 'parts')` with a mapping lookup, `count(DISTINCT id)` above a series, `lag()` over the groups
-- ordered: yes
--@ setup
DROP TABLE IF EXISTS maps;
CREATE TABLE maps (tier text, kind text, sub text, derived_kind text, derived_sub text);
INSERT INTO maps SELECT t, 'cat_' || chr(97 + c), 'catg_' || chr(97 + a), 'cat_' || chr(97 + (c + 1) % 4), 'catg_' || chr(97 + (a + 1) % 6)
FROM unnest(ARRAY['tier_a', 'tier_b', 'tier_c']) t, generate_series(0, 3) c, generate_series(0, 5) a;
CREATE UNIQUE INDEX maps_key ON maps (tier, kind, sub);
ANALYZE maps
--@ pg
WITH dim_ids AS MATERIALIZED (SELECT DISTINCT id FROM dims WHERE id <= 40),
	sel AS (SELECT dims.* FROM dim_ids d INNER JOIN dims ON dims.id = d.id),
	periods AS (SELECT * FROM (VALUES ('cur', daterange(DATE '2025-06-01', DATE '2025-12-31', '[]')), ('prev', daterange(DATE '2024-06-01', DATE '2024-12-31', '[]'))) p(period, range)),
	all_dates AS (SELECT period, CAST(NULL AS date) AS day FROM periods),
	r AS (SELECT e.*, dims.tier AS dtier, periods.period AS bucket_period, CAST(NULL AS date) AS bucket_date
	      FROM events e INNER JOIN periods ON e.ts <@ periods.range INNER JOIN sel dims ON dims.id = e.parent_id
	      WHERE e.deleted_at IS NULL AND ((e.ts >= DATE '2025-06-01' AND e.ts <= DATE '2025-12-31') OR (e.ts >= DATE '2024-06-01' AND e.ts <= DATE '2024-12-31'))),
	u AS (
	SELECT r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat AS cat, mo.catg AS catg, avg(score) AS score,
	       count(r.id) FILTER (WHERE dir_a = 'pos') AS pos_count, count(r.id) FILTER (WHERE dir_a = 'neg') AS neg_count, count(r.id) AS n
	FROM r INNER JOIN LATERAL (SELECT m.derived_kind AS cat, m.derived_sub AS catg, o.dir_a, o.score
	          FROM jsonb_to_recordset(r.doc -> 'parts') AS o(kind text, sub text, dir_a text, score float)
	          LEFT JOIN LATERAL (SELECT derived_kind, derived_sub FROM maps WHERE maps.tier = r.dtier AND maps.kind = o.kind AND maps.sub = o.sub LIMIT 1) m ON true
	          WHERE o.score IS NOT NULL) mo ON true
	WHERE mo.cat IS NOT NULL
	GROUP BY r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat, mo.catg
	      UNION ALL
	SELECT r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat AS cat, CAST(NULL AS text) AS catg, avg(score) AS score,
	       count(r.id) FILTER (WHERE dir_a = 'pos') AS pos_count, count(r.id) FILTER (WHERE dir_a = 'neg') AS neg_count, count(r.id) AS n
	FROM r INNER JOIN LATERAL (SELECT m.derived_kind AS cat, m.derived_sub AS catg, o.dir_a, o.score
	          FROM jsonb_to_recordset(r.doc -> 'parts') AS o(kind text, sub text, dir_a text, score float)
	          LEFT JOIN LATERAL (SELECT derived_kind, derived_sub FROM maps WHERE maps.tier = r.dtier AND maps.kind = o.kind AND maps.sub = o.sub LIMIT 1) m ON true
	          WHERE o.score IS NOT NULL) mo ON true
	WHERE mo.cat IS NOT NULL
	GROUP BY r.id, r.parent_id, r.bucket_date, r.bucket_period, mo.cat
	      UNION ALL
	SELECT r.id, r.parent_id, r.bucket_date, r.bucket_period, CAST(NULL AS text) AS cat, CAST(NULL AS text) AS catg, avg(score) AS score,
	       count(r.id) FILTER (WHERE dir_a = 'pos') AS pos_count, count(r.id) FILTER (WHERE dir_a = 'neg') AS neg_count, count(r.id) AS n
	FROM r INNER JOIN LATERAL (SELECT m.derived_kind AS cat, m.derived_sub AS catg, o.dir_a, o.score
	          FROM jsonb_to_recordset(r.doc -> 'parts') AS o(kind text, sub text, dir_a text, score float)
	          LEFT JOIN LATERAL (SELECT derived_kind, derived_sub FROM maps WHERE maps.tier = r.dtier AND maps.kind = o.kind AND maps.sub = o.sub LIMIT 1) m ON true
	          WHERE o.score IS NOT NULL) mo ON true
	GROUP BY r.id, r.parent_id, r.bucket_date, r.bucket_period),
	g AS (SELECT coalesce(cat, 'm_total') AS cat, catg, day, period, avg(score) AS value, CAST(sum(pos_count) AS int) AS pos_count, CAST(sum(neg_count) AS int) AS neg_count,
	             coalesce(count(DISTINCT id), 0) AS items, count(DISTINCT parent_id) AS parents
	      FROM (SELECT x.*, d.day, d.period AS period FROM all_dates d LEFT JOIN u x ON jsonb_build_array(x.bucket_period, x.bucket_date) = jsonb_build_array(d.period, d.day)) t
	      GROUP BY GROUPING SETS ((cat, catg, day, period))),
	t AS (SELECT g.*, lag(g.value) OVER w AS prev, lag(g.items) OVER w AS prev_n FROM g WINDOW w AS (PARTITION BY day, cat, catg ORDER BY period DESC))
SELECT cat, catg, period, round(value::numeric, 6) AS value, pos_count, neg_count, items, parents, round(prev::numeric, 6) AS prev, prev_n FROM t WHERE period = 'cur' ORDER BY (cat = 'm_total') DESC, cat, catg
