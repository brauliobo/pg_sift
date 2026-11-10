-- query: revenue-by-carrier
-- kind: group_by_dim_attr
-- join: inner
SELECT s.carrier, count(*) AS lines, sum(l.qty * l.price) AS revenue FROM order_line l JOIN shipment s ON s.order_id = l.order_id AND s.line_no = l.line_no GROUP BY s.carrier;

-- query: unshipped-lines-by-status
-- kind: anti_join_count
-- join: anti
SELECT h.status, count(*) AS lines FROM order_line l JOIN header h ON h.id = l.order_id WHERE NOT EXISTS (SELECT 1 FROM shipment s WHERE s.order_id = l.order_id AND s.line_no = l.line_no) GROUP BY h.status;

-- query: lines-by-carrier-left
-- kind: group_by_dim_attr
-- join: left
SELECT s.carrier, count(*) AS lines, count(s.order_id) AS shipped FROM order_line l LEFT JOIN shipment s ON s.order_id = l.order_id AND s.line_no = l.line_no GROUP BY s.carrier;

-- query: orders-by-carrier-and-status
-- kind: count_distinct_across_join
-- join: inner
SELECT s.carrier, h.status, count(DISTINCT l.order_id) AS orders FROM order_line l JOIN shipment s ON s.order_id = l.order_id AND s.line_no = l.line_no JOIN header h ON h.id = l.order_id GROUP BY s.carrier, h.status;

-- query: paid-orders-order_line-totals
-- kind: filter_dim_attr
-- join: inner
SELECT h.status, count(*) AS lines, sum(l.qty) AS qty FROM order_line l JOIN header h ON h.id = l.order_id WHERE h.status = 'paid' AND h.placed_on >= date '2024-06-01' GROUP BY h.status;

-- query: header-with-order_line-aggregates
-- kind: group_by_dim_attr
-- join: inner
SELECT h.status, count(DISTINCT h.id) AS orders, count(*) AS lines, sum(l.qty) AS qty FROM header h JOIN order_line l ON l.order_id = h.id GROUP BY h.status;

-- query: top-carriers-by-qty
-- kind: top_n_across_join
-- join: inner
-- ordered: yes
SELECT s.carrier, sum(l.qty) AS qty FROM order_line l JOIN shipment s ON s.order_id = l.order_id AND s.line_no = l.line_no GROUP BY s.carrier ORDER BY qty DESC, s.carrier LIMIT 5;

-- query: shipped-fast-by-status
-- kind: filter_agg_dim_attrs
-- join: inner
SELECT h.status, count(*) FILTER (WHERE s.shipped_on <= h.placed_on + 30) AS fast, count(*) FILTER (WHERE s.carrier = 'w1') AS carrier1, count(*) AS n
FROM order_line l JOIN shipment s ON s.order_id = l.order_id AND s.line_no = l.line_no JOIN header h ON h.id = l.order_id GROUP BY h.status;
