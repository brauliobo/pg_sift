#!/usr/bin/env python3
"""Plots of a churn run (bench/churn/churn.sh): one line per arm over the cycles.

    bench/churn/plot.py SCN OUT.png [--db churn]

Panels: immutable segments + write buffer rows, share of units that answer from pre-aggregates, units listed per superblock covered, index file over
live bytes, zone-map tightness (ts), the three query latencies, cumulative bytes of segments the maintenance wrote, cumulative WAL of the maintenance
(VACUUM included) over the WAL of the DML.
"""
import argparse
import csv
import io
import subprocess
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def rows(db, sql):
    out = subprocess.run(["psql", "-X", "-d", db, "--csv", "-c", sql], check=True, capture_output=True, text=True).stdout
    return list(csv.DictReader(io.StringIO(out)))


def series(rs, arm_key, x_key, y):
    d = defaultdict(list)
    for r in rs:
        v = y(r)
        if v is not None:
            d[r[arm_key]].append((int(r[x_key]), v))
    return {a: sorted(p) for a, p in d.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scn")
    ap.add_argument("out")
    ap.add_argument("--db", default="churn")
    a = ap.parse_args()
    st = rows(a.db, f"select * from churn.state where scn = '{a.scn}' and stage = 'maint'")
    qr = rows(a.db, f"select * from churn.qres where scn = '{a.scn}' and stage = 'maint'")
    lg = rows(a.db, f"select * from churn.log where scn = '{a.scn}'")
    f = lambda r, k: float(r[k]) if r[k] != "" else None
    panels = [
        ("segments (immutable)", series(st, "arm", "cycle", lambda r: f(r, "segs")), False),
        ("units answered from pre-aggregates (%)", series(st, "arm", "cycle", lambda r: 100.0 * f(r, "units_exact") / f(r, "units_listed")), False),
        ("units listed per unit covered", series(st, "arm", "cycle", lambda r: f(r, "units_listed") / f(r, "units_covered")), False),
        ("index file / live bytes", series(st, "arm", "cycle", lambda r: f(r, "idx_bytes") / max(f(r, "idx_live_bytes"), 1)), False),
        ("zone-map tightness: units a ts point reads", series(st, "arm", "cycle", lambda r: f(r, "zone_width")), False),
    ]
    for q in ("rollup", "window", "scan"):
        panels.append((f"{q} query (ms)", series([r for r in qr if r["q"] == q], "arm", "cycle", lambda r: f(r, "ms")), True))
    cum = defaultdict(float)
    maint = defaultdict(list)
    for r in sorted(lg, key=lambda r: (r["arm"], int(r["cycle"]))):
        if r["step"] == "dml":
            continue
        cum[r["arm"]] += f(r, "seg_bytes") or 0
        maint[r["arm"]].append((int(r["cycle"]), cum[r["arm"]] / 1e6))
    panels.append(("maintenance: MB of segments written (cumulative)", dict(maint), False))
    dml = defaultdict(float)
    for r in lg:
        if r["step"] == "dml":
            dml[r["arm"]] += f(r, "wal_bytes") or 0
    wal = defaultdict(float)
    wser = defaultdict(list)
    for r in sorted(lg, key=lambda r: (r["arm"], int(r["cycle"]))):
        if r["step"] != "dml":
            wal[r["arm"]] += f(r, "wal_bytes") or 0
            wser[r["arm"]].append((int(r["cycle"]), wal[r["arm"]] / 1e6))
    panels.append(("maintenance + VACUUM WAL (MB, cumulative)", dict(wser), False))
    fig, axs = plt.subplots(3, 3, figsize=(15, 11))
    for ax, (title, s, log) in zip(axs.flat, panels):
        for arm, pts in sorted(s.items()):
            ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", ms=3, label=arm)
        ax.set_title(title, fontsize=10)
        ax.set_xlabel("cycle")
        if log:
            ax.set_yscale("log")
        ax.grid(alpha=0.3)
    axs.flat[0].legend(fontsize=8)
    fig.suptitle(f"churn scenario {a.scn}")
    fig.tight_layout()
    fig.savefig(a.out, dpi=110)


main()
