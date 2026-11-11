-- the last 10% of the dimension rows (at least one) have no fact; hot percent take dim 1
CREATE FUNCTION gen_dim(lo bigint, hi bigint) RETURNS SETOF dim LANGUAGE sql AS $$
  SELECT g::int, gx.word(g, k.s + 1, k.card), CASE WHEN g = 1 THEN 'w1' ELSE gx.word(g, k.s + 2, 5) END, g = 1 OR gx.u(g, k.s + 3) < 0.4, gx.pick(g, k.s + 4, 10) FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_fact(lo bigint, hi bigint) RETURNS SETOF fact LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, greatest(k.nd * 9 / 10, 1), k.s + 11, k.corr, k.skew, k.hot),
         gx.pick(g, k.s + 12, 100000) / 100.0, (ARRAY['new', 'paid', 'done', 'void'])[gx.pick(g, k.s + 13, 4)], gx.ts(g, k.s + 14, '2024-01-01', 365)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO dim SELECT * FROM gen_dim(1, :nd);
INSERT INTO fact SELECT * FROM gen_fact(1, :nf);
