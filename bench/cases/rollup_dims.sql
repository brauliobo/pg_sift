-- the rollup of two dimensions stored in each unit
-- label: `GROUP BY country, category`, `rollup_dims = 'country, category: amount'`
-- ordered: yes
--@ pg
SELECT country, category, count(*), sum(amount) FROM orders GROUP BY country, category ORDER BY country, category
