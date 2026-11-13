CREATE INDEX orders_customer_idx ON orders (customer_id);
CREATE INDEX orders_item_idx ON orders (item_id);
CREATE INDEX orders_store_idx ON orders (place_id);
CREATE INDEX orders_created_idx ON orders (created_at);
CREATE INDEX customer_segment_idx ON customer (segment);
CREATE INDEX item_category_idx ON item (category);
