#!/usr/bin/env python3
"""compare.py ARM_A ARM_B: the read classes of two builds, per class, from reads.py results.

An ARM is one or more result files (a comma-separated list, or a shell glob in quotes: 'out/reads-std-tip*.json'); the samples of its runs are pooled.
A is the base, B the build under test. Per class and mode (warm, cold), the ratios B/A of the mean, the mean without spikes, the p90 and the phase-balanced median
(the median of the medians of the six phases of the write-buffer cycle) of the instructions, the spikes of each arm (one-shot samples above 3 times the median of
their class: they come at the same rate on every build and move a raw mean by 5 to 80 percent on their own), and the blocks hit and read.
A class is flagged when
  - the p90 is more than 25 percent above the base (10 percent, the owner's bar, when both arms pooled 3 runs or more: the p90 of one run of 42 samples moves by
    up to 19 percent between two runs of one build on 28 columns), or
  - the phase-balanced median is more than 15 percent above, and
  - the mean without spikes is more than 5 percent above (the owner's bar),
the last two only when both arms pooled 3 runs or more: with fewer they are shown, not flagged (two runs of the same tip on 28 columns differ by 14 percent in the mean and by a
factor of up to 2 in the phase-balanced median, because the state the churn reaches differs; a gate pools three runs a side).
The cold rows (7 samples a run, each in the state of its own batch) flag only when 3 runs or more a side are pooled; with fewer they are shown. The raw mean is shown and never flags. Read every flag against the noise of the SAME build: run one build twice and compare the two runs (`compare.py a1.json a2.json`).
Exit status 1 when a class is flagged.
"""
import glob, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reads

TRIMMED, P90, P90_ONE, PBM, MIN_RUNS = 1.05, 1.10, 1.25, 1.15, 3


def arm(spec):
    paths = [p for x in spec.split(",") for p in (sorted(glob.glob(x)) or [x])]
    return paths, reads.load(paths)


def ratio(a, b):
    return b / a if a else float("nan")


def flags(a, b, runs):
    """the reasons to flag a class: stats of the base, stats of the build under test, the runs pooled by the fewer arm"""
    out = []
    pooled = runs >= MIN_RUNS
    for name, key, bar in (("p90", "p90", P90 if pooled else P90_ONE),) + ((("phase median", "pbm", PBM), ("mean without spikes", "tmean", TRIMMED)) if pooled else ()):
        if ratio(a[key], b[key]) > bar:
            out.append(f"{name} +{100 * (ratio(a[key], b[key]) - 1):.0f}%")
    return out


def rows(sa, sb, classes, runs=0, modes=("warm", "cold")):
    """[(class, mode, stats A, stats B, [flags])] for the classes both arms measured"""
    out = []
    for q in classes:
        for mode in modes:
            a, b = reads.stats(sa, q, mode), reads.stats(sb, q, mode)
            if a and b:
                out.append((q, mode, a, b, flags(a, b, runs) if mode == "warm" or runs >= MIN_RUNS else []))
    return out


def main(a):
    (pa, sa), (pb, sb) = arm(a[0]), arm(a[1])
    runs = min(len(pa), len(pb))
    print(f"A = {len(pa)} run(s), B = {len(pb)} run(s); ratios B/A; flags: p90 > +{((P90 if runs >= MIN_RUNS else P90_ONE) - 1) * 100:.0f}%"
          + (f", phase median > +{(PBM - 1) * 100:.0f}%, mean without spikes > +{(TRIMMED - 1) * 100:.0f}%" if runs >= MIN_RUNS else f" (the phase median and the mean without spikes need {MIN_RUNS} runs a side to flag)"))
    print(f"{'class':8s} {'mode':4s} {'n A|B':>7s} {'mean A':>8s} {'mean B':>8s} {'x':>5s} {'x no spk':>8s} {'p90 x':>6s} {'pbm x':>6s} {'spikes':>7s} {'hit A|B':>11s} {'read A|B':>11s}  flags")
    bad = 0
    for q, mode, x, y, fl in rows(sa, sb, reads.lab.Q, runs):
        bad += bool(fl)
        print(f"{q:8s} {mode:4s} {x['n']:3d}|{y['n']:<3d} {x['mean'] / 1e6:8.2f} {y['mean'] / 1e6:8.2f} {ratio(x['mean'], y['mean']):5.2f} {ratio(x['tmean'], y['tmean']):8.2f} "
              f"{ratio(x['p90'], y['p90']):6.2f} {ratio(x['pbm'], y['pbm']):6.2f} {x['spikes']:3d}|{y['spikes']:<3d} {x['hit']:5.0f}|{y['hit']:<5.0f} {x['read']:5.0f}|{y['read']:<5.0f}  {', '.join(fl)}")
    print(f"{bad} class/mode rows flagged")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main(sys.argv[1:]) if len(sys.argv) == 3 else sys.exit(__doc__)
