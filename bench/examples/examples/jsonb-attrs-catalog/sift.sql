CREATE INDEX event_col ON event USING sift_col (((attrs ->> 'sku')::int), ((attrs ->> 'qty')::int), (attrs ->> 'channel'), at);
CREATE INDEX catalog_col ON catalog USING sift_col (sku, category, brand, price);
