-- The 20 latest events of the members of one tenant: no rollup or incremental view answers it (rows are not aggregates)
-- order: 3
--@ events
SELECT id, created_at FROM {src} WHERE {pred} ORDER BY created_at DESC, id DESC LIMIT 20
--@ lateral
SELECT id, created_at FROM (SELECT l.id, l.created_at FROM (SELECT member FROM tenant_pairs WHERE tenant_id = {t}) m,
  LATERAL (SELECT id, created_at FROM events WHERE dim_a_id = m.member ORDER BY created_at DESC, id DESC LIMIT 20) l) x
ORDER BY created_at DESC, id DESC LIMIT 20
