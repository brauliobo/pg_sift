-- generic DISTINCT aggregates: a group's distinct values go through the transition function in btree order (NULL last), the first of equal values only
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT lang, count(DISTINCT placed_at) AS days, sum(DISTINCT store_id) AS ssum, avg(DISTINCT store_id) AS savg, min(DISTINCT status) AS lo,
       array_agg(DISTINCT country_code) AS ccs, string_agg(DISTINCT source, ',') AS srcs, array_agg(DISTINCT discount) AS discs,
       count(DISTINCT total) FILTER (WHERE discount > 50) AS bigdisc, sum(DISTINCT store_id % 7) FILTER (WHERE status = 'paid') AS paid_mods
FROM orders GROUP BY lang ORDER BY lang
