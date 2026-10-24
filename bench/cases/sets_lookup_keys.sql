-- grouping sets over the columns two lookup tables bring (each joined on one text column) and a key of many values: the group table is a hash
-- table, the sets that read the same rows share the key ids of a chunk, and a lookup of one column resolves its rows in a loop of its kind
-- label: `JOIN labels l ON l.category = o.category JOIN regions r ON r.country = o.country`, `GROUPING SETS` of `l.label, r.region, status, customer_id % 1000`
-- ordered: yes
--@ setup
CREATE TABLE labels AS SELECT DISTINCT category, upper(category) AS label FROM orders;
CREATE TABLE regions AS SELECT DISTINCT country, left(country, 1) AS region FROM orders;
ANALYZE labels, regions
--@ pg
SELECT l.label, r.region, o.status, o.customer_id % 1000 AS bucket, count(*) AS n, sum(o.quantity) AS items
FROM orders o JOIN labels l ON l.category = o.category JOIN regions r ON r.country = o.country
GROUP BY GROUPING SETS ((l.label, r.region, o.status, o.customer_id % 1000), (l.label, o.status), (r.region, o.customer_id % 1000), ()) ORDER BY 1, 2, 3, 4
