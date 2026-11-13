-- dims = nd = nf/20; 2% of the keys appear twice (version 2 with other attributes) and 0.1% three times; the last 10% of the keys have no fact
CREATE FUNCTION gen_dim(lo bigint, hi bigint) RETURNS SETOF dim LANGUAGE sql AS $$
  SELECT g::int, gx.word(g * 10 + v, k.s + 1, k.card), 'w' || gx.pick(g * 10 + v, k.s + 2, 5), v::int
  FROM generate_series(lo, hi) g, gx.knob k, generate_series(1, 3) v
  WHERE v = 1 OR (v = 2 AND gx.u(g, k.s + 3) < 0.02) OR (v = 3 AND gx.u(g, k.s + 3) < 0.001) $$;
CREATE FUNCTION gen_fact(lo bigint, hi bigint) RETURNS SETOF fact LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot), gx.pick(g, k.s + 11, 100000) / 100.0, gx.ts(g, k.s + 12, '2024-01-01', 365) FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO dim SELECT * FROM gen_dim(1, :nd);
INSERT INTO fact SELECT * FROM gen_fact(1, :nf);
