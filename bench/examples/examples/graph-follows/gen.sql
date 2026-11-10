-- people = nd = nf/20; followees are drawn with the skew knob (popular people), followers uniformly; the last 10% of the people follow nobody
CREATE FUNCTION gen_follows(lo bigint, hi bigint) RETURNS SETOF follows LANGUAGE sql AS $$
  SELECT g, gx.pick(g, k.s + 10, k.nd * 9 / 10), gx.pick(g, k.s + 11, k.nd, k.skew), date '2022-01-01' + gx.pick(g, k.s + 12, 1000) FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO person SELECT g, 'c' || gx.pick(g, :s + 1, 12), gx.pick(g, :s + 2, 4) FROM generate_series(1, :nd) g;
INSERT INTO follows SELECT * FROM gen_follows(1, :nf);
