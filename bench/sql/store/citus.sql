-- Citus on one node, the table in the `columnar` table access method of the citus_columnar extension (append-only, stripes of column chunks, each
-- compressed on its own; its settings are left at their defaults: 10,000-row chunk groups, 150,000-row stripes, zstd level 3). The columns are
-- those of the PostgreSQL layout. The access method takes btree and hash indexes only, so the primary key serves the lookup by id and the title
-- search has no index (no GIN); there is no UPDATE or DELETE on a columnar table. Sections: `table` before the load, `indexes` after it.
--@ table
CREATE EXTENSION citus_columnar;
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
) USING columnar;
--@ indexes
ALTER TABLE orders ADD PRIMARY KEY (id);
