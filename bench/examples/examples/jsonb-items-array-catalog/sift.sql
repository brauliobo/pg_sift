CREATE INDEX doc_col ON doc USING sift_col (channel, at, items);
CREATE INDEX catalog_col ON catalog USING sift_col (sku, category, brand, price);
