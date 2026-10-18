-- The sift_col index of the analytics corpus: the default of `bench/bench analytics --col-index` and of `scripts/coldiff.sh --suite analytics`
-- (a statement or @FILE instead with --col-index). Lines starting with -- are dropped; a build of pg_sift without the sift_col access
-- method skips the index and measures the baseline plan alone. The paths grammar is docs/COL.md section 6.
CREATE INDEX orders_sift_col ON orders USING sift_col (id, tenant_id, placed_at, status, source, country_code, lang, deleted_at, attributes, metrics, items, notes)
	WITH (paths = 'attributes->channel text, attributes->device text, attributes->locale text, attributes->normalized bool, metrics.* float8, items[]{category text, sku text, kind text, qty int4, price float8, discount float8}, notes[]{type text, author_kind text, text text}');
