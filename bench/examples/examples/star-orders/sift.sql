-- unconnected: one sift_col index on the fact over its keys and measures, one on each dimension that is grouped or filtered
CREATE INDEX orders_col ON orders USING sift_col (customer_id, item_id, place_id, status, qty, amount, created_at);
CREATE INDEX customer_col ON customer USING sift_col (id, segment, country, tier);
CREATE INDEX item_col ON item USING sift_col (id, category, brand);
CREATE INDEX store_col ON store USING sift_col (id, region, kind);
