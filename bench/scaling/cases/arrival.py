"""Arrival order: rows in the order they arrived, keys interleaved. Key k appears at k/size of the timeline and its ROWS rows arrive within the next LIFE of it, so a unit holds rows
of the keys created in the last LIFE of the timeline. A request of 51 keys reads, by the index that decides the pruning of the key:
  zone   min/max zones only: a unit is read when its key range holds a requested key, 51 x LIFE of the units however many keys there are (linear in the keys);
  sets   key_columns = 'p' on units of at most 32 keys (an exact summary): a unit is read when it holds a row of a requested key, at most 51 x ROWS units (flat once the table is larger);
  bloom  the same on units of more keys (a bloom filter, 10 bits a key): the 1% false positives of the 51 probes keep 1 - 0.99^51 = 40% of the units (linear again, at 40% of the units).
The size is the keys; the units read (the plan's Units) are the quantity asserted, the time is information (every unit still pays for the test of its summary)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query

ROWS, LIFE = 16, 0.04
PADS = {'zone': 2000, 'sets': 2000, 'bloom': 1000}          # bytes of padding: 3 or 7 rows a heap page, so 24 or 56 rows (keys) in a unit of 8 pages
INDEX = {'zone': "key_columns = 'off'", 'sets': "key_columns = 'p'", 'bloom': "key_columns = 'p'"}


def setup(size):
    sql = f"""create table base as
select p, i, hashint4(i) as r1, hashint4(i + 1) as r2, hashint4(i + 2) as r3,
       p::float8 / {size} + {LIFE} * (hashint4(i * 7 + 3)::bigint + 2147483648) / 4294967296.0 as t
from (select i, i / {ROWS} as p from generate_series(0, {size * ROWS - 1}) i) x;"""
    for t, pad in PADS.items():
        sql += f"""
create table {t} (p int, d date, s int, v int, r1 int, r2 int, r3 int, pad text);
alter table {t} alter column pad set storage plain;
insert into {t} select p, date '2024-01-01' + (t * 800)::int, i % 7, i % 100, r1, r2, r3, repeat('x', {pad}) from base order by t;
vacuum freeze {t};
create index {t}_ix on {t} using sift_col (p, d, s, v, r1, r2, r3) with ({INDEX[t]});
analyze {t};"""
    return sql + "\ndrop table base;"


def req(n):
    return ', '.join(str(int((i + 0.5) * n / 51)) for i in range(51))


def queries():
    return [Query(f'{t}_sum', (lambda t: lambda n: f"select sum(r1) from {t} where p in ({req(n)})")(t), None, units='flat' if t == 'sets' else 'info') for t in PADS]


SIZES = [1562, 3125, 6250, 12500, 25000]
CASES = [Case('arrival', SIZES, setup=setup, unit='keys', quick=2, per_size=True, native=False, queries=queries())]
GATE = CASES
