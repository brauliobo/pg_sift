-- employees = nd = nf/20; employee i reports to (i - 2) / 5 + 1, employee 1 is the root; the last 10% never file a timesheet
CREATE FUNCTION gen_employee(lo bigint, hi bigint) RETURNS SETOF employee LANGUAGE sql AS $$
  SELECT g::int, CASE WHEN g = 1 THEN NULL ELSE ((g - 2) / 5 + 1)::int END, gx.word(g, k.s + 1, k.card), gx.pick(g, k.s + 2, 6), gx.u(g, k.s + 3) < 0.9
  FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_timesheet(lo bigint, hi bigint) RETURNS SETOF timesheet LANGUAGE sql AS $$
  SELECT g, gx.fkey(g, k.nf, k.nd * 9 / 10, k.s + 10, k.corr, k.skew, k.hot), date '2024-01-01' + gx.pick(g, k.s + 11, 365) - 1, gx.pick(g, k.s + 12, 10)
  FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO employee SELECT * FROM gen_employee(1, :nd);
INSERT INTO timesheet SELECT * FROM gen_timesheet(1, :nf);
