-- accounts = nd = nf/10; profiles exist for 90% of them (hash); the last 10% of the accounts have no activity
CREATE FUNCTION gen_profile(lo bigint, hi bigint) RETURNS SETOF profile LANGUAGE sql AS $$
  SELECT g::int, gx.pick(g, k.s + 3, 4), 'l' || gx.pick(g, k.s + 4, 10), gx.pick(g, k.s + 5, 2000) FROM generate_series(lo, hi) g, gx.knob k WHERE gx.u(g, k.s + 6) < 0.9 $$;
CREATE FUNCTION gen_activity(lo bigint, hi bigint) RETURNS SETOF activity LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot), gx.pick(g, k.s + 11, 100), gx.ts(g, k.s + 12, '2024-01-01', 365) FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO account SELECT g, gx.word(g, :s + 1, :card), 'r' || gx.pick(g, :s + 2, 6) FROM generate_series(1, :nd) g;
INSERT INTO profile SELECT * FROM gen_profile(1, :nd);
INSERT INTO activity SELECT * FROM gen_activity(1, :nf);
