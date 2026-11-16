-- items = nd = nf/10, tags = max(20, nd/50), about 3 tags per item (zipf skew: a few tags are very common)
SELECT greatest(:nd / 50, 20) AS nt \gset
CREATE FUNCTION gen_item_tag(lo bigint, hi bigint) RETURNS TABLE (item_id int, tag_id int) LANGUAGE sql AS $$
  SELECT DISTINCT g::int, gx.pick(g * 4 + j, k.s + 2, greatest(k.nd / 50, 20), k.skew) FROM generate_series(lo, hi) g, generate_series(1, 3) j, gx.knob k $$;
CREATE FUNCTION gen_event(lo bigint, hi bigint) RETURNS SETOF event LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd, k.s + 10, k.corr, 1, k.hot), gx.pick(g, k.s + 11, 100000) / 100.0, gx.ts(g, k.s + 12, '2024-01-01', 365) FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO tag SELECT g, 'w' || g, gx.word(g, :s + 3, 8) FROM generate_series(1, :nt) g;
INSERT INTO item SELECT g, gx.word(g, :s + 4, :card), gx.pick(g, :s + 5, 100) FROM generate_series(1, :nd) g;
INSERT INTO item_tag SELECT * FROM gen_item_tag(1, :nd);
INSERT INTO event SELECT * FROM gen_event(1, :nf);
