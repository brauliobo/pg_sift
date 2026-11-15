-- users = nd = nf/10; the last 15% of the users never have a session; null_pct percent of the sessions are anonymous (NULL user_id), 5% name a user that does not exist
CREATE FUNCTION gen_users(lo bigint, hi bigint) RETURNS SETOF users LANGUAGE sql AS $$
  SELECT g::int, gx.word(g, k.s + 1, 4), 'c' || gx.pick(g, k.s + 2, 12), date '2020-01-01' + gx.pick(g, k.s + 3, 1500) FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_session(lo bigint, hi bigint) RETURNS SETOF session LANGUAGE sql AS $$
  SELECT g, CASE WHEN gx.isnull(g, k.s + 10, k.null_pct) THEN NULL
                 WHEN gx.u(g, k.s + 11) < 0.05 THEN k.nd + 1 + gx.pick(g, k.s + 12, greatest(k.nd / 20, 1))
                 ELSE gx.fkey(g, k.nf, k.nd * 17 / 20, k.s + 13, k.corr, k.skew, k.hot) END,
         gx.ts(g, k.s + 14, '2024-01-01', 365), gx.pick(g, k.s + 15, 3600), 'k' || gx.pick(g, k.s + 16, 5)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO users SELECT * FROM gen_users(1, :nd);
INSERT INTO session SELECT * FROM gen_session(1, :nf);
