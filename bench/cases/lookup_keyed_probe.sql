-- the ids of a request (a CTE of them) joined to the table of their rows: the lookup table is read by a btree probe for each key while the probes cost less than its pages, where it was read whole
-- label: `WITH ids AS (3 keys), d AS (ids JOIN segs)`, `orders JOIN d ON d.segment_id = orders.segment_id`, `count`, `sum`; the lookup table of 6,313 rows of 350 bytes
-- ordered: yes
--@ setup
CREATE TABLE segs (segment_id int PRIMARY KEY, kind int, pad text);
INSERT INTO segs SELECT i, i % 5, repeat('x', 350) FROM generate_series(0, 6312) i;
CREATE TABLE picks (grp int, segment_id int);
INSERT INTO picks SELECT 1, i FROM generate_series(10, 12) i;
ANALYZE segs; ANALYZE picks
--@ pg
WITH ids AS MATERIALIZED (SELECT DISTINCT segs.segment_id FROM segs WHERE segs.segment_id IN (SELECT segment_id FROM picks WHERE grp = 1)),
	d AS (SELECT segs.*, to_jsonb(segs) AS doc FROM ids INNER JOIN segs ON segs.segment_id = ids.segment_id)
SELECT d.kind, count(*) AS n, sum(o.amount) AS revenue FROM orders o INNER JOIN d ON d.segment_id = o.segment_id WHERE o.order_date >= DATE '2025-01-01' GROUP BY 1 ORDER BY 1
