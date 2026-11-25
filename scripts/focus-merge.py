#!/usr/bin/env python3
# The focus tables of scripts/focus/ (made by scripts/focus-gen.sh): one file NAME.tsv (NAME may hold folders: agg/v6/star.tsv) for each regression test and _hot.tsv, each a single line
# NAME<TAB>FILE:FUNCTION... with the tokens sorted and unique. A row depends only on its test's measured functions and on the hot set, so a file
# is written only when its bytes change and the tests that were not measured keep their files.
#
#   scripts/focus-merge.py WORK TEST...   merge WORK/load.json and WORK/cov/TEST.json into scripts/focus (env OLD, ALL, HOT_PCT)
#   scripts/focus-merge.py --selftest
import json, os, sys, tempfile

HOT = "=hot"


def render(name, tokens):
    return name + "\t" + " ".join(sorted(tokens)) + "\n"


def table_files(directory):
    return sorted(os.path.relpath(os.path.join(root, f), directory) for root, _, fs in os.walk(directory) for f in fs)


def read_table(directory):
    rows = {}
    for fname in table_files(directory):
        name, _, tokens = open(os.path.join(directory, fname)).read().rstrip("\n").partition("\t")
        rows[HOT if fname == "_hot.tsv" else name] = set(tokens.split())
    return rows


def prune_empty_dirs(directory):
    for root, _, _ in os.walk(directory, topdown=False):
        if root != directory and not os.listdir(root):
            os.rmdir(root)


def measured(work, test, load):
    return {f + ":" + g for f, gs in json.load(open("%s/cov/%s.json" % (work, test))).items() for g in gs if (f, g) not in load}


def merge(directory, work, tests, everything, hot_pct):
    load = {(f, g) for f, gs in json.load(open(work + "/load.json")).items() for g in gs}
    rows = read_table(directory) if os.path.isdir(directory) else {}
    new = {t: measured(work, t, load) for t in tests}
    if everything:
        count = {}
        for tokens in new.values():
            for x in tokens:
                count[x] = count.get(x, 0) + 1
        rows = {HOT: {x for x, c in count.items() if c > len(new) * hot_pct / 100}}
    for t, tokens in new.items():
        rows[t] = tokens - rows.get(HOT, set())
    os.makedirs(directory, exist_ok=True)
    files = {("_hot" if name == HOT else name) + ".tsv": render(name, tokens) for name, tokens in rows.items()}
    for fname in table_files(directory):
        if fname not in files:
            os.remove(os.path.join(directory, fname))
    prune_empty_dirs(directory)
    for fname, text in files.items():
        path = os.path.join(directory, fname)
        if not os.path.exists(path) or open(path).read() != text:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w").write(text)


def selftest():
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../bench/steps"))
    import tmpfs  # noqa: F401  (tempfile on tmpfs)

    def snapshot(directory):
        return {f: (open(os.path.join(directory, f)).read(), os.stat(os.path.join(directory, f)).st_mtime_ns) for f in table_files(directory)}

    def measure(work, covs):
        os.makedirs(work + "/cov", exist_ok=True)
        json.dump({"src/a.c": ["loaded"]}, open(work + "/load.json", "w"))
        for t, fns in covs.items():
            os.makedirs(os.path.dirname("%s/cov/%s.json" % (work, t)), exist_ok=True)
            json.dump(fns, open("%s/cov/%s.json" % (work, t), "w"))

    with tempfile.TemporaryDirectory() as d:
        out, work = d + "/focus", d + "/work"
        covs = {"t1": {"src/a.c": ["loaded", "f", "common"], "src/b.c": ["g"]}, "t2": {"src/a.c": ["common", "h"]}, "t3": {"src/a.c": ["common"]}}
        measure(work, covs)
        merge(out, work, ["t1", "t2", "t3"], True, 50)
        want = {"_hot.tsv": "=hot\tsrc/a.c:common\n", "t1.tsv": "t1\tsrc/a.c:f src/b.c:g\n", "t2.tsv": "t2\tsrc/a.c:h\n", "t3.tsv": "t3\t\n"}
        got = {f: text for f, (text, _) in snapshot(out).items()}
        assert got == want, got
        for names in (["t1"], ["t2", "t1"], ["t1", "t2", "t3"]):
            before = snapshot(out)
            merge(out, work, names, False, 50)
            assert snapshot(out) == before, "measuring unchanged tests rewrote a file: " + " ".join(names)
        covs["t2"] = {"src/a.c": ["common", "h2"]}
        measure(work, covs)
        before = snapshot(out)
        merge(out, work, ["t3", "t2"], False, 50)
        after = snapshot(out)
        assert [f for f in after if after[f] != before[f]] == ["t2.tsv"] and after["t2.tsv"][0] == "t2\tsrc/a.c:h2\n"
        merge(out, work, ["t2", "t3"], False, 50)
        assert snapshot(out) == after
        os.remove(work + "/cov/t3.json")
        merge(out, work, ["t1", "t2"], True, 50)
        assert table_files(out) == ["_hot.tsv", "t1.tsv", "t2.tsv"], "a test without a measurement keeps its row"
        covs = {"a/b/t4": {"src/a.c": ["common", "k"]}, "t1": covs["t1"]}
        measure(work, covs)
        merge(out, work, ["a/b/t4", "t1"], False, 50)
        assert table_files(out) == ["_hot.tsv", "a/b/t4.tsv", "t1.tsv", "t2.tsv"], "a test in a folder has its table in the folder"
        assert read_table(out)["a/b/t4"] == {"src/a.c:k"}
        merge(out, work, ["t1"], True, 50)
        assert table_files(out) == ["_hot.tsv", "t1.tsv"], "a table that is not measured again goes, and its empty folders with it"
        assert not os.path.exists(out + "/a")
    print("focus-merge: selftest ok: an unchanged test keeps its file, a measured test changes only its own, the order of names does not matter")


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        selftest()
    else:
        merge(os.environ["OLD"], sys.argv[1], sys.argv[2:], os.environ["ALL"] == "1", int(os.environ["HOT_PCT"]))
