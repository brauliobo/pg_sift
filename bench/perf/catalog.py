"""Fixed, versioned logical workloads. Smoke membership is independent of selection.

A case with gate=False is measured and reported but does not decide the verdict: its operation takes under a millisecond, and on hosted runners it
was 37 of the 52 non-pass verdicts of 439 runs (storage_insert 25, storage_point_lookup 12) without one confirmed regression."""
from dataclasses import dataclass, replace


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
    gate: bool = True
    lifecycle: str = 'read'
    workload_version: str = 'v1'
    mode: str = '1T'
    modes: tuple = ('1T',)
    needs: str = ''

    def at(self, mode):
        # a case runs once per mode: the 1T run keeps the name of the case, the others are named case@MODE
        return replace(self, name=self.name if mode == '1T' else f'{self.name}@{mode}', mode=mode, modes=(mode,))

    def statements(self, seed_max, operations):
        if self.lifecycle != 'insert_trajectory':
            raise ValueError('point rendering requires insert_trajectory')
        return [self.sql.format(id=seed_max+i) for i in range(1,operations+1)]

    def expected_rows(self, seed_max, operations):
        return ("SELECT id,g,amount,body,pad FROM seed UNION ALL "
                f"SELECT {seed_max}+i,1,7,'insert common',repeat('p',1500) FROM generate_series(1,{operations}) i")


ANALYTICAL = ('1T', '4T')
RESET = 'TRUNCATE mutable; INSERT INTO mutable SELECT * FROM seed ORDER BY id; VACUUM (FREEZE, ANALYZE) mutable'
STATE = 'SELECT count(*),sum(id),sum(amount) FROM mutable'
CASES = [
    Case('bm25_top10_common', 'bm25', "SELECT id FROM docs WHERE body @@> 'common' ORDER BY sift.score(ctid) DESC,id LIMIT 10", plan='docs_bm25', smoke=True),
    Case('bm25_top10_rare', 'bm25', "SELECT id FROM docs WHERE body @@> 'rare' ORDER BY sift.score(ctid) DESC,id LIMIT 10", plan='docs_bm25'),
    Case('bm25_count_common', 'bm25', "SELECT count(*) FROM docs WHERE body @@> 'common'", "SELECT count(*) FROM seed WHERE body LIKE '%common%'", 'docs_bm25'),
    Case('bm25_count_rare', 'bm25', "SELECT count(*) FROM docs WHERE body @@> 'rare'", "SELECT count(*) FROM seed WHERE body LIKE '%rare%'", 'docs_bm25'),
    Case('bm25_filtered_top10', 'bm25', "SELECT id FROM docs WHERE body @@> 'common' AND g=3 ORDER BY sift.score(ctid) DESC,id LIMIT 10", plan='docs_bm25'),
    Case('bm25_filtered_count', 'bm25', "SELECT count(*) FROM docs WHERE body @@> 'rare' AND g=3", "SELECT count(*) FROM seed WHERE body LIKE '%rare%' AND g=3", 'docs_bm25'),
    Case('agg_scan', 'agg', 'SELECT sum(amount),min(id),max(id) FROM docs WHERE id BETWEEN 100 AND 7000', 'SELECT sum(amount),min(id),max(id) FROM seed WHERE id BETWEEN 100 AND 7000', 'SiftAgg', True, modes=ANALYTICAL),
    Case('agg_group', 'agg', 'SELECT g,count(*),sum(amount) FROM docs GROUP BY g ORDER BY g', 'SELECT g,count(*),sum(amount) FROM seed GROUP BY g ORDER BY g', 'SiftAgg', modes=ANALYTICAL),
    Case('agg_distinct', 'agg', 'SELECT count(DISTINCT g) FROM docs', 'SELECT count(DISTINCT g) FROM seed', 'SiftAgg', modes=ANALYTICAL),
    Case('agg_filter', 'agg', 'SELECT g,count(*),sum(amount) FROM docs WHERE amount>200 GROUP BY g ORDER BY g', 'SELECT g,count(*),sum(amount) FROM seed WHERE amount>200 GROUP BY g ORDER BY g', 'SiftAgg', modes=ANALYTICAL),
    Case('agg_table_group', 'agg', 'SELECT g,count(*),sum(amount) FROM store GROUP BY g ORDER BY g', 'SELECT g,count(*),sum(amount) FROM seed GROUP BY g ORDER BY g', 'SiftAgg', modes=ANALYTICAL, needs='table'),
    Case('agg_table_scan', 'agg', 'SELECT sum(amount) FROM store WHERE id BETWEEN 100 AND 7000', 'SELECT sum(amount) FROM seed WHERE id BETWEEN 100 AND 7000', 'SiftAgg', modes=ANALYTICAL, needs='table'),
    Case('storage_point_lookup', 'storage', 'SELECT id,g,amount,body FROM store WHERE id=4097', 'SELECT id,g,amount,body FROM seed WHERE id=4097', 'store_pk', True, gate=False, needs='table'),
    Case('storage_insert', 'storage', "INSERT INTO mutable VALUES ({id},1,7,'insert common',repeat('p',1500))", reset=RESET, check=STATE, lifecycle='insert_trajectory', workload_version='committed_append_settled_composite_v4', gate=False, needs='table'),
    Case('storage_update', 'storage', 'UPDATE mutable SET amount=amount+1 WHERE id BETWEEN 100 AND 200', reset=RESET, check=STATE, lifecycle='reset_each_operation', needs='table'),
    Case('storage_vacuum_space', 'storage', 'VACUUM (DISABLE_PAGE_SKIPPING) mutable', reset=RESET+'; DELETE FROM mutable WHERE id>2048', check=STATE, lifecycle='reset_each_operation', needs='table'),
    Case('bm25_phrase_nightly', 'bm25', "SELECT count(*) FROM docs WHERE body @@> '\"common wire\"'", "SELECT count(*) FROM seed WHERE body LIKE '%common wire%'", 'docs_bm25', nightly=True),
    Case('agg_high_cardinality_nightly', 'agg', 'SELECT id,count(*),sum(amount) FROM docs GROUP BY id ORDER BY id', 'SELECT id,count(*),sum(amount) FROM seed GROUP BY id ORDER BY id', 'SiftAgg', nightly=True, modes=ANALYTICAL),
]


def select(groups, tier):
    unknown = set(groups) - {'smoke', 'bm25', 'agg', 'storage'}
    if unknown:
        raise ValueError(f'unknown groups: {sorted(unknown)}')
    return [c for c in CASES if tier == 'nightly' or (not c.nightly and (c.smoke or c.group in groups))]


def expand(cases):
    return [c.at(mode) for c in cases for mode in c.modes]
