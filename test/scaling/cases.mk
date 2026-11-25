# The cases of check_scaling_c.c (CASES) and the sources of src/ they run (TARGETS): both are compiled with the coverage instrumentation.
CASES   = cases/card.c cases/cube.c cases/inset.c cases/months.c cases/pack.c cases/plan.c cases/qset.c cases/regex.c cases/stream.c cases/topn.c cases/negative.c
TARGETS = $(SRC)/agg/topn_v6.c $(SRC)/agg/v6/v6_inset_filter.c $(SRC)/col/scope_v6/cube.c $(SRC)/col/scope_v6/cube_dim.c $(SRC)/col/f2/compact_plan.c $(SRC)/core/quarantine_set.c $(SRC)/agg/plan/card_est.c $(SRC)/agg/stream_exec.c
# the host layer of the SQLite and MariaDB ports: its own host.h, not the one of the extension
HOSTSRC = $(SRC)/../sqlite/host/regex.c $(SRC)/../sqlite/host/regex_parse.c $(SRC)/../sqlite/host/regex_exec.c
