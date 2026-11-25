# The cases of runner.c (CASES) and the sources of src/ they run (TARGETS): both are compiled with the coverage instrumentation.
CASES   = cases/card.c cases/cube.c cases/inset.c cases/months.c cases/pack.c cases/plan.c cases/qset.c cases/regex.c cases/stream.c cases/topn.c cases/negative.c
TARGETS = $(SRC)/agg/topn/tourn.c $(SRC)/agg/inset/filter.c $(SRC)/col/f2/cube/cube.c $(SRC)/col/f2/cube/dim.c $(SRC)/col/f2/compact/plan.c $(SRC)/core/quarantine/set.c $(SRC)/agg/cost/card_est.c $(SRC)/agg/stream_exec.c
# the host layer of the SQLite and MariaDB ports: its own host.h, not the one of the extension
HOSTSRC = $(SRC)/../sqlite/host/regex.c $(SRC)/../sqlite/host/regex_parse.c $(SRC)/../sqlite/host/regex_exec.c
