"""Point keys: a request of 51 keys whose rows lie in 51 different units of a table that grows (one row per heap page, a key for each row, the rows of a key scattered):
the point directory lists the rows of the keys, so the units that hold none are not read and the others go on as that row alone. The time must not grow with the units
(a plain IN list of the keys, and the keys of a lookup join to a table of 51 rows). The index of each size is partial on id <= size * rows of a unit, as in `units`.
SCALING_UNIT_PAGES is the pages of a unit of the build under test (256, the default; 8 in the gate build, SIFT_SB_SHIFT=3)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query

PAGES = int(os.environ.get('SCALING_UNIT_PAGES', 256))
ROWS = PAGES
MUL, MOD = 40503, 1000003                    # the key of row i: a permutation of the rows below MOD, so the rows of 51 keys are 51 rows far apart
IDS = [37 + 160 * j for j in range(51)]      # inside the first 1,020 units of the smallest size, whatever the geometry
KEYS = ', '.join(str(i * MUL % MOD) for i in IDS)


def setup(size):
    return f"""create table t (id int, k int, v int, pad text);
alter table t alter column pad set storage plain;
insert into t select i, i::bigint * {MUL} % {MOD}, i % 1000, repeat('x', 7000) from generate_series(1, {size * ROWS}) i;
create table d as select k from t where id in ({', '.join(map(str, IDS))});
vacuum freeze t; analyze d;"""


def at(size):
    return f"drop index if exists ix; create index ix on t using sift_col (id, k, v) with (point_tid_columns = 'k') where id <= {size * ROWS};"


def q(name, body):
    return Query(name, lambda size: f"select {body.format(bound=size * ROWS)}", 'flat')


SIZES = [512, 1024, 2048, 4096] if PAGES > 16 else [1024, 1536, 2048, 4096, 8192]
QUERIES = [
    q('in51', f"count(*), sum(v) from t where k in ({KEYS}) and id <= {{bound}}"),
    q('join51', "count(*), sum(t.v) from t join d on d.k = t.k where t.id <= {bound}"),
]
CHECK = lambda size: [f"select count(*) = 51 and sum(v) = (select sum(i % 1000) from unnest(array[{', '.join(map(str, IDS))}]) i) from t where k in ({KEYS}) and id <= {size * ROWS}"]
CASES = [Case('pointkeys', SIZES, setup=setup, at=at, native=False, unit='units', check=CHECK, queries=QUERIES)]
GATE = CASES if PAGES == 8 else []
