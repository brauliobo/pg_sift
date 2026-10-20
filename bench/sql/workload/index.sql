-- The column index of the check analytics workload: scalar columns, the metrics_a with typed-as-text values, and the normalized document
-- (coldiff.doc_norm repairs string scores) with its parts array. After test/col/workload.sql.
CREATE INDEX t_work_col ON t_work USING sift_col (id, dim_a_id, lookup_id, foreign_id_b, item_date, updated_at, deleted_at, country_code, label_a, segment,
	metrics_a, tags_a, (coldiff.doc_norm(doc)), (coldiff.wl_txt(doc)))
	WITH (paths = 'metrics_a.* astext, tags_a[]{type text}, doc_norm.lang astext, doc_norm.flag_a astext, doc_norm.parts[]{category astext, value_num astext, tone astext, score float8}, wl_txt.parts[]{category astext, score astext}');
VACUUM (FREEZE, ANALYZE) t_work;
