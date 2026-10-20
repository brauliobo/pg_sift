-- the variant of index.sql for the jsonb_typeof of an astext path: the mixed key (numbers among strings and JSON nulls) is an astext leaf, whose chunks
-- keep the kind (number or string) of each value and the JSON nulls (docs/COL.md 2.2)
DROP INDEX IF EXISTS docs_col;
CREATE INDEX docs_col ON docs USING sift_col (id, grp, day, attrs, scores, items)
  WITH (paths = 'attrs->channel text, attrs.nested.a.b int4, attrs.tags[] text, attrs.mixed astext, scores.* float8, items[]{kind text, sub text, flag text, score float8}');
