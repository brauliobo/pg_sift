"""Key partitions: the rows of keys 0..size-1, PER rows a key, range-partitioned by key (KEYS keys a partition), one sift_col index a partition, clustered by key; `one` is the
same rows in a single table. A request of 51 keys spaced over the key range touches 51 partitions at every size from 5,100 keys (the partitions it touches, not the ones that
exist), so its time must stay flat while the keys grow to 100k (the partitions to 1,000). The single table is information: its zone pass walks every unit."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query

PER, KEYS = 20, 100


def setup(size):
    rows = f"select (i / {PER}) as p, date '2024-01-01' + ((i % {PER}) * 800 / {PER}) as d, (i % 7) as s, i % 100 as v, repeat('x', 150) as pad from generate_series(0, {size * PER - 1}) i"
    return f"""create table par (p int, d date, s int, v int, pad text) partition by range (p);
select format('create table par_%s partition of par for values from (%s) to (%s)', i, i * {KEYS}, (i + 1) * {KEYS}) from generate_series(0, {(size + KEYS - 1) // KEYS - 1}) i \\gexec
insert into par {rows};
create table one as select * from par;
vacuum freeze par; vacuum freeze one;
create index par_ix on par using sift_col (p, d, s, v);
create index one_ix on one using sift_col (p, d, s, v);
analyze par; analyze one;"""


def req(n):
    return ', '.join(str(int((i + 0.5) * n / 51)) for i in range(51))


def qs(tab, target):
    return [
        Query(f'{tab}_group', lambda n: f"select s, count(*), sum(v) from {tab} where p in ({req(n)}) group by s", target),
        Query(f'{tab}_window', lambda n: f"select s, count(*), sum(v) from {tab} where p in ({req(n)}) and d >= date '2025-09-01' group by s", target),
        Query(f'{tab}_count', lambda n: f"select count(*) from {tab} where p in ({req(n)})", target),
    ]


SIZES = [6250, 12500, 25000, 50000, 100000]
CASES = [Case('keyparts', SIZES, setup=setup, unit='keys', quick=2, per_size=True, queries=qs('par', 'flat') + qs('one', None))]
GATE = CASES
