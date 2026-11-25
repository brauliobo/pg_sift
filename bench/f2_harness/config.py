import os
import platform


DEFAULT_SCALES = [10000, 40000, 160000]
RELEASE_SCALES = [10000, 100000, 1000000]
SCALES = list(DEFAULT_SCALES)
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAINT_MEM = "4MB"
WORK_MEM = "4MB"
PAR_WORK_MEM = "16MB"
BACKEND_CACHE_CAP = "2MB"
POOL_EXTRA_INDEXES = 32
UUID_BYTES = 16
VACUUM_RSS_GUARD_MB = 6144.0  # 6 GB safety cap against v5.1.0 VACUUM OOM spikes
VACUUM_TIMEOUT_SEC = 90.0

INDEX_COLS = (
    "id, entity_id, event_date, deleted_at, updated_at, group_key, distinct_tag, status, "
    "amount, score, qty, event_uuid, metrics, notes, analysis"
)
INDEX_PATHS = (
    "metrics.channel text, metrics.latency float8, "
    "notes.tag text, notes.priority int4, "
    "analysis.flag text, "
    "analysis.items[]{kind text, weight float8}"
)
INDEX_KEY_ORDER = "entity_id, event_date"
INDEX_SCOPE = "parent_id: entity_id -> entity_parents(entity_id, parent_id)"
MEGA_ENTITY_ID = 1
MEDIAN_ENTITY_ID = 1500
MEGA_PARENT_ID = 1
MID_PARENT_ID = 42


def klabel(n):
    """10000 -> 10k, 1000000 -> 1M"""
    return f"{n // 1000000}M" if n >= 1000000 and n % 1000000 == 0 else f"{n // 1000}k"


def vacuum_timeout():
    """the VACUUM guard grows with the ladder: 90 s up to 160k rows"""
    return VACUUM_TIMEOUT_SEC * max(1.0, SCALES[-1] / DEFAULT_SCALES[-1])


def cpu_model():
    try:
        with open("/proc/cpuinfo", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def read_proc_mem_kb(pid):
    """Return (vm_hwm_kb, rss_anon_kb, vm_rss_kb) from /proc/<pid>/status."""
    hwm = anon = rss = 0.0
    try:
        with open(f"/proc/{pid}/status", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    hwm = float(line.split()[1])
                elif line.startswith("RssAnon:"):
                    anon = float(line.split()[1])
                elif line.startswith("VmRSS:"):
                    rss = float(line.split()[1])
    except OSError:
        pass
    return hwm, anon, rss


def ols_slope(xs, ys):
    """Compute ordinary least-squares slope dy/dx."""
    n = len(xs)
    if n < 2:
        return 0.0
    mx = sum(xs) / n
    my = sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    return num / den if den > 0 else 0.0
