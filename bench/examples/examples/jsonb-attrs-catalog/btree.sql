CREATE INDEX event_sku_idx ON event (((attrs ->> 'sku')::int));
CREATE INDEX event_attrs_idx ON event USING gin (attrs jsonb_path_ops);
CREATE INDEX catalog_category_idx ON catalog (category);
