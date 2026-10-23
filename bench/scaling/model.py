"""Case model and the log-log fit of the focused scaling suite (bench/scaling/scaling.py)."""
import math
from dataclasses import dataclass, field
from typing import Callable

TARGETS = {'flat': 0.30, 'linear': 1.15}      # largest log-log slope that still passes; None = information only
FLOOR_MS = 0.5                                # below this at every size the slope is noise: PASS


@dataclass
class Query:
    name: str
    sql: Callable[[int], str]                 # size -> statement
    target: str | None = 'flat'               # flat | linear | None
    needs_sift: bool = True                   # the sift engine must plan a sift node, else the time is not the index: FAIL


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
