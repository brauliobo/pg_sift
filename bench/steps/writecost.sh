#!/usr/bin/env bash
export LC_ALL=C
# What a write costs on the primary and on a standby (README "What a write costs on the primary and the standby"): a primary with a hot standby on this
# host, 1,000,000 rows, three tables (`pk`: primary key; `bt`: plus btrees on (prop, rdate) and (rdate); `sd`: primary key plus one sift_col index over five columns).
# Cells: backend instructions, WAL bytes, standby replay instructions per row (perf stat, user mode, attached to the backend and to the standby's startup
# process), 2,000 single-row INSERTs, 4,000 rows in statements of 100, COPY of 50,000 rows, 2,000 UPDATEs of a column no index covers / only the sift_col
# covers / every index covers, 2,000 DELETEs, VACUUM; then 20 s of pgbench inserters (1, 4 and 16 clients): WAL and standby replay per row (the backend instructions of the inserters are n/a: perf does not follow the backends a pgbench run forks).
#   bench/steps/writecost.sh OUTDIR [ROWS]    writes OUTDIR/write-single-statements.tsv and OUTDIR/write-concurrent-inserts.tsv
# PGBIN: the server's bin directory (default: pg_config --bindir); EXT_SHARE and EXT_LIB: the extension_control_path and dynamic_library_path of a staged build.
# Needs perf (kernel.perf_event_paranoid <= 1). The scratch directory (SCRATCH, default $OUTDIR/scratch) is removed at the end.
set -euo pipefail
OUT=${1:?output directory}; N=${2:-1000000}
PGBIN=${PGBIN:-$(pg_config --bindir)}; export PATH=$PGBIN:$PATH
SCRATCH=${SCRATCH:-$OUT/scratch}; PP=${PPORT:-56191}; SP=${SPORT:-56192}
mkdir -p "$OUT"; rm -rf "$SCRATCH"; mkdir -p "$SCRATCH"
trap 'pg_ctl -D "$SCRATCH/s" -m immediate stop >/dev/null 2>&1 || true; pg_ctl -D "$SCRATCH/p" -m immediate stop >/dev/null 2>&1 || true; rm -rf "$SCRATCH"' EXIT
conf="port = %s
unix_socket_directories = '$SCRATCH'
listen_addresses = ''
shared_buffers = 512MB
max_wal_size = 4GB
fsync = off
synchronous_commit = off
jit = off
hot_standby = on
wal_level = replica
max_wal_senders = 4
autovacuum = off
${EXT_SHARE:+extension_control_path = '$EXT_SHARE:\$system'}
${EXT_LIB:+dynamic_library_path = '$EXT_LIB:\$libdir'}
"
initdb -D "$SCRATCH/p" -U postgres -A trust -E UTF8 --locale=C.UTF-8 >/dev/null
printf "$conf" "$PP" >>"$SCRATCH/p/postgresql.conf"
echo "host replication all samehost trust" >>"$SCRATCH/p/pg_hba.conf"
pg_ctl -D "$SCRATCH/p" -l "$SCRATCH/p.log" -w start >/dev/null
P() { psql -X -q -At -U postgres -h "$SCRATCH" -p "$PP" -d postgres "$@"; }
S() { psql -X -q -At -U postgres -h "$SCRATCH" -p "$SP" -d postgres "$@"; }
P -c "CREATE EXTENSION pg_sift"
pg_basebackup -D "$SCRATCH/s" -U postgres -h "$SCRATCH" -p "$PP" -R -X stream >/dev/null
sed -i '/^port = /d' "$SCRATCH/s/postgresql.conf"; printf "$conf" "$SP" | grep -v "^max_wal_senders" >>"$SCRATCH/s/postgresql.conf"
pg_ctl -D "$SCRATCH/s" -l "$SCRATCH/s.log" -w start >/dev/null
child() { ps --ppid "$(head -1 "$1/postmaster.pid")" -o pid=,args= | awk -v w="$2" '$0 ~ w {print $1; exit}'; }
spid=$(child "$SCRATCH/s" "startup")
caught_up() { local l; l=$(P -c "select pg_current_wal_lsn()"); until [[ $(S -c "select pg_last_wal_replay_lsn() >= '$l'::pg_lsn") == t ]]; do sleep 0.2; done; }
instr() { awk -F, '/instructions/ {printf "%d", $1}' "$1"; }
cols="id bigint primary key, prop int, rdate date, a int, b int, c int, d text"
ddl() { # variant
  P -c "DROP TABLE IF EXISTS t" -c "CREATE TABLE t ($cols)"
  P -c "INSERT INTO t SELECT g, g % 1000, date '2020-01-01' + (g % 1500), g % 97, g % 8191, g % 31, repeat('x', 40) FROM generate_series(1, $N) g"
  case $1 in
    bt) P -c "CREATE INDEX t_pr ON t (prop, rdate)" -c "CREATE INDEX t_r ON t (rdate)" ;;
    sd) P -c "CREATE INDEX t_sift ON t USING sift_col (prop, rdate, a, b, c)" ;;
  esac
  P -c "VACUUM ANALYZE t" -c "CHECKPOINT"; caught_up
}
idxsize() { P -c "select pg_indexes_size('t')"; }
TSV1="$OUT/write-single-statements.tsv"; TSV2="$OUT/write-concurrent-inserts.tsv"
printf 'N\tvariant\tcell\trows\tinstr_backend/row\twal_B/row\tfpi_B/row\twal_rec/row\tindex_growth_B/row\treplay_instr/row\tindex_B_after\tsift_segs_before(n,rows,bytes)\tsift_segs_after\tload1\ttiming\n' >"$TSV1"
printf 'N\tvariant\tclients\trows\tinstr/row\twal_B/row\treplay_instr/row\tload1\ttiming\n' >"$TSV2"
cell() { # variant cell rows file-of-statements
  local v=$1 c=$2 rows=$3 f=$4 d; d=$(mktemp -d "$SCRATCH/c.XXXXXX")
  local l0 w0 f0 i0; read -r l0 w0 f0 < <(P -F' ' -c "select pg_current_wal_lsn(), wal_records, wal_fpi from pg_stat_wal"); i0=$(idxsize)
  perf stat -x, -e instructions:u -p "$spid" -o "$d/rep" & local rp=$!
  { echo '\set ON_ERROR_STOP on'; echo 'select pg_backend_pid() as pid \gset'; echo '\o '"$d/pid"; echo 'select :pid;'; echo '\o'
    echo '\! perf stat -x, -e instructions:u -p $(cat '"$d/pid"') -o '"$d/be"' & echo $! > '"$d/bep"'; sleep 0.3'
    cat "$f"; echo '\! kill -INT $(cat '"$d/bep"'); sleep 0.3'; } >"$d/run.sql"
  P -f "$d/run.sql" >/dev/null
  caught_up; kill -INT $rp; wait $rp 2>/dev/null || true
  P -c "select pg_stat_force_next_flush()" >/dev/null
  local l1 w1 f1 i1; read -r l1 w1 f1 < <(P -F' ' -c "select pg_current_wal_lsn(), wal_records, wal_fpi from pg_stat_wal"); i1=$(idxsize)
  local wal; wal=$(P -c "select ('$l1'::pg_lsn - '$l0'::pg_lsn)::bigint")
  printf '%s\t%s\t%s\t%s\t%d\t%d\t%d\t%.2f\t%.1f\t%d\t%s\t\t\t%s\tinstructions only\n' "$N" "$v" "$c" "$rows" "$(( $(instr "$d/be") / rows ))" "$(( wal / rows ))" "$(( (f1 - f0) * 8192 / rows ))" \
    "$(awk -v a="$w0" -v b="$w1" -v r="$rows" 'BEGIN {print (b - a) / r}')" "$(awk -v a="$i0" -v b="$i1" -v r="$rows" 'BEGIN {print (b - a) / r}')" "$(( $(instr "$d/rep") / rows ))" "$i1" "$(cut -d' ' -f1 /proc/loadavg)" >>"$TSV1"
  rm -rf "$d"
}
gen() { # kind -> file of statements
  local f; f=$(mktemp "$SCRATCH/q.XXXXXX")
  case $1 in
    ins1) awk -v n="$N" 'BEGIN {for (i = 1; i <= 2000; i++) printf "insert into t values (%d, %d, date %s, %d, %d, %d, %s);\n", n + i, i % 1000, "'"'"'2021-01-01'"'"'", i % 97, i % 8191, i % 31, "'"'"'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'"'"'"}' >"$f" ;;
    ins100) awk -v n="$N" 'BEGIN {for (s = 0; s < 40; s++) printf "insert into t select %d + g, g %% 1000, date %s, g %% 97, g %% 8191, g %% 31, repeat(%s, 40) from generate_series(%d, %d) g;\n", n + 2000, "'"'"'2021-01-01'"'"'", "'"'"'x'"'"'", s * 100 + 1, s * 100 + 100}' >"$f" ;;
    copy) echo "copy t from '$SCRATCH/copy.csv' csv;" >"$f" ;;
    upd_hot) awk -v n="$N" 'BEGIN {for (i = 1; i <= 2000; i++) printf "update t set d = repeat(%s, 40) where id = %d;\n", "'"'"'y'"'"'", (i * 487) % n + 1}' >"$f" ;;
    upd_cov) awk -v n="$N" 'BEGIN {for (i = 1; i <= 2000; i++) printf "update t set c = c + 1 where id = %d;\n", (i * 487) % n + 1}' >"$f" ;;
    upd_key) awk -v n="$N" 'BEGIN {for (i = 1; i <= 2000; i++) printf "update t set prop = prop + 1, rdate = rdate + 1 where id = %d;\n", (i * 487) % n + 1}' >"$f" ;;
    del) awk -v n="$N" 'BEGIN {for (i = 1; i <= 2000; i++) printf "delete from t where id = %d;\n", (i * 487) % n + 1}' >"$f" ;;
    vacuum) echo "vacuum t;" >"$f" ;;
  esac
  echo "$f"
}
P -c "copy (select g + $N + 100000, g % 1000, date '2021-01-01', g % 97, g % 8191, g % 31, repeat('x', 40) from generate_series(1, 50000) g) to '$SCRATCH/copy.csv' csv"
for v in pk bt sd; do
  ddl "$v"
  cell "$v" ins1 2000 "$(gen ins1)"
  cell "$v" ins100 4000 "$(gen ins100)"
  cell "$v" copy 50000 "$(gen copy)"
  cell "$v" upd_hot 2000 "$(gen upd_hot)"
  cell "$v" upd_cov 2000 "$(gen upd_cov)"
  cell "$v" upd_key 2000 "$(gen upd_key)"
  cell "$v" del 2000 "$(gen del)"
  cell "$v" vacuum 8000 "$(gen vacuum)"
  for c in 1 4 16; do
    printf '\\set n :n + 1\n\\set id :base + :client_id * 10000000 + :n\ninsert into t values (:id, :id %% 1000, date %s, :id %% 97, :id %% 8191, :id %% 31, repeat(%s, 40));\n' "'2021-01-01'" "'x'" >"$SCRATCH/w.pgb"
    caught_up
    l0=$(P -c "select pg_current_wal_lsn()")
    perf stat -x, -e instructions:u -p "$spid" -o "$SCRATCH/rep" & rp=$!
    tx=$(pgbench -n -T 20 -D n=0 -D base=$(( N * 4 + c * 400000000 )) -c "$c" -j "$(( c > 3 ? 3 : c ))" -f "$SCRATCH/w.pgb" -h "$SCRATCH" -p "$PP" -U postgres postgres | grep -oE 'processed: [0-9]+' | grep -oE '[0-9]+')
    caught_up; kill -INT $rp; wait $rp 2>/dev/null || true
    wal=$(P -c "select (pg_current_wal_lsn() - '$l0'::pg_lsn)::bigint")
    printf '%s\t%s\t%s\t%s\t%s\t%d\t%d\t%s\tinstructions only\n' "$N" "$v" "$c" "$tx" n/a "$(( wal / tx ))" "$(( $(instr "$SCRATCH/rep") / tx ))" "$(cut -d' ' -f1 /proc/loadavg)" >>"$TSV2"
  done
done
echo "wrote $TSV1 and $TSV2" >&2
