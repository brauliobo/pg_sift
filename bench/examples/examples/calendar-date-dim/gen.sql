-- the calendar has 731 days from 2023-01-01 (fixed size); sales are on the first 658 days (the last 10% of the calendar has no sale) and 3% on days after the calendar ends; skew puts most sales on early days
CREATE FUNCTION gen_calendar(lo bigint, hi bigint) RETURNS SETOF calendar LANGUAGE sql AS $$
  SELECT d::date, extract(dow FROM d)::int, extract(month FROM d)::int, 'q' || extract(quarter FROM d), extract(dow FROM d) = 0 OR gx.u(g, k.s + 1) < 0.03
  FROM generate_series(lo, hi) g, gx.knob k, LATERAL (SELECT date '2023-01-01' + (g - 1)::int AS d) x $$;
CREATE FUNCTION gen_sale(lo bigint, hi bigint) RETURNS SETOF sale LANGUAGE sql AS $$
  SELECT g, date '2023-01-01' + CASE WHEN gx.u(g, k.s + 11) < 0.03 THEN 731 + gx.pick(g, k.s + 12, 30) - 1 ELSE gx.fkey(g, k.nf, 658, k.s + 10, k.corr, k.skew, k.hot) - 1 END,
         gx.pick(g, k.s + 13, 50), gx.pick(g, k.s + 14, 100000) / 100.0 FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO calendar SELECT * FROM gen_calendar(1, 731);
INSERT INTO sale SELECT * FROM gen_sale(1, :nf);
