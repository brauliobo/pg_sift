-- The indexes an application puts on an orders table (btree on the filter columns, a unique one on the number, a GIN on the items),
-- built after the load.
\set ON_ERROR_STOP on
ALTER TABLE orders ADD PRIMARY KEY (id);
CREATE INDEX orders_store_id_placed_at_index ON orders USING btree (tenant_id, placed_at, deleted_at);
CREATE INDEX orders_customer_id_index ON orders USING btree (customer_id);
CREATE UNIQUE INDEX orders_number_index ON orders USING btree (number);
CREATE INDEX orders_items_gin_index ON orders USING gin (items jsonb_path_ops);
CREATE INDEX orders_updated_at_brin ON orders USING brin (updated_at);
