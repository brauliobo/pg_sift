-- sift.facet_counts: terms with a top N, a range, a histogram, a date histogram and a drill-down selection over the match set of a search
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT * FROM sift.facet_counts('orders', 'memo', 'late OR missing OR refund',
  '[{"name": "store", "expr": "store_id", "size": 5, "missing": true}, {"name": "status", "expr": "status", "missing": true, "selected": ["paid", "shipped"]},
    {"name": "total", "expr": "total", "edges": [100, 300]}, {"name": "month", "expr": "placed_at", "unit": "month"},
    {"name": "disc", "expr": "discount", "interval": 50, "fill": true}]', 'total > 20', true)
