-- query: seconds-by-title-prefix
-- kind: group_by_dim_attr
-- join: inner
SELECT left(d.title, 9) AS title_prefix, count(*) AS n, sum(v.seconds) AS seconds FROM page_view v JOIN doc d ON d.id = v.doc_id GROUP BY 1;

-- query: views-of-needle-docs
-- kind: filter_dim_attr
-- join: inner
SELECT d.kind, count(*) AS n FROM page_view v JOIN doc d ON d.id = v.doc_id WHERE d.description LIKE 'needle7 %' GROUP BY d.kind;

-- query: distinct-docs-by-kind
-- kind: count_distinct_across_join
-- join: inner
SELECT d.kind, count(DISTINCT v.doc_id) AS docs, count(*) AS views FROM page_view v JOIN doc d ON d.id = v.doc_id GROUP BY d.kind;

-- query: top-docs-by-seconds
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT d.id, d.title, sum(v.seconds) AS seconds FROM page_view v JOIN doc d ON d.id = v.doc_id GROUP BY d.id, d.title ORDER BY seconds DESC, d.id LIMIT 10;

-- query: docs-without-views
-- kind: anti_join_count
-- join: anti
SELECT d.kind, count(*) AS unseen FROM doc d WHERE NOT EXISTS (SELECT 1 FROM page_view v WHERE v.doc_id = d.id) GROUP BY d.kind;

-- query: long-views-by-kind
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT d.kind, count(*) FILTER (WHERE v.seconds >= 300) AS long_views, count(*) FILTER (WHERE d.title LIKE 'Title 1%') AS title1, count(*) AS n FROM page_view v JOIN doc d ON d.id = v.doc_id GROUP BY d.kind;

-- query: description-length-by-kind
-- kind: sum_avg
-- join: inner
SELECT d.kind, sum(length(d.description)) AS bytes, round(avg(v.seconds), 4) AS avg_seconds FROM page_view v JOIN doc d ON d.id = v.doc_id GROUP BY d.kind;
