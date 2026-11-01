"""The IO of the generic runs (steps/generic.py): the host's counters (steps/hostio.py) around each timed call of an engine, and the engine's own counters beside them.

`metered(engine, run, sink)` wraps the runner of a case: each call is measured over the engine's process tree (`engine.pids()`, a container's init or the in-process client), the
device of its data (`engine.datadir()`) and the cgroup, and the engine's cumulative counters (`engine.native_io()`: Elasticsearch `_nodes/stats/fs`, ClickHouse `system.events`) are
differenced; the figures of every call go to `sink`. `medians` gives the figure of a case: the median of the timed calls. Engines without counters of their own (DuckDB, Citus,
TimescaleDB, PostgreSQL containers) have the host's only; on tmpfs or overlay storage the device columns are absent.
"""
import statistics

import hostio


def metered(engine, run, sink):
    datadir = engine.datadir()

    def go():
        meter = hostio.Meter(engine.pids, datadir)
        before = engine.native_io()
        meter.start()
        out = run()
        figures = meter.stop()
        after = engine.native_io()
        figures.update({k: after[k] - before[k] for k in after if k in before})
        sink.append(figures)
        return out
    return go


def medians(sink, runs):
    """{figure: median over the last RUNS calls} where every one of those calls has it"""
    calls = sink[-runs:] if runs else sink[-1:]
    return {k: statistics.median(c[k] for c in calls) for k in calls[0] if all(c.get(k) is not None for c in calls)} if calls else {}
