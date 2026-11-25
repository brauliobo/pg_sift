# The cases of check_scaling_c.c (CASES) and the sources of src/ they run (TARGETS): both are compiled with the coverage instrumentation.
CASES   = case_card.c case_cube.c case_inset.c case_months.c case_pack.c case_plan.c case_qset.c case_regex.c case_stream.c case_topn.c case_negative.c
TARGETS = $(SRC)/agg/topn_v6.c $(SRC)/agg/v6/inset/filter.c $(SRC)/col/scope_v6/cube.c $(SRC)/col/f2/compact/plan.c $(SRC)/core/quarantine/set.c $(SRC)/agg/v6/cost/card_est.c $(SRC)/agg/stream_exec.c
# the host layer of the SQLite and MariaDB ports: its own host.h, not the one of the extension
HOSTSRC = $(SRC)/../sqlite/host/regex.c $(SRC)/../sqlite/host/regex_parse.c $(SRC)/../sqlite/host/regex_exec.c
