-- keys 1..nd are expected; 15% of them (hash) have no row yet (arrive in waves below), 5% of the facts name a key above nd (never arrives)
CREATE FUNCTION gen_entity(lo bigint, hi bigint) RETURNS SETOF entity LANGUAGE sql AS $$
  SELECT g::int, gx.word(g, k.s + 1, k.card), 'r' || gx.pick(g, k.s + 2, 6) FROM generate_series(lo, hi) g, gx.knob k WHERE gx.u(g, k.s + 3) >= 0.15 $$;
CREATE FUNCTION gen_fact(lo bigint, hi bigint) RETURNS SETOF fact LANGUAGE sql AS $$
  SELECT g, CASE WHEN gx.u(g, k.s + 11) < 0.05 THEN k.nd + 1 + gx.pick(g, k.s + 12, 100) ELSE gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot) END,
         gx.pick(g, k.s + 13, 100000) / 100.0, gx.ts(g, k.s + 14, '2024-01-01', 365) FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO entity SELECT * FROM gen_entity(1, :nd);
INSERT INTO fact SELECT * FROM gen_fact(1, :nf);
