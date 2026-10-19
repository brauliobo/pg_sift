-- PostgreSQL as it is without pg_sift: a heap table with the indexes a team puts on it for these reports and this search page
-- (primary key, the date, country and status filters, a GIN full-text index). Sections: `table` before the load, `indexes` after it.
--@ table
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
CREATE INDEX orders_created ON orders (created_at);
CREATE INDEX orders_country_status ON orders (country, status);
CREATE INDEX orders_title ON orders USING gin (to_tsvector('simple', title));
