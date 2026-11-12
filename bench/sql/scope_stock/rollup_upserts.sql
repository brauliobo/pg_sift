INSERT INTO r_month AS r SELECT dim_a_id, date_trunc('month', created_at)::date, count(*), sum(amount) FROM @ROWS@ GROUP BY 1, 2
  ON CONFLICT (dim_a_id, month) DO UPDATE SET n = r.n + excluded.n, amount_sum = r.amount_sum + excluded.amount_sum;
INSERT INTO r_score AS r SELECT dim_a_id, score, count(*) FROM @ROWS@ GROUP BY 1, 2
  ON CONFLICT (dim_a_id, score) DO UPDATE SET n = r.n + excluded.n;
INSERT INTO r_chan AS r SELECT dim_a_id, channel_id, region, count(*) FROM @ROWS@ GROUP BY 1, 2, 3
  ON CONFLICT (dim_a_id, channel_id, region) DO UPDATE SET n = r.n + excluded.n;
INSERT INTO r_kind AS r SELECT dim_a_id, date_trunc('month', created_at)::date, kind, count(*) FROM @ROWS@ GROUP BY 1, 2, 3
  ON CONFLICT (dim_a_id, month, kind) DO UPDATE SET n = r.n + excluded.n;
