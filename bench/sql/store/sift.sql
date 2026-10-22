-- PostgreSQL with pg_sift on the same heap table: a sift_col index over the columns the reports read (plus the month of the order, which
-- is what the monthly reports group by) and a BM25 index on the title; the primary key serves the lookup by id. No other index.
--@ table
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE TABLE orders (
  id          bigint        NOT NULL,
  customer_id int           NOT NULL,
  country     text          NOT NULL,
  category    text          NOT NULL,
  status      text          NOT NULL,
  amount      numeric(10,2) NOT NULL,
  quantity    smallint      NOT NULL,
  created_at  timestamp     NOT NULL,
  title       text          NOT NULL
);
--@ indexes
ALTER TABLE orders ADD PRIMARY KEY (id);
CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, quantity, created_at, (date_trunc('month', created_at)::date));
CREATE INDEX orders_title ON orders USING sift (title);
