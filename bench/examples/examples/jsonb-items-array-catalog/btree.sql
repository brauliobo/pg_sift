CREATE INDEX doc_items_idx ON doc USING gin (items jsonb_path_ops);
CREATE INDEX doc_at_idx ON doc (at);
CREATE INDEX catalog_category_idx ON catalog (category);
