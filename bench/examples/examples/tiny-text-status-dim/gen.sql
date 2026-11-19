-- 5 statuses; status_code follows the skew knob (zipf: most facts are 'open'); a sixth code 'legacy' on 1% of the facts has no status row
CREATE FUNCTION gen_fact(lo bigint, hi bigint) RETURNS SETOF fact LANGUAGE sql AS $$
  SELECT g, CASE WHEN gx.u(g, k.s + 10) < 0.01 THEN 'legacy' ELSE (ARRAY['open', 'pending', 'paid', 'shipped', 'closed'])[gx.pick(g, k.s + 11, 5, k.skew)] END,
         gx.pick(g, k.s + 12, 100000) / 100.0, gx.ts(g, k.s + 13, '2024-01-01', 365), gx.pick(g, k.s + 14, greatest(k.nf / 100, 10)::int) FROM generate_series(lo, hi) g, gx.knob k $$;
INSERT INTO status VALUES ('open', 'Open', false, 1), ('pending', 'Pending', false, 2), ('paid', 'Paid', false, 3), ('shipped', 'Shipped', false, 4), ('closed', 'Closed', true, 5);
INSERT INTO fact SELECT * FROM gen_fact(1, :nf);
