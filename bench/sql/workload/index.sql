-- The column index of the event analytics workload: scalar columns, the metrics with typed-as-text values, and the normalized document
-- (coldiff.load_norm repairs string scores) with its parts array. After test/col/workload.sql.
CREATE INDEX t_load_col ON t_load USING sift_col (id, entity_id, src_id, prod_id, item_date, updated_at, deleted_at, country_code, mix, segment,
	metrics, comments, (coldiff.load_norm(doc)), (coldiff.wl_txt(doc)))
	WITH (paths = 'metrics.* astext, comments[]{type text}, load_norm.lang astext, load_norm.mapped astext, load_norm.parts[]{category astext, val astext, sign astext, score float8}, wl_txt.parts[]{category astext, score astext}');
VACUUM (FREEZE, ANALYZE) t_load;
