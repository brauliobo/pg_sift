-- 4 lines per order (nf and nb are multiples of 4), headers = nd = nf/4, 80% of the lines are shipped; heap order follows (order_id, line_no)
CREATE FUNCTION gen_header(lo bigint, hi bigint) RETURNS SETOF header LANGUAGE sql AS $$
  SELECT g, gx.pick(g, k.s + 1, greatest(k.nd / 10, 10), k.skew), (ARRAY['open', 'paid', 'closed'])[gx.pick(g, k.s + 2, 3)], date '2024-01-01' + gx.pick(g, k.s + 3, 365) - 1
  FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_line(lo bigint, hi bigint) RETURNS SETOF order_line LANGUAGE sql AS $$
  SELECT (g - 1) / 4 + 1, ((g - 1) % 4 + 1)::int, gx.pick(g, k.s + 10, 500, k.skew), gx.pick(g, k.s + 11, 9), gx.pick(g, k.s + 12, 10000) / 100.0 FROM generate_series(lo, hi) g, gx.knob k $$;
CREATE FUNCTION gen_shipment(lo bigint, hi bigint) RETURNS SETOF shipment LANGUAGE sql AS $$
  SELECT (g - 1) / 4 + 1, ((g - 1) % 4 + 1)::int, gx.word(g, k.s + 13, k.card), date '2024-01-05' + gx.pick(g, k.s + 14, 365) - 1
  FROM generate_series(lo, hi) g, gx.knob k WHERE gx.u(g, k.s + 15) < 0.8 $$;
INSERT INTO header SELECT * FROM gen_header(1, :nd);
INSERT INTO order_line SELECT * FROM gen_line(1, :nf);
INSERT INTO shipment SELECT * FROM gen_shipment(1, :nf);
