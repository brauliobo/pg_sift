-- TPC-H-like shipping modes (TPC-H Q12): orders and lineitem joined, conditional counts by ship mode
-- label: TPC-H-like shipping modes (two tables, two ship modes, three date comparisons, `sum(CASE ..)` of the order priority by `l_shipmode`; lookup tables loaded by the query, not kept from an earlier one), 1,000,000 lineitem rows
-- ordered: yes
-- set: sift.agg_lookup_cache_kb = 0; sift.agg_lookup_shared_kb = 0
--@ pg
SELECT l_shipmode, sum(CASE WHEN o_orderpriority IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END) AS high_line_count, sum(CASE WHEN o_orderpriority NOT IN ('1-URGENT', '2-HIGH') THEN 1 ELSE 0 END) AS low_line_count
FROM orders, lineitem WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL', 'SHIP') AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate
AND l_receiptdate >= date '1994-01-01' AND l_receiptdate < date '1995-01-01' GROUP BY l_shipmode ORDER BY l_shipmode
