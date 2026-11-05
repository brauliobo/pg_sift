-- a Sort of the groups of a scan by a measure and then two keys: one node ranks the distinct values of each key once and sorts the rows by the ranks
-- label: `ORDER BY count(*) % 3, country DESC, customer_id` over `count(*) GROUP BY country, customer_id`, about 100,000 groups
-- ordered: yes
--@ pg
SELECT country, customer_id, count(*) AS n FROM orders GROUP BY country, customer_id ORDER BY count(*) % 3, country DESC, customer_id
