"""Fixed, versioned logical workloads. Smoke membership is independent of selection."""
from dataclasses import dataclass


@dataclass(frozen=True)
class Case:
    name: str
    group: str
    sql: str
    oracle: str = ''
    plan: str = ''
    smoke: bool = False
    reset: str = ''
    check: str = ''
    settings: str = ''
    nightly: bool = False
    lifecycle: str = 'read'
    workload_version: str = 'v1'

    def statements(self, seed_max, operations):
        if self.lifecycle != 'insert_trajectory':
            raise ValueError('point rendering requires insert_trajectory')
        return [self.sql.format(id=seed_max+i) for i in range(1,operations+1)]

    def expected_rows(self, seed_max, operations):
        return ("SELECT id,g,amount,body,pad FROM seed UNION ALL "
                f"SELECT {seed_max}+i,1,7,'insert common',repeat('p',1500) FROM generate_series(1,{operations}) i")


RESET = 'TRUNCATE mutable; INSERT INTO mutable SELECT * FROM seed ORDER BY id; VACUUM (FREEZE, ANALYZE) mutable'
STATE = 'SELECT count(*),sum(id),sum(amount) FROM mutable'
CASES = [
    Case('bm25_top10_common', 'bm25', "SELECT id FROM docs WHERE body @@> 'common' ORDER BY sift.score(ctid) DESC,id LIMIT 10", plan='docs_bm25', smoke=True),
    Case('bm25_top10_rare', 'bm25', "SELECT id FROM docs WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC,id LIMIT 10", plan='docs_bm25'),
    Case('bm25_count_common', 'bm25', "SELECT count(*) FROM docs WHERE body @@> 'common'", "SELECT count(*) FROM seed WHERE body LIKE '%common%'", 'docs_bm25'),
    Case('bm25_count_rare', 'bm25', "SELECT count(*) FROM docs WHERE body @@> 'rare'", "SELECT count(*) FROM seed WHERE body LIKE '%rare%'", 'docs_bm25'),
    Case('bm25_filtered_top10', 'bm25', "SELECT id FROM docs WHERE body @@> 'common' AND g=3 ORDER BY sift.score(ctid) DESC,id LIMIT 10", plan='docs_bm25'),
    Case('bm25_filtered_count', 'bm25', "SELECT count(*) FROM docs WHERE body @@> 'rare' AND g=3", "SELECT count(*) FROM seed WHERE body LIKE '%rare%' AND g=3", 'docs_bm25'),
    Case('agg_scan', 'agg', 'SELECT sum(amount),min(id),max(id) FROM docs WHERE id BETWEEN 100 AND 7000', 'SELECT sum(amount),min(id),max(id) FROM seed WHERE id BETWEEN 100 AND 7000', 'SiftAgg', True, settings='SET sift.agg_preagg=off'),
    Case('agg_group', 'agg', 'SELECT g,count(*),sum(amount) FROM docs GROUP BY g ORDER BY g', 'SELECT g,count(*),sum(amount) FROM seed GROUP BY g ORDER BY g', 'SiftAgg'),
    Case('agg_distinct', 'agg', 'SELECT count(DISTINCT g) FROM docs', 'SELECT count(DISTINCT g) FROM seed', 'SiftAgg'),
    Case('agg_filter', 'agg', 'SELECT g,count(*),sum(amount) FROM docs WHERE amount>200 GROUP BY g ORDER BY g', 'SELECT g,count(*),sum(amount) FROM seed WHERE amount>200 GROUP BY g ORDER BY g', 'SiftAgg'),
    Case('agg_table_group', 'agg', 'SELECT g,count(*),sum(amount) FROM store GROUP BY g ORDER BY g', 'SELECT g,count(*),sum(amount) FROM seed GROUP BY g ORDER BY g', 'SiftAgg'),
    Case('agg_table_scan', 'agg', 'SELECT sum(amount) FROM store WHERE id BETWEEN 100 AND 7000', 'SELECT sum(amount) FROM seed WHERE id BETWEEN 100 AND 7000', 'SiftAgg', settings='SET sift.agg_preagg=off'),
    Case('storage_point_lookup', 'storage', 'SELECT id,g,amount,body FROM store WHERE id=4097', 'SELECT id,g,amount,body FROM seed WHERE id=4097', 'store_pk', True),
    Case('storage_insert', 'storage', "INSERT INTO mutable VALUES ({id},1,7,'insert common',repeat('p',1500))", reset=RESET, check=STATE, lifecycle='insert_trajectory', workload_version='committed_append_settled_pair_v3'),
    Case('storage_update', 'storage', 'UPDATE mutable SET amount=amount+1 WHERE id BETWEEN 100 AND 200', reset=RESET, check=STATE, lifecycle='reset_each_operation'),
    Case('storage_vacuum_space', 'storage', 'VACUUM (DISABLE_PAGE_SKIPPING) mutable', reset=RESET+'; DELETE FROM mutable WHERE id>2048', check=STATE, lifecycle='reset_each_operation'),
    Case('bm25_phrase_nightly', 'bm25', "SELECT count(*) FROM docs WHERE body @@> '\"common wire\"'", "SELECT count(*) FROM seed WHERE body LIKE '%common wire%'", 'docs_bm25', nightly=True),
    Case('agg_high_cardinality_nightly', 'agg', 'SELECT id,count(*),sum(amount) FROM docs GROUP BY id ORDER BY id', 'SELECT id,count(*),sum(amount) FROM seed GROUP BY id ORDER BY id', 'SiftAgg', nightly=True),
]


def select(groups, tier):
    unknown = set(groups) - {'smoke', 'bm25', 'agg', 'storage'}
    if unknown:
        raise ValueError(f'unknown groups: {sorted(unknown)}')
    return [c for c in CASES if tier == 'nightly' or (not c.nightly and (c.smoke or c.group in groups))]
