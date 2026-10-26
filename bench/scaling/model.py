"""Case model and the log-log fit of the focused scaling suite (bench/scaling/scaling.py)."""
import math
from dataclasses import dataclass, field
from typing import Callable

TARGETS = {'flat': 0.50, 'linear': 1.15, 'plan': 0.15}      # largest log-log slope that still passes (margin over the measured 0.1-0.4 and 0.8-1.05); None = information only. plan: the buffers of a first plan, a
# floor of catalogs and the census with a small part that grows with the units: 0.08 with the leaf statistics block, 0.26 when the plan reads the zones of every unit
FLOOR_MS = 0.5                                # below this at every size the slope is noise: PASS


@dataclass
class Query:
    name: str
    sql: Callable[[int], str]                 # size -> statement
    target: str | None = 'flat'               # flat | linear | None
    needs_sift: bool = True                   # the sift engine must plan a sift node, else the time is not the index: FAIL
    units: str | None = None                  # flat | linear | info: the units read (the plan's Units) against the size, a second SLOPE line `<query>[units]`; None = not reported
    first_plan: str | None = None             # plan | info: the shared buffers the first plan of a fresh backend touches against the size (`<query>[first-plan buffers]`); None = not reported


@dataclass
class Case:
    name: str                                 # also the schema scal_<name>
    sizes: list[int]
    queries: list[Query]
    setup: Callable[[int], str]               # largest size -> SQL that creates the schema, data and the sift_col index
    at: Callable[[int], str] = lambda size: ''  # SQL run before measuring one size (e.g. the partial index of that size)
    quick: int = 2                            # --quick keeps the first this many sizes
    per_size: bool = False                    # setup(size) is called for each size (the schema is rebuilt), not once for the largest
    native: bool = True                       # also measure the reference engine (sift off, no sift index scan)
    unit: str = 'size'
    check: Callable[[int], list[str]] | None = None   # size -> queries (at the first size) whose result must be the same under the sift and the native engine; a boolean must be true


def fit(points):
    """points [(size, ms)] -> (slope of the least-squares log-log fit, slope of the last segment)"""
    xs = [math.log(s) for s, _ in points]
    ys = [math.log(max(t, 0.01)) for _, t in points]
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0
    last = (ys[-1] - ys[-2]) / (xs[-1] - xs[-2]) if n > 1 and xs[-1] != xs[-2] else 0.0
    return slope, last


def verdict(points, target):
    """-> (slope, last, 'PASS'|'FAIL'|'INFO')"""
    slope, last = fit(points)
    if target is None:
        return slope, last, 'INFO'
    if max(t for _, t in points) < FLOOR_MS:
        return slope, last, 'PASS'
    return slope, last, 'PASS' if slope <= TARGETS[target] else 'FAIL'
