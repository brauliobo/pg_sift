-- TPC-H-like returned items (TPC-H Q10): customer, orders and lineitem joined, the 20 customers of the largest lost revenue
-- label: TPC-H-like returned items (three tables, one quarter of `o_orderdate`, `l_returnflag = 'R'`, `GROUP BY` customer, `LIMIT 20`), 1,000,000 lineitem rows
-- ordered: yes
--@ pg
SELECT c_custkey, c_name, sum(l_extendedprice * (1 - l_discount)) AS revenue, c_acctbal FROM customer, orders, lineitem
WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND o_orderdate >= date '1993-10-01' AND o_orderdate < date '1994-01-01' AND l_returnflag = 'R'
GROUP BY c_custkey, c_name, c_acctbal ORDER BY revenue DESC, c_custkey LIMIT 20
