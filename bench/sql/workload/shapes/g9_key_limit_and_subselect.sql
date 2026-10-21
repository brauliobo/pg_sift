-- Status (3.2): fixed: eight keys, and x = ANY (ARRAY (sub-select)) is pushed
-- GAP 9: more than 5 group keys in one scan silently falls back to the heap (no message, no setting to raise it).
-- Expected: SiftAgg for 6 keys (or an EXPLAIN note that names the limit).
\echo -- 5 keys: pushed
EXPLAIN (COSTS OFF) SELECT entity_id, src_id, prod_id, country_code, mix, count(*) FROM t_load r WHERE deleted_at IS NULL GROUP BY 1, 2, 3, 4, 5;
\echo -- 6 keys: not pushed
EXPLAIN (COSTS OFF) SELECT entity_id, src_id, prod_id, country_code, mix, item_date, count(*) FROM t_load r WHERE deleted_at IS NULL GROUP BY 1, 2, 3, 4, 5, 6;
-- GAP 10 (minor): `= ANY(ARRAY(SELECT ...))` is not pushed (InitPlan array), while `IN (SELECT ...)` is (Lookup Joins: t_load_entity (semi)) and so is a constant array.
-- Expected: the same semi lookup for the ARRAY(sub-select) spelling, which the query builders emit for `where_any`.
\echo -- sub-select IN: pushed
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_load r WHERE entity_id IN (SELECT id FROM t_load_entity WHERE type = 'accommodation') AND deleted_at IS NULL;
\echo -- ARRAY(sub-select): not pushed
EXPLAIN (COSTS OFF) SELECT count(*) FROM t_load r WHERE entity_id = ANY(ARRAY(SELECT id FROM t_load_entity WHERE type = 'accommodation')) AND deleted_at IS NULL;
