-- The column index of the analytics corpus on the test access method (sift_colstub, scripts/coldiff.sh --col-index): the paths
-- grammar of src/sift_aggstub.c. A real sift_col index is the statement of the analytics directory (col_index.sql).
CREATE INDEX orders_sift_col ON orders USING sift_col (id, tenant_id, placed_at, status, source, country_code, lang, deleted_at, attributes, metrics, items, notes)
	WITH (paths = 'attributes: channel text, device text, locale text, normalized bool; metrics: *:float8; items: []{category text, sku text, kind text, qty int4, price float8, discount float8}; notes: []{type text, author_kind text, text text}');
