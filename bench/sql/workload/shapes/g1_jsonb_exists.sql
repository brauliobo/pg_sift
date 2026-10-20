-- GAP 1: key-exists on a document held by the index (jsonb_exists / ? operator) keeps the normal plan.
-- Expected: Custom Scan (SiftAgg), like `jsonb_typeof(doc -> 'mapped') IS NOT NULL` which is exactly equivalent.
\echo -- not pushed: jsonb_exists
EXPLAIN (COSTS OFF) SELECT (CASE WHEN jsonb_exists(coldiff.doc_norm(r.doc), 'flag_a') THEN 'm' ELSE 'u' END) k, count(*) FROM t_work r WHERE r.deleted_at IS NULL GROUP BY 1;
\echo -- not pushed: ? operator
EXPLAIN (COSTS OFF) SELECT (CASE WHEN coldiff.doc_norm(r.doc) ? 'flag_a' THEN 'm' ELSE 'u' END) k, count(*) FROM t_work r WHERE r.deleted_at IS NULL GROUP BY 1;
\echo -- pushed: jsonb_typeof(doc -> key) IS NOT NULL
EXPLAIN (COSTS OFF) SELECT (CASE WHEN jsonb_typeof(coldiff.doc_norm(r.doc) -> 'flag_a') IS NOT NULL THEN 'm' ELSE 'u' END) k, count(*) FROM t_work r WHERE r.deleted_at IS NULL GROUP BY 1;
