-- accounts = nd = nf/50 spread over max(20, nd/200) tenants with the skew knob (tenant 1 is the big one); an account has 1 to 3 products; the last 10% of the accounts have no usage
SELECT greatest(:nd / 200, 20) AS nt \gset
CREATE FUNCTION gen_account(lo bigint, hi bigint) RETURNS SETOF account LANGUAGE sql AS $$
  SELECT gx.pick(g, k.s + 2, greatest(k.nd / 200, 20), k.skew), g::int, gx.pick(g, k.s + 3, 4), (ARRAY['active', 'trial', 'closed'])[gx.pick(g, k.s + 4, 3)]
  FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_usage(lo bigint, hi bigint) RETURNS SETOF usage LANGUAGE sql AS $$
  SELECT g, gx.pick(a, k.s + 2, greatest(k.nd / 200, 20), k.skew), a, gx.pick(g, k.s + 11, 1000), gx.ts(g, k.s + 12, '2024-01-01', 365)
  FROM generate_series(lo, hi) g, gx.knob k, LATERAL (SELECT gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, 1, k.hot) AS a) x $$;
INSERT INTO tenant SELECT g, gx.word(g, :s + 1, 4), 'r' || gx.pick(g, :s + 5, 5) FROM generate_series(1, :nt) g;
INSERT INTO account SELECT * FROM gen_account(1, :nd);
INSERT INTO grant_row SELECT t.tenant_id, t.id, 'p' || p, gx.u(t.id * 10 + p, :s + 6) < 0.8
 FROM account t, generate_series(1, 3) p WHERE p = 1 OR gx.u(t.id * 10 + p, :s + 7) < 0.4;
INSERT INTO usage SELECT * FROM gen_usage(1, :nf);
