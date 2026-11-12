-- pg_ivm: immediate incremental views of the join of events and the maintained pair table, one for each family of classes (a tenant's classes read one tenant's rows)
CREATE EXTENSION pg_ivm;
CREATE INDEX events_dim ON events (dim_a_id);
SELECT pgivm.create_immv('im_main', $q$
  SELECT p.tenant_id, e.dim_a_id, date_trunc('month', e.created_at::timestamp) AS month, count(*) AS n, sum(e.amount) AS amount_sum
    FROM events e JOIN pairs p ON p.dim_a_id = e.dim_a_id GROUP BY p.tenant_id, e.dim_a_id, date_trunc('month', e.created_at::timestamp) $q$);
SELECT pgivm.create_immv('im_score', $q$
  SELECT p.tenant_id, e.score, count(*) AS n FROM events e JOIN pairs p ON p.dim_a_id = e.dim_a_id GROUP BY p.tenant_id, e.score $q$);
SELECT pgivm.create_immv('im_chan', $q$
  SELECT p.tenant_id, e.channel_id, e.region, count(*) AS n FROM events e JOIN pairs p ON p.dim_a_id = e.dim_a_id GROUP BY p.tenant_id, e.channel_id, e.region $q$);
SELECT pgivm.create_immv('im_kind', $q$
  SELECT p.tenant_id, date_trunc('month', e.created_at::timestamp) AS month, e.kind, count(*) AS n
    FROM events e JOIN pairs p ON p.dim_a_id = e.dim_a_id GROUP BY p.tenant_id, date_trunc('month', e.created_at::timestamp), e.kind $q$);
VACUUM (FREEZE, ANALYZE) im_main, im_score, im_chan, im_kind;
