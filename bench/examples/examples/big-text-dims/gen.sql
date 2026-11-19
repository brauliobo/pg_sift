-- docs = nd = nf/20; description is a kilobyte of text with a 'needle<k>' marker, title 'Title <k> ...'; the last 10% of the docs have no page_view
CREATE FUNCTION gen_doc(lo bigint, hi bigint) RETURNS SETOF doc LANGUAGE sql AS $$
  SELECT g::int, gx.word(g, k.s + 1, k.card), 'Title ' || gx.pick(g, k.s + 2, 500) || ' ' || md5(g::text),
         'needle' || gx.pick(g, k.s + 3, 100) || ' ' || repeat(md5(g::text) || ' ', 30) FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_view(lo bigint, hi bigint) RETURNS SETOF page_view LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot), gx.pick(g, k.s + 11, 600), gx.ts(g, k.s + 12, '2024-01-01', 365) FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO doc SELECT * FROM gen_doc(1, :nd);
INSERT INTO page_view SELECT * FROM gen_view(1, :nf);
