-- The column index of the review analytics workload: scalar columns, the ratings with typed-as-text values, and the normalized document
-- (coldiff.wl_norm repairs string scores) with its opinions array. After test/col/workload.sql.
CREATE INDEX t_wl_col ON t_wl USING sift_col (id, entity_id, source_id, product_id, item_date, updated_at, deleted_at, country_code, composition, segment,
	ratings, comments, (coldiff.wl_norm(doc)))
	WITH (paths = 'ratings.* astext, comments[]{type text}, wl_norm.lang astext, wl_norm.mapped astext, wl_norm.opinions[]{category astext, subrating astext, polarity astext, score float8}');
VACUUM (FREEZE, ANALYZE) t_wl;
