import time
from .client import PgClient
from .config import MAINT_MEM, SCALES, WORK_MEM
from .schema import setup_base_schema


class Run:
    """what one harness run shares: the client, the arguments and every measurement the report reads"""

    def __init__(self, args):
        self.args = args
        self.pg = PgClient(args.host, args.port, args.db, args.user)
        self.t_suite_start = time.monotonic()
        self.tsv_rows = []

    def rec(self, section, metric, scale, value, unit, notes=""):
        self.tsv_rows.append((section, metric, str(scale), f"{value}", unit, notes))


def record_meta(st):
    args, pg, rec = st.args, st.pg, st.rec
    st.ext_ver = pg.query_scalar("CREATE EXTENSION IF NOT EXISTS pg_sift; SELECT extversion FROM pg_extension WHERE extname = 'pg_sift';")
    st.pg_ver = pg.query_scalar("SHOW server_version;")
    st.scope_info = setup_base_schema(pg)
    st.mult = st.scope_info["mult"]

    rec("meta", "round", "-", args.round, "str", f"branch={args.git_branch} sha={args.git_sha}")
    rec("meta", "pg_sift_version", "-", st.ext_ver, "str", f"pg={st.pg_ver}")
    rec("meta", "build_time", "-", f"{args.build_sec:.2f}", "s", f"make -j{args.make_jobs}")
    rec(
        "meta",
        "scope_multiplicity",
        "-",
        f"{st.mult:.3f}",
        "x",
        f"2000 entities -> 100 parents; Zipf p1={st.scope_info['p1_entities']} "
        f"top10={st.scope_info['top10_pct']:.1f}% 1-parent={st.scope_info['single_parent_pct']:.1f}% "
        f"max={st.scope_info['max_mult']} overlap={st.scope_info['overlap_parents_pct']:.1f}%",
    )
    rec("meta", "budgets", "-", f"maint={MAINT_MEM},work={WORK_MEM}", "str", "tight scaling budgets")
    rec("meta", "scales", "-", ",".join(str(n) for n in SCALES), "str", "ladder of the slopes")
