-- The column index of the event analytics workload: scalar columns, the metrics with typed-as-text values, and the normalized document
-- (coldiff.doc_norm repairs string scores) with its parts array. After test/col/workload.sql.
CREATE INDEX t_load_col ON t_work USING sift_col (id, dim_a_id, foreign_id, foreign_id_b, item_date, updated_at, deleted_at, country_code, mix, segment,
	metrics, tags_a, (coldiff.doc_norm(doc)), (coldiff.wl_txt(doc)))
	WITH (paths = 'metrics.* astext, tags_a[]{type text}, doc_norm.lang astext, doc_norm.flag_a astext, doc_norm.parts[]{category astext, val astext, tone astext, score float8}, wl_txt.parts[]{category astext, score astext}');
VACUUM (FREEZE, ANALYZE) t_work;
