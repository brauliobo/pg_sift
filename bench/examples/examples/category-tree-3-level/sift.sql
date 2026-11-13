CREATE INDEX listing_col ON listing USING sift_col (category_id, price, status, listed_at);
CREATE INDEX category_col ON category USING sift_col (id, parent_id, depth, name);
