-- authors = nd = nf/20; a body is 12 words 'w<k>' of a 300-word vocabulary drawn with the skew knob (a few very common words); the last 10% of the authors have no document
CREATE FUNCTION gen_document(lo bigint, hi bigint) RETURNS SETOF document LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, 1, k.hot),
         (SELECT string_agg('w' || gx.pick(g * 16 + j, k.s + 11, 300, k.skew), ' ') FROM generate_series(1, 12) j), gx.ts(g, k.s + 12, '2024-01-01', 365)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO author SELECT g, 'c' || gx.pick(g, :s + 1, 12), gx.pick(g, :s + 2, 4) FROM generate_series(1, :nd) g;
INSERT INTO document SELECT * FROM gen_document(1, :nf);
