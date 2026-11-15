-- devices = nd = nf/100, sites = nd/50 (min 5); a reading every 30 s of the fact clock; the last 10% of the devices never report
SELECT greatest(:nd / 50, 5) AS ns \gset
CREATE FUNCTION gen_reading(lo bigint, hi bigint) RETURNS SETOF reading LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, greatest(k.nd * 9 / 10, 1), k.s + 10, k.corr, k.skew, k.hot), timestamp '2024-01-01' + g * interval '30 seconds',
         gx.pick(g, k.s + 11, 100000) / 100.0, CASE WHEN gx.u(g, k.s + 12) < 0.05 THEN gx.pick(g, k.s + 13, 4) ELSE 0 END
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO site SELECT g, 'r' || gx.pick(g, :s + 1, 6), 'tz' || gx.pick(g, :s + 2, 4) FROM generate_series(1, :ns) g;
INSERT INTO device SELECT g, gx.pick(g, :s + 3, :ns), gx.word(g, :s + 4, :card), date '2022-01-01' + gx.pick(g, :s + 5, 700) FROM generate_series(1, :nd) g;
INSERT INTO reading SELECT * FROM gen_reading(1, :nf);
