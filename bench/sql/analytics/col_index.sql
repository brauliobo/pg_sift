-- The sift_col index of the analytics corpus: the default of `bench/bench analytics --col-index`, which takes a statement or @FILE
-- instead. Lines starting with -- are dropped; a build of pg_sift without the sift_col access method skips the index and measures the
-- baseline plan alone. PLACEHOLDER: the columns, paths and options are the ones of the sift_col branch (docs/COL.md) once it lands;
-- edit this file (or pass --col-index) when its DDL differs.
CREATE INDEX orders_sift_col ON orders USING sift_col (store_id, placed_at, source, deleted_at, country_code, lang, attributes, metrics, items, notes);
