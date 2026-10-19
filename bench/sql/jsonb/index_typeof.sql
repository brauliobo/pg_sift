-- the variant of index.sql for j16: the mixed key (numbers among strings and JSON nulls) is a float8 leaf, and jsonb_typeof(attrs->'mixed') = 'number'
-- is answered from the JSON types that column format minor 4 keeps for the values the leaf cannot hold (docs/COL-JSONB-31.md 5.1)
DROP INDEX IF EXISTS docs_col;
CREATE INDEX docs_col ON docs USING sift_col (id, grp, day, attrs, scores, items)
  WITH (paths = 'attrs->channel text, attrs.nested.a.b int4, attrs.tags[] text, attrs.mixed float8, scores.* float8, items[]{kind text, sub text, flag text, score float8}');
