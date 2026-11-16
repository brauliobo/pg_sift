-- dims = nd = nf/20, 10% soft deleted; facts 10% soft deleted; the last 10% of the dims have no fact
CREATE FUNCTION gen_dim(lo bigint, hi bigint) RETURNS SETOF dim LANGUAGE sql AS $$
  SELECT g::int, gx.word(g, k.s + 1, k.card), 'w' || gx.pick(g, k.s + 2, 5), CASE WHEN gx.u(g, k.s + 3) < 0.1 THEN timestamp '2024-06-01' END FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_fact(lo bigint, hi bigint) RETURNS SETOF fact LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot), gx.pick(g, k.s + 11, 100000) / 100.0, gx.ts(g, k.s + 12, '2024-01-01', 365),
         CASE WHEN gx.u(g, k.s + 13) < 0.1 THEN timestamp '2024-07-01' END FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO dim SELECT * FROM gen_dim(1, :nd);
INSERT INTO fact SELECT * FROM gen_fact(1, :nf);
