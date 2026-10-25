-- TPC-H-like Q1, the pricing summary: eight aggregates over almost all of lineitem, two group keys
-- label: TPC-H-like Q1 (`l_shipdate <= 1998-09-02`, `GROUP BY l_returnflag, l_linestatus`, sums, averages and a count; `sum(price * (1 - discount) * (1 + tax))` is a 38-digit product that the scan sums exactly with a carry), 1,000,000 lineitem rows
-- ordered: yes
--@ pg
SELECT l_returnflag, l_linestatus, sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
       sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, avg(l_discount) AS avg_disc, count(*) AS count_order
FROM lineitem WHERE l_shipdate <= date '1998-12-01' - 90 GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus
