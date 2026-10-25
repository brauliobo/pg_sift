-- TPC-H-like Q6, the revenue forecast: a sum of a product over a year, a discount band and a quantity bound
-- label: TPC-H-like Q6 (`sum(l_extendedprice * l_discount)` over one `l_shipdate` year, `l_discount` 0.05 to 0.07, `l_quantity < 24`), 1,000,000 lineitem rows
-- ordered: yes
--@ pg
SELECT sum(l_extendedprice * l_discount) AS revenue FROM lineitem
WHERE l_shipdate >= date '1994-01-01' AND l_shipdate < date '1995-01-01' AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24
