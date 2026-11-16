-- items = nd = nf/50 (the last 20% are never sold), categories = nd/20, departments = categories/10 (min 3); zipf skew makes some items sell a lot
SELECT greatest(:nd / 20, 5) AS nc \gset
SELECT greatest(:nc / 10, 3) AS ndp \gset
CREATE FUNCTION gen_item(lo bigint, hi bigint) RETURNS SETOF item LANGUAGE sql AS $$
  SELECT g::int, gx.pick(g, k.s + 4, greatest(k.nd / 20, 5)), 'b' || gx.pick(g, k.s + 5, 40), 1 + gx.pick(g, k.s + 6, 10000) / 100.0
  FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_sale(lo bigint, hi bigint) RETURNS SETOF sale LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd * 4 / 5, k.s + 10, k.corr, k.skew, k.hot), gx.pick(g, k.s + 11, 9), gx.pick(g, k.s + 12, 100000) / 100.0, gx.ts(g, k.s + 13, '2024-01-01', 365)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO department SELECT g, gx.word(g, :s + 1, 6), 'd' || g FROM generate_series(1, :ndp) g;
INSERT INTO category SELECT g, gx.pick(g, :s + 2, :ndp), gx.word(g, :s + 3, :card) FROM generate_series(1, :nc) g;
INSERT INTO item SELECT * FROM gen_item(1, :nd);
INSERT INTO sale SELECT * FROM gen_sale(1, :nf);
