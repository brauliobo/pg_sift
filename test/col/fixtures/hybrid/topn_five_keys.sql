-- trap: five sort keys, within the eight of the top-N scan (SIFT_TOPN_MAX_KEYS): the scan and the b-tree walk with an incremental sort are a choice of the cost (314.9 against 318.3 at 4000 rows, about 0.1 ms apart); the rows are the same
-- shape: topn
-- expect: any
-- key: -
SELECT id, tenant_id, source, status, lang FROM orders WHERE memo @@> 'express' ORDER BY tenant_id DESC, source, status NULLS FIRST, lang DESC, id LIMIT 20
