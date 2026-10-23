"""Properties: 100 rows per key; a request of 51 keys must stay flat while the keys grow 10k -> 31.6k -> 100k (the table must hold more than 51 units, else the request reads them all; rows carry 150 bytes of padding for that: 10k keys are 83 units). Two layouts of the same rows:
clustered (key, date order, asserted) and arrival (date order, keys interleaved: reported, the cost of the layout and not of sift)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query

PER = 100


def setup(size):
    base = f"select (i / {PER}) as p, date '2024-01-01' + ((i % {PER}) * 800 / {PER}) as d, (i % 7) as s, i % 100 as v, i, repeat('x', 150) as pad from generate_series(0, {size * PER - 1}) i"
    return f"""create table base as {base};
create table clu as select p, d, s, v, pad from base order by p, d;
create table arr as select p, d, s, v, pad from base order by d, hashint4(i);
drop table base;
vacuum freeze clu; vacuum freeze arr;
create index clu_ix on clu using sift_col (p, d, s, v);
create index arr_ix on arr using sift_col (p, d, s, v);
analyze clu; analyze arr;"""


def req(n):
    return ', '.join(str(int((i + 0.5) * n / 51)) for i in range(51))


def qs(tab, target):
    return [
        Query(f'{tab}_group', lambda n: f"select s, count(*), sum(v) from {tab} where p in ({req(n)}) group by s", target),
        Query(f'{tab}_window', lambda n: f"select s, count(*), sum(v) from {tab} where p in ({req(n)}) and d >= date '2025-09-01' group by s", target),
        Query(f'{tab}_count', lambda n: f"select count(*) from {tab} where p in ({req(n)})", target),
    ]


CASES = [Case('properties', [10000, 31600, 100000], setup=setup, unit='properties', quick=2, per_size=True,
              queries=qs('clu', 'flat') + qs('arr', None))]
