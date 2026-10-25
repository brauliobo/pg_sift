-- The TPC-H-like dataset of the cases (bench/cases.tsv, dataset `tpch`): customer, orders and lineitem with the columns, value ranges and key relations of TPC-H, generated here
-- (this is not dbgen: the distributions are simplified, 4 lines for each order, so the answers are not the published ones and the numbers are not TPC-H results).
--
--   psql -v n=1000000 -f gen.sql
--
-- n is the number of lineitem rows (6,000,000 at TPC-H scale factor 1): n/4 orders and n/40 customers. Every draw is a hash of (row, draw number): the same arguments give the same rows.
\set ON_ERROR_STOP on
\if :{?n}
\else
\set n 1000000
\endif
CREATE FUNCTION pg_temp.u(bigint, int) RETURNS float8 LANGUAGE sql IMMUTABLE PARALLEL SAFE
  AS $$ SELECT (hashint8extended($1, $2::bigint) & 16777215)::float8 / 16777216 $$;

CREATE TABLE customer AS
SELECT c::int AS c_custkey, 'Customer#' || lpad(c::text, 9, '0') AS c_name, (pg_temp.u(c, 1) * 25)::int AS c_nationkey,
       (ARRAY['AUTOMOBILE','BUILDING','FURNITURE','HOUSEHOLD','MACHINERY'])[1 + (pg_temp.u(c, 2) * 5)::int] AS c_mktsegment,
       round((pg_temp.u(c, 3) * 10999 - 999)::numeric, 2)::numeric(12,2) AS c_acctbal
FROM generate_series(1, greatest(:n / 40, 1000)) c;

CREATE TABLE orders AS
SELECT k::int AS o_orderkey, 1 + (pg_temp.u(k, 4) * greatest(:n / 40, 1000))::int AS o_custkey,
       (ARRAY['O','F','P'])[1 + (pg_temp.u(k, 5) * 3)::int] AS o_orderstatus,
       round((pg_temp.u(k, 6) * 400000 + 800)::numeric, 2)::numeric(12,2) AS o_totalprice,
       date '1992-01-01' + (pg_temp.u(k, 7) * 2406)::int AS o_orderdate,
       (ARRAY['1-URGENT','2-HIGH','3-MEDIUM','4-NOT SPECIFIED','5-LOW'])[1 + (pg_temp.u(k, 8) * 5)::int] AS o_orderpriority,
       0 AS o_shippriority
FROM generate_series(1, :n / 4) k;

CREATE TABLE lineitem AS
SELECT l.k::int AS l_orderkey, l.n::int AS l_linenumber,
       (1 + pg_temp.u(l.g, 9) * 49)::int::numeric(12,2) AS l_quantity,
       round((900 + pg_temp.u(l.g, 10) * 100000)::numeric, 2)::numeric(12,2) AS l_extendedprice,
       round((pg_temp.u(l.g, 11) * 0.10)::numeric, 2)::numeric(12,2) AS l_discount,
       round((pg_temp.u(l.g, 12) * 0.08)::numeric, 2)::numeric(12,2) AS l_tax,
       l.od + 1 + (pg_temp.u(l.g, 13) * 121)::int AS l_shipdate,
       l.od + 30 + (pg_temp.u(l.g, 14) * 60)::int AS l_commitdate,
       l.od + 2 + (pg_temp.u(l.g, 13) * 121)::int + 1 + (pg_temp.u(l.g, 15) * 29)::int AS l_receiptdate,
       (ARRAY['R','A','N'])[1 + (pg_temp.u(l.g, 16) * 3)::int] AS l_returnflag,
       (ARRAY['O','F'])[1 + (pg_temp.u(l.g, 17) * 2)::int] AS l_linestatus,
       (ARRAY['AIR','FOB','MAIL','RAIL','REG AIR','SHIP','TRUCK'])[1 + (pg_temp.u(l.g, 18) * 7)::int] AS l_shipmode,
       (ARRAY['COLLECT COD','DELIVER IN PERSON','NONE','TAKE BACK RETURN'])[1 + (pg_temp.u(l.g, 19) * 4)::int] AS l_shipinstruct
FROM (SELECT g, 1 + (g - 1) / 4 AS k, 1 + (g - 1) % 4 AS n, date '1992-01-01' + (pg_temp.u(1 + (g - 1) / 4, 7) * 2406)::int AS od FROM generate_series(1, :n) g) l;

ALTER TABLE customer ADD PRIMARY KEY (c_custkey);
ALTER TABLE orders ADD PRIMARY KEY (o_orderkey);
ALTER TABLE lineitem ADD PRIMARY KEY (l_orderkey, l_linenumber);
CREATE INDEX orders_custkey ON orders (o_custkey);
CREATE INDEX orders_orderdate ON orders (o_orderdate);
CREATE INDEX lineitem_shipdate ON lineitem (l_shipdate);
CREATE INDEX lineitem_receiptdate ON lineitem (l_receiptdate);
CREATE INDEX lineitem_col ON lineitem USING sift_col (l_shipdate, l_commitdate, l_receiptdate, l_returnflag, l_linestatus, l_shipmode, l_quantity, l_extendedprice, l_discount, l_tax);
CREATE INDEX orders_col ON orders USING sift_col (o_orderdate, o_orderpriority, o_orderstatus, o_custkey, o_totalprice);
CREATE INDEX customer_col ON customer USING sift_col (c_mktsegment, c_nationkey, c_acctbal);
VACUUM (FREEZE, ANALYZE) customer, orders, lineitem;
