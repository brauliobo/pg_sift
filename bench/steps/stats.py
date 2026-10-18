"""The statistics of repeated measurements, one implementation for steps/cell.py (the cell's error), steps/tables.py (the rounds of a cell)
and steps/analytics.py (the runs of a query). `python3 -I` puts no directory on sys.path: the importers insert this one (steps/readme.py does the same).
"""
import math
import statistics


def number(x):
    """float(x), None when x is not a number"""
    try:
        return float(x)
    except ValueError:
        return None


def mean(v):
    return statistics.fmean(v)


def stdev(v):
    """sample standard deviation (two passes: cheap on the hundred thousand latencies of a cell)"""
    m = mean(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1))


def sem(v):
    """standard error of the mean, sd / sqrt(n); None for a single value"""
    return stdev(v) / math.sqrt(len(v)) if len(v) > 1 else None


def summary(xs):
    """the row fields of a repeated measurement (None values dropped): median, min, max, stderr (sem), n and the values; None when empty"""
    xs = [x for x in xs if x is not None]
    if not xs:
        return None
    return dict(value=statistics.median(xs), min=min(xs), max=max(xs), stderr=sem(xs), n=len(xs), values=xs)
