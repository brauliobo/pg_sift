-- query: logs-by-team-owner
-- kind: chain
-- join: inner
SELECT t.owner, count(*) AS logs FROM log l JOIN service s ON s.id = l.service_id JOIN team t ON t.id = s.team_id GROUP BY t.owner;

-- query: errors-by-environment
-- kind: filter_dim_attr
-- join: inner
SELECT s.env, count(*) AS errors FROM log l JOIN service s ON s.id = l.service_id WHERE l.level = 'ERROR' GROUP BY s.env;

-- query: message-prefix-by-team-tier
-- kind: filter_dim_attr
-- join: inner
SELECT t.tier, count(*) AS n FROM log l JOIN service s ON s.id = l.service_id JOIN team t ON t.id = s.team_id WHERE l.message LIKE 'ERROR: code1%' GROUP BY t.tier;

-- query: message-substring-count
-- kind: filter_dim_attr
-- join: inner
SELECT s.env, count(*) AS n FROM log l JOIN service s ON s.id = l.service_id WHERE l.message LIKE '%code7 %' GROUP BY s.env;

-- query: distinct-services-by-team
-- kind: count_distinct_across_join
-- join: inner
SELECT s.team_id, count(DISTINCT l.service_id) AS services, count(*) AS logs FROM log l JOIN service s ON s.id = l.service_id GROUP BY s.team_id;

-- query: top-services-by-errors
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT s.id, s.name, count(*) AS errors FROM log l JOIN service s ON s.id = l.service_id WHERE l.level = 'ERROR' GROUP BY s.id, s.name ORDER BY errors DESC, s.name LIMIT 10;

-- query: services-without-logs
-- kind: anti_join_count
-- join: anti
SELECT s.env, count(*) AS silent FROM service s WHERE NOT EXISTS (SELECT 1 FROM log l WHERE l.service_id = s.id) GROUP BY s.env;

-- query: level-mix-by-tier
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT t.tier, count(*) FILTER (WHERE l.level = 'ERROR') AS errors, count(*) FILTER (WHERE l.level = 'WARN' AND s.env = 'prod') AS prod_warn, count(*) AS n
FROM log l JOIN service s ON s.id = l.service_id JOIN team t ON t.id = s.team_id GROUP BY t.tier;
