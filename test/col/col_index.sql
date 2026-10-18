-- The column index of the analytics corpus on sift_col (scripts/coldiff.sh --col-index): the paths grammar of docs/COL.md section 6.
CREATE INDEX orders_sift_col ON orders USING sift_col (id, store_id, placed_at, status, source, country_code, lang, deleted_at, attributes, metrics, items, notes)
	WITH (paths = 'attributes->channel text, attributes->device text, attributes->locale text, attributes->normalized bool, metrics.* float8, items[]{category text, sku text, kind text, qty int4, price float8, discount float8}, notes[]{type text, author_role text, text text}');
