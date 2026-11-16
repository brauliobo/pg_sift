-- sources = nd = nf/100; events fall uniformly into the 12 months, so the heap order inside a partition is not the time order; the last 10% of the sources never report
CREATE FUNCTION gen_event(lo bigint, hi bigint) RETURNS SETOF event LANGUAGE sql AS $$
  SELECT g, gx.ts(g, k.s + 11, '2024-01-01', 366), gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot), gx.pick(g, k.s + 12, 1000),
         CASE WHEN gx.u(g, k.s + 13) < 0.05 THEN 1 ELSE 0 END FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO source SELECT g, gx.word(g, :s + 1, :card), 'r' || gx.pick(g, :s + 2, 6) FROM generate_series(1, :nd) g;
INSERT INTO event SELECT * FROM gen_event(1, :nf);
