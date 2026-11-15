-- accounts = nd = nf/50; 4 versions of 90 days from 2024-01-01, the last open ended; entries are posted from 2023-12-01 (the first month has no version) over 395 days
CREATE FUNCTION gen_version(lo bigint, hi bigint) RETURNS SETOF account_version LANGUAGE sql AS $$
  SELECT a::int, date '2024-01-01' + (v - 1) * 90, CASE WHEN v = 4 THEN date '9999-12-31' ELSE date '2024-01-01' + v * 90 END,
         gx.word(a * 4 + v, k.s + 2, k.card), gx.pick(a * 4 + v, k.s + 3, 5)
  FROM generate_series(lo, hi) a, generate_series(1, 4) v, gx.knob k $$;
CREATE FUNCTION gen_entry(lo bigint, hi bigint) RETURNS SETOF entry LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd, k.s + 10, k.corr, k.skew, k.hot), date '2023-12-01' + gx.pick(g, k.s + 11, 395) - 1,
         gx.pick(g, k.s + 12, 1000000) / 100.0 - 5000, 'k' || gx.pick(g, k.s + 13, 4)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO account SELECT g, 'k' || gx.pick(g, :s + 1, 3) FROM generate_series(1, :nd) g;
INSERT INTO account_version SELECT * FROM gen_version(1, :nd);
INSERT INTO entry SELECT * FROM gen_entry(1, :nf);
