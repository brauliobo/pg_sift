-- the variant of index.sql for the jsonb_typeof of an astext path: the mixed key (numbers among strings and JSON nulls) is an astext leaf, whose chunks
-- keep the kind (number or string) of each value and the JSON nulls
DROP INDEX IF EXISTS docs_col;
CREATE INDEX docs_col ON docs USING sift_col (id, grp, day, attrs, metrics_a, items)
  WITH (paths = 'attrs->channel text, attrs.nested.a.b int4, attrs.tags[] text, attrs.mixed astext, metrics_a.* float8, items[]{kind text, sub text, flag text, score float8}');
