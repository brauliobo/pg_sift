CREATE INDEX sale_item_idx ON sale (item_id);
CREATE INDEX sale_sold_idx ON sale (sold_at);
CREATE INDEX item_category_idx ON item (category_id);
CREATE INDEX category_department_idx ON category (department_id);
