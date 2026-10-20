-- the variant of index.sql for j09 and j14: the scores and the item score keep the text `->>` gives (docs/COL-SHRED.md, astext)
DROP INDEX IF EXISTS docs_col;
CREATE INDEX docs_col ON docs USING sift_col (id, grp, day, attrs, scores, items)
  WITH (paths = 'attrs->channel text, attrs.nested.a.b int4, attrs.tags[] text, scores.* astext, items[]{kind text, sub text, flag text, score astext}');
