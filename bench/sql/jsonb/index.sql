-- the column index of the benchmark: the three jsonb columns shredded into typed paths (docs/COL-SHRED.md)
CREATE INDEX docs_col ON docs USING sift_col (id, grp, day, attrs, metrics_a, items)
  WITH (paths = 'attrs->channel text, attrs.nested.a.b int4, attrs.tags[] text, metrics_a.* float8, items[]{kind text, sub text, flag text, score float8}');
-- the GIN twins of the same documents
CREATE INDEX docs_gin_items ON docs USING gin (items jsonb_path_ops);
CREATE INDEX docs_gin_attrs ON docs USING gin (attrs jsonb_path_ops);
CREATE INDEX docs_gin_scores ON docs USING gin (metrics_a jsonb_path_ops);
