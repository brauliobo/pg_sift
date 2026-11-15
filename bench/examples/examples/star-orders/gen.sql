-- customers = nd, items = nd/5, places = nd/50; null_pct of the orders have no store; segment and category have `card` values
CREATE FUNCTION gen_customer(lo bigint, hi bigint) RETURNS SETOF customer LANGUAGE sql AS $$
  SELECT g::int, gx.word(g, k.s + 1, k.card, k.skew), 'c' || gx.pick(g, k.s + 2, 8), gx.pick(g, k.s + 3, 5) FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_orders(lo bigint, hi bigint) RETURNS SETOF orders LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd, k.s + 10, k.corr, k.skew, k.hot), gx.pick(g, k.s + 11, greatest(k.nd / 5, 10), k.skew),
         CASE WHEN gx.isnull(g, k.s + 12, k.null_pct) THEN NULL ELSE gx.pick(g, k.s + 12, greatest(k.nd / 50, 5)) END,
         gx.pick(g, k.s + 13, 9), gx.pick(g, k.s + 14, 100000) / 100.0,
         (ARRAY['new', 'paid', 'shipped', 'done', 'cancelled'])[gx.pick(g, k.s + 15, 5)], gx.ts(g, k.s + 16, '2024-01-01', 365)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO customer SELECT * FROM gen_customer(1, :nd);
INSERT INTO item SELECT g, gx.word(g, :s + 4, :card), 'b' || gx.pick(g, :s + 5, 50), 1 + gx.pick(g, :s + 6, 10000) / 100.0 FROM generate_series(1, greatest(:nd / 5, 10)) g;
INSERT INTO store SELECT g, 'r' || gx.pick(g, :s + 7, 8), 'k' || gx.pick(g, :s + 8, 4) FROM generate_series(1, greatest(:nd / 50, 5)) g;
INSERT INTO orders SELECT * FROM gen_orders(1, :nf);
