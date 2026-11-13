-- accounts = nd = nf/20 (uuid = md5 of the ordinal); the last 10% have no event
CREATE FUNCTION acc(n bigint) RETURNS uuid LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$ SELECT md5('acc' || n)::uuid $$;
CREATE FUNCTION gen_account(lo bigint, hi bigint) RETURNS SETOF account LANGUAGE sql AS $$
  SELECT acc(g), g::int, gx.word(g, k.s + 1, k.card), 'r' || gx.pick(g, k.s + 2, 8) FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_event(lo bigint, hi bigint) RETURNS SETOF event LANGUAGE sql AS $$
  SELECT md5('ev' || g)::uuid, g, acc(gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot)), gx.pick(g, k.s + 11, 100000) / 100.0, gx.ts(g, k.s + 12, '2024-01-01', 365)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO account SELECT * FROM gen_account(1, :nd);
INSERT INTO event SELECT * FROM gen_event(1, :nf);
