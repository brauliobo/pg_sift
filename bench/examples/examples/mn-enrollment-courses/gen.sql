-- students = nd = nf/10, courses = max(20, nd/50), enrollments = nf (a pair may repeat in another term); the last 10% of the students never enroll
SELECT greatest(:nd / 50, 20) AS nc \gset
CREATE FUNCTION gen_enrollment(lo bigint, hi bigint) RETURNS SETOF enrollment LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, 1, k.hot), gx.pick(g, k.s + 11, greatest(k.nd / 50, 20), k.skew), gx.pick(g, k.s + 12, 100),
         't' || gx.pick(g, k.s + 13, 8) FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO student SELECT g, gx.word(g, :s + 1, :card), 'c' || gx.pick(g, :s + 2, 12) FROM generate_series(1, :nd) g;
INSERT INTO course SELECT g, gx.word(g, :s + 3, 8), gx.pick(g, :s + 4, 4) FROM generate_series(1, :nc) g;
INSERT INTO enrollment SELECT * FROM gen_enrollment(1, :nf);
