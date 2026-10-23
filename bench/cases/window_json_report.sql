-- a ranked report as one document per category: the rank of the category, its share of the total, and the top 3 countries as a JSON array
-- label: `rank() OVER`, share of the total and `json_agg(.. ORDER BY ..)` of the 3 best countries per category, over `GROUP BY category, country`
-- ordered: yes
--@ pg
WITH g AS (SELECT category, country, sum(amount) AS revenue, count(*) AS n FROM orders WHERE status <> 'cancelled' GROUP BY category, country),
     c AS (SELECT category, sum(revenue) AS revenue, sum(n) AS n FROM g GROUP BY category),
     t AS (SELECT category, country, revenue, row_number() OVER (PARTITION BY category ORDER BY revenue DESC, country) AS rn FROM g)
SELECT c.category, rank() OVER (ORDER BY c.revenue DESC, c.category) AS rank, round(100 * c.revenue / sum(c.revenue) OVER (), 2) AS share, c.n,
       (SELECT json_agg(json_build_object('country', t.country, 'revenue', t.revenue) ORDER BY t.rn) FROM t WHERE t.category = c.category AND t.rn <= 3) AS top_countries
FROM c ORDER BY rank
