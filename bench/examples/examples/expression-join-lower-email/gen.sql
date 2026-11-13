-- people = nd = nf/10 with unique folded emails; contacts spell the email of a member in upper or mixed case; 10% name an email that no member has
CREATE FUNCTION gen_member(lo bigint, hi bigint) RETURNS SETOF member LANGUAGE sql AS $$
  SELECT g::int, 'user' || g || '@example.com', gx.pick(g, k.s + 1, 4), 'c' || gx.pick(g, k.s + 2, 12) FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_contact(lo bigint, hi bigint) RETURNS SETOF contact LANGUAGE sql AS $$
  SELECT g, CASE WHEN gx.u(g, k.s + 11) < 0.5 THEN upper(e) ELSE 'User' || m || '@Example.com' END, gx.ts(g, k.s + 12, '2024-01-01', 365), gx.word(g, k.s + 13, k.card)
  FROM generate_series(lo, hi) g, gx.knob k, LATERAL (SELECT gx.fkey(g, k.nf, k.nd * 10 / 9, k.s + 10, k.corr, k.skew, k.hot) AS m) x, LATERAL (SELECT 'user' || m || '@example.com' AS e) y $$;
INSERT INTO member SELECT * FROM gen_member(1, :nd);
INSERT INTO contact SELECT * FROM gen_contact(1, :nf);
