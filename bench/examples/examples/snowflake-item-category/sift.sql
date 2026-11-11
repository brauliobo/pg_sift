CREATE INDEX sale_col ON sale USING sift_col (item_id, qty, amount, sold_at);
CREATE INDEX item_col ON item USING sift_col (id, category_id, brand);
CREATE INDEX category_col ON category USING sift_col (id, department_id, kind);
CREATE INDEX department_col ON department USING sift_col (id, division, name);
