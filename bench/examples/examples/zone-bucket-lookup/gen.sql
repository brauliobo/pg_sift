-- zones = nd = nf/500 with keys 'z-<n>'; about 500 readings per zone; the last 10% of the zones are empty
CREATE FUNCTION zk(n bigint) RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE AS $$ SELECT 'z-' || lpad(n::text, 7, '0') $$;
CREATE FUNCTION gen_zone(lo bigint, hi bigint) RETURNS SETOF zone LANGUAGE sql AS $$
  SELECT zk(g), 'r' || gx.pick(g, k.s + 1, 8), gx.word(g, k.s + 2, k.card), gx.pick(g, k.s + 3, 1000) FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_reading(lo bigint, hi bigint) RETURNS SETOF reading LANGUAGE sql AS $$
  SELECT g, zk(gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot)), gx.pick(g, k.s + 11, 100), gx.ts(g, k.s + 12, '2024-01-01', 365) FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO zone SELECT * FROM gen_zone(1, :nd);
INSERT INTO reading SELECT * FROM gen_reading(1, :nf);
