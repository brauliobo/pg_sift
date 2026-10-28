-- TPC-H-like shipping priority (TPC-H Q3): customer, orders and lineitem joined, the 10 orders of the largest revenue
-- label: TPC-H-like shipping priority (three tables, `c_mktsegment = 'BUILDING'`, a date on `o_orderdate` and on `l_shipdate`, `ORDER BY revenue DESC LIMIT 10`), 1,000,000 lineitem rows
-- ordered: yes
--@ pg
SELECT l_orderkey, sum(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate, o_shippriority FROM customer, orders, lineitem
WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey AND o_orderdate < date '1995-03-15' AND l_shipdate > date '1995-03-15'
GROUP BY l_orderkey, o_orderdate, o_shippriority ORDER BY revenue DESC, o_orderdate, l_orderkey LIMIT 10
