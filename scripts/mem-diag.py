import importlib.util, os, platform, subprocess, sys, threading, time
spec = importlib.util.spec_from_file_location("ms", os.path.join(os.path.dirname(os.path.abspath(__file__)), "mem-slope.py"))
ms = importlib.util.module_from_spec(spec); spec.loader.exec_module(ms)
only = os.environ.get("ONLY", "^(merge_col|lookup_join_class)$")
rows = int(os.environ.get("ROWS", "50000"))
repeat = int(os.environ.get("REPEAT", "4"))
sizes = ms.sizes_of(rows)
P = lambda *a: print("DIAG", *a, flush=True)

def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True).stdout.strip().replace("\n", " | ")

P("kernel", platform.release(), "loadavg", open("/proc/loadavg").read().strip())
P("platform", platform.machine(), "pagesize", os.sysconf("SC_PAGE_SIZE"), "cpus", os.cpu_count())
P("thp", sh("cat /sys/kernel/mm/transparent_hugepage/enabled"), "glibc", sh("ldd --version | head -1"))
P("cpu", sh("grep -m1 -E 'model name|Model|CPU part' /proc/cpuinfo"), sh("lscpu | grep -E 'Model name|L2|L3' | tr -s ' '"))
P("server", ms.script("select version();").strip(), "libs", sh("ldd $(pg_config --pkglibdir)/pg_sift.so | grep -E 'lz4|zstd|libc'"))
ms.build_all(sizes)

def run_once(n, setup, warm, op):
    ms.prepare(setup + "\n" + (op if warm.strip() == "=" else warm))
    p = ms.psql()
    out, _ = ms.send(p, "select 'PID', pg_backend_pid();", "PIDDONE")
    pid = int(next(l for l in out if "PID" in l).strip().split("|")[1])
    out, err = ms.send(p, ms.SETTINGS + "select sift._test_thp_disable();\nselect 'MS', sift._test_malloc_stats(true);", "WARMDONE")
    ms0 = next((l.strip() for l in out if l.startswith("MS|")), "")
    statm = os.open(f"/proc/{pid}/statm", os.O_RDONLY)
    status = os.open(f"/proc/{pid}/status", os.O_RDONLY)
    def exact_kb():
        txt = os.pread(status, 4096, 0).decode()
        return int(txt[txt.index("RssAnon:") + 8:].split()[0])
    base, ebase = ms.anon_kb_fd(statm), exact_kb()
    peak, epeak, n, gap, stop = [base], [ebase], [0, 0], [0.0, 0.0], threading.Event()
    def sampler(read, pk, i):
        last = time.perf_counter()
        while not stop.is_set():
            pk[0] = max(pk[0], read()); n[i] += 1
            now = time.perf_counter(); gap[i] = max(gap[i], now - last); last = now
    ts = [threading.Thread(target=sampler, args=(lambda: ms.anon_kb_fd(statm), peak, 0)), threading.Thread(target=sampler, args=(exact_kb, epeak, 1))]
    for t in ts: t.start()
    ms.send(p, op, "OPDONE")
    stop.set()
    for t in ts: t.join()
    stats = f"statm-peak {(peak[0] - base) / 1024:.2f} exact-peak {(epeak[0] - ebase) / 1024:.2f} MB, samples {n[0]}/{n[1]}, max gap ms {gap[0] * 1000:.1f}/{gap[1] * 1000:.1f}, base statm {base} exact {ebase} kB, load {open('/proc/loadavg').read().split()[0]}"
    end = ms.anon_kb_fd(statm)
    out, _ = ms.send(p, "select 'MS', sift._test_malloc_stats(false);\nselect 'CTX', sum(total_bytes), sum(used_bytes), count(*) from pg_backend_memory_contexts;\n"
                        "select 'SCOPES', coalesce(sum(peak), 0) from sift._test_mem_highwater();\nselect 'SC', scope, peak from sift._test_mem_highwater() order by scope;", "HWDONE")
    sc = {l.split("|")[1]: int(l.split("|")[2]) for l in out if l.startswith("SC|")}
    ms1 = next((l.strip() for l in out if l.startswith("MS|")), "")
    ctx = next((l.strip() for l in out if l.startswith("CTX|")), "")
    os.close(statm); p.stdin.write("\\q\n"); p.wait()
    return (peak[0] - base) / 1024.0, (end - base) / 1024.0, ms0, ms1, ctx, sc, stats

for name, kind, ceil, status, setup, op, warm, _ in ms.read_cases(only):
    res = {}
    for n in sizes:
        fmt = dict(f=f"f{n}", d=f"d{n}", m=f"m{n}", s=f"s{n}", n=n)
        runs = []
        for _ in range(repeat):
            if ms.uses_copy(setup, warm, op): ms.copy_table(n)
            runs.append(run_once(n, setup.format(**fmt), warm if warm == "=" else warm.format(**fmt), op.format(**fmt)))
        res[n] = runs
        for i, r in enumerate(runs):
            P(name, n, f"run{i}", f"peak {r[0]:.2f} MB, end {r[1]:.2f} MB", r[6], "malloc-before", r[2], "malloc-after", r[3], "ctx", r[4])
        if name == "lookup_join_class":
            P(name, n, "plan", ms.script(ms.SETTINGS + "explain " + op.format(**fmt)).replace("\n", " | "))
    peaks = [min(r[0] for r in res[n]) for n in sizes]
    P(name, "min-peaks", " ".join(f"{p:.2f}" for p in peaks), "slope B/row", f"{(peaks[-1] - peaks[0]) * 1048576 / (sizes[-1] - sizes[0]):.1f}")
    for s in sorted({s for r in res.values() for x in r for s in x[5]}):
        P(name, "scope", s, " ".join(f"{max(x[5].get(s, 0) for x in res[n]) / 1048576:.2f}" for n in sizes))
