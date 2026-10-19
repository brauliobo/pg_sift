-- PostgreSQL with pg_sift, the table itself in column format (sift_table): no separate column index; the primary key serves the lookup by
-- id and a BM25 index serves the title search.
--@ table
CREATE EXTENSION pg_sift;
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
) USING sift_table;
--@ indexes
ALTER TABLE orders ADD PRIMARY KEY (id);
CREATE INDEX orders_title ON orders USING sift (title);
